// GridVolumeImpl implementation.
//
// Extracted from the old Hd_RUZINO_WetbrushVolume non-cloud half: the grid
// metadata parsing, the two-phase registry/primvar density buffer build, and
// the VolumeDesc fill. The shared AABB/BLAS + TLAS machinery now lives in
// Hd_RUZINO_Volume (the shell), so this file is purely the grid-volume
// resource + data work. The registry contract is the generic
// Ruzino::GridFieldDesc (RHI/grid_field_desc.hpp), shared with the producer.
#include "grid_volume_impl.h"

#include <spdlog/spdlog.h>

#include <mutex>

#include "../../hydra2Ingest.h"
#include "RHI/grid_field_desc.hpp"
#include "RHI/rhi.hpp"
#include "RHI/shared_buffer_registry.hpp"
#include "nvrhi/nvrhi.h"
#include "renderParam.h"
#include "renderTLAS.h"

RUZINO_NAMESPACE_OPEN_SCOPE
using namespace pxr;

GfVec3f GridVolumeImpl::boundsMin() const
{
    return gridMin;
}

GfVec3f GridVolumeImpl::boundsMax() const
{
    return GfVec3f(
        gridMin[0] + spacingX * static_cast<float>(gridResX),
        gridMin[1] + spacingY * static_cast<float>(gridResY),
        gridMin[2] + spacingZ * static_cast<float>(gridResZ));
}

bool GridVolumeImpl::parsePrimvars(
    HdSceneDelegate* sceneDelegate,
    const SdfPath& id,
    HdDirtyBits* dirtyBits)
{
    bool update_gpu_resources = false;

    // 3D grid metadata primvars (authored by the bake stage, e.g.
    // render_wetbrush.py, or by HydraRenderer.save_volume_checkpoint).
    // Hydra 2.0 direct read with legacy fallback (shared helper). Spacing is
    // per-axis when cellSizeX/Y/Z are authored (checkpoint path, mirroring
    // the per-axis registry GridFieldDesc); scalar cellSize alone means the
    // legacy isotropic bake layout.
    auto readPrimvar = [&](const char* name) -> VtValue {
        VtValue v;
        if (!Ruzino_Hydra2::ReadPrimvar(sceneDelegate, id, TfToken(name), &v)) {
            v = sceneDelegate->Get(id, TfToken(name));
        }
        return v;
    };
    VtValue rx_v = readPrimvar("gridResX");
    VtValue ry_v = readPrimvar("gridResY");
    VtValue rz_v = readPrimvar("gridResZ");
    VtValue cs_v = readPrimvar("cellSize");
    VtValue csx_v = readPrimvar("cellSizeX");
    VtValue csy_v = readPrimvar("cellSizeY");
    VtValue csz_v = readPrimvar("cellSizeZ");
    VtValue gm_v = readPrimvar("gridMin");
    VtValue paint_v = readPrimvar("paintField");

    auto read_uint = [](const VtValue& v) -> uint32_t {
        if (v.IsHolding<int>())
            return static_cast<uint32_t>(v.UncheckedGet<int>());
        if (v.IsHolding<uint>())
            return static_cast<uint32_t>(v.UncheckedGet<uint>());
        return 0;
    };
    auto read_float = [](const VtValue& v) -> float {
        return v.IsHolding<float>() ? v.UncheckedGet<float>() : 0.0f;
    };
    uint32_t new_rx = read_uint(rx_v);
    uint32_t new_ry = read_uint(ry_v);
    uint32_t new_rz = read_uint(rz_v);
    // Per-axis spacing when authored (the checkpoint path writes
    // cellSizeX/Y/Z mirroring the per-axis GridFieldDesc; the registry is
    // anisotropic-capable, e.g. a thin Z stack). Fall back to the scalar
    // cellSize of the legacy bake path (isotropic cells) per axis.
    float new_csx = read_float(csx_v);
    float new_csy = read_float(csy_v);
    float new_csz = read_float(csz_v);
    const float new_cs = read_float(cs_v);
    if (new_csx == 0.0f)
        new_csx = new_cs;
    if (new_csy == 0.0f)
        new_csy = new_cs;
    if (new_csz == 0.0f)
        new_csz = new_cs;
    GfVec3f new_gm = gm_v.IsHolding<GfVec3f>() ? gm_v.UncheckedGet<GfVec3f>()
                                               : GfVec3f(0.0f);
    if (new_rx != gridResX || new_ry != gridResY || new_rz != gridResZ ||
        new_csx != spacingX || new_csy != spacingY || new_csz != spacingZ ||
        new_gm != gridMin) {
        gridResX = new_rx;
        gridResY = new_ry;
        gridResZ = new_rz;
        spacingX = new_csx;
        spacingY = new_csy;
        spacingZ = new_csz;
        gridMin = new_gm;
        update_gpu_resources = true;
    }

    // Check if the shared-registry buffer has a new version (a producer
    // updated the grid field this frame). The bindless SRV auto-tracks
    // content changes, so we only need to re-run buildDensityResource when
    // the version bumps (buffer handle may have changed).
    nvrhi::BufferHandle _dummy;
    size_t _dummy_bytes;
    uint64_t reg_ver;
    if (SharedGPUBufferRegistry::get().lookup(
            Ruzino::kGridFieldPaintKey, _dummy, _dummy_bytes, reg_ver) &&
        reg_ver != registryVersion) {
        update_gpu_resources = true;
    }

    if (!paint_v.IsEmpty() && paint_v.IsHolding<VtVec4fArray>()) {
        const auto& pf = paint_v.UncheckedGet<VtVec4fArray>();
        paintField.assign(pf.begin(), pf.end());
        update_gpu_resources = true;
        spdlog::info(
            "GridVolumeImpl {}: loaded 3D paint field ({} voxels, "
            "{}x{}x{})",
            id.GetText(),
            paintField.size(),
            gridResX,
            gridResY,
            gridResZ);
    }

    return update_gpu_resources;
}

bool GridVolumeImpl::buildDensityResource(Hd_RUZINO_RenderParam* render_param)
{
    auto device = RHI::get_device();

    size_t cell_count = static_cast<size_t>(gridResX) * gridResY * gridResZ;

    // ------------------------------------------------------------------
    // Phase 1: check the shared GPU buffer registry for a zero-copy buffer
    // produced by a simulation. If hit, use it directly (skip createBuffer +
    // writeBuffer entirely). The metadata blob is a Ruzino::GridFieldDesc
    // (shared header RHI/grid_field_desc.hpp — both sides include it, no
    // out-of-band POD agreement).
    //
    // Requirements (all met on the producer side):
    //   - packed_paint is created with CanHaveRawViews (matches the
    //     RawBuffer_SRV binding below) — see node_brush_wb_sim.cpp.
    //   - commit flushes the pack dispatch + transitions the buffer to
    //     ShaderResource before registering — see node_brush_wb_commit.cpp.
    // ------------------------------------------------------------------
    nvrhi::BufferHandle ext_buf;
    size_t ext_bytes = 0;
    uint64_t ext_version = 0;
    const void* ext_meta_ptr = nullptr;
    size_t ext_meta_bytes = 0;
    bool registry_hit = SharedGPUBufferRegistry::get().lookup(
        Ruzino::kGridFieldPaintKey,
        ext_buf,
        ext_bytes,
        ext_version,
        &ext_meta_ptr,
        &ext_meta_bytes);

    if (registry_hit && ext_buf &&
        ext_meta_bytes >= sizeof(Ruzino::GridFieldDesc)) {
        auto* meta = static_cast<const Ruzino::GridFieldDesc*>(ext_meta_ptr);
        if (meta->format != Ruzino::GridFieldFormat::DensityColorRGBA) {
            spdlog::warn(
                "GridVolumeImpl: registry hit has unsupported format {} "
                "(expected DensityColorRGBA) -- skipping",
                static_cast<uint32_t>(meta->format));
            return false;
        }
        gridResX = meta->resX;
        gridResY = meta->resY;
        gridResZ = meta->resZ;
        spacingX = meta->spacingX;
        spacingY = meta->spacingY;
        spacingZ = meta->spacingZ;
        gridMin = GfVec3f(meta->originX, meta->originY, meta->originZ);
        cell_count = static_cast<size_t>(gridResX) * gridResY * gridResZ;

        // Use the external buffer. Only (re)register the bindless descriptor
        // when the buffer handle or version changes (first time, or the
        // producer reallocated the buffer).
        if (!densityDescriptorHandle.Get() ||
            densityBuffer.Get() != ext_buf.Get() ||
            registryVersion != ext_version) {
            densityBuffer = ext_buf;
            registryVersion = ext_version;
            auto descriptor_table =
                render_param->InstanceCollection->get_buffer_descriptor_table();
            densityDescriptorHandle = descriptor_table->CreateDescriptorHandle(
                nvrhi::BindingSetItem::RawBuffer_SRV(0, densityBuffer));
            spdlog::info(
                "GridVolumeImpl: registry hit (v{}, {}x{}x{}, {} bytes, "
                "bindless={})",
                ext_version,
                gridResX,
                gridResY,
                gridResZ,
                ext_bytes,
                densityDescriptorHandle.Get());
        }
    }
    else if (cell_count == 0 || paintField.size() != cell_count) {
        // Phase 2 fallback requires primvar data — if it's missing/empty, skip.
        spdlog::warn(
            "GridVolumeImpl: no registry buffer and paint field mismatch "
            "({}x{}x{} voxels, {} entries) -- skipping GPU resources",
            gridResX,
            gridResY,
            gridResZ,
            paintField.size());
        return false;
    }
    else {
        // ------------------------------------------------------------------
        // Phase 2 fallback: create our own buffer from the primvar data
        // (Float4 per voxel: density, r, g, b).
        // ------------------------------------------------------------------
        const size_t density_bytes = cell_count * sizeof(GfVec4f);
        if (!densityBuffer ||
            densityBuffer->getDesc().byteSize != density_bytes) {
            nvrhi::BufferDesc desc =
                nvrhi::BufferDesc{}
                    .setCanHaveRawViews(true)
                    .setByteSize(density_bytes)
                    .setStructStride(sizeof(GfVec4f))
                    .setInitialState(nvrhi::ResourceStates::ShaderResource)
                    .setCpuAccess(nvrhi::CpuAccessMode::None)
                    .setKeepInitialState(true)
                    .setDebugName("gridVolumeDensityBuffer");
            densityBuffer = device->createBuffer(desc);

            auto descriptor_table =
                render_param->InstanceCollection->get_buffer_descriptor_table();
            densityDescriptorHandle = descriptor_table->CreateDescriptorHandle(
                nvrhi::BindingSetItem::RawBuffer_SRV(0, densityBuffer));
        }

        // Upload the per-voxel (density, r, g, b) data.
        auto copy_commandlist = device->createCommandList();
        copy_commandlist->open();
        copy_commandlist->writeBuffer(
            densityBuffer, paintField.data(), density_bytes);
        copy_commandlist->close();
        {
            std::lock_guard lock(execution_launch_mutex);
            device->executeCommandList(copy_commandlist);
        }
    }

    bindlessIndex_ = densityDescriptorHandle.Get();

    spdlog::info(
        "GridVolumeImpl: density buffer ready ({} cells, bindless={}), "
        "slab AABB [({:.3f},{:.3f},{:.3f})..({:.3f},{:.3f},{:.3f})]",
        cell_count,
        bindlessIndex_,
        boundsMin()[0],
        boundsMin()[1],
        boundsMin()[2],
        boundsMax()[0],
        boundsMax()[1],
        boundsMax()[2]);

    return true;
}

void GridVolumeImpl::fillVolumeDesc(VolumeDesc& vd) const
{
    vd.bindlessIndex = bindlessIndex_;
    vd.gridResX = gridResX;
    vd.gridResY = gridResY;
    vd.gridResZ = gridResZ;
    vd.cellSize = float3(spacingX, spacingY, spacingZ);
    vd.gridMin = float3(gridMin[0], gridMin[1], gridMin[2]);
    vd.volumeKind = uint32_t(VolumeKind::GridVolume);  // paint density slab
    // Cloud fields are unused for grid volumes; zero them for determinism.
    vd.coverage = 0.0f;
    vd.densityScale = 0.0f;
    vd.phaseG = 0.0f;
    vd.layerTop = 0.0f;
    vd.layerBottom = 0.0f;
    vd.noiseFreq = float3(0.0f);
    vd.worleyFreq = float3(0.0f);
    vd.detailErosion = 0.0f;
    vd._cloudPad0 = 0.0f;
    vd._cloudPad1 = 0.0f;
    vd._cloudPad2 = float2(0.0f, 0.0f);
}

RUZINO_NAMESPACE_CLOSE_SCOPE
