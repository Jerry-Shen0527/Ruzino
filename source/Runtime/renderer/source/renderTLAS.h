#pragma once
#include "Lights/EmissiveMeshRegistry.h"
#include "api.h"
#include "geometries/mesh.h"
#include "nvrhi/nvrhi.h"
#include "sky/hosek_sky_model.h"

// SceneTypes
#include "../nodes/shaders/Scene/BindlessMaterial.slang"
#include "../nodes/shaders/Scene/Lights/LightData.slang"
#include "../nodes/shaders/Scene/SceneTypes.slang"
#include "internal/memory/DeviceMemoryPool.hpp"
#include "internal/memory/SceneArray.hpp"

RUZINO_NAMESPACE_OPEN_SCOPE

class HD_RUZINO_API Hd_RUZINO_RenderInstanceCollection {
   public:
    explicit Hd_RUZINO_RenderInstanceCollection();
    ~Hd_RUZINO_RenderInstanceCollection();

    nvrhi::rt::IAccelStruct* get_tlas();
    DescriptorTableManager* get_buffer_descriptor_table() const
    {
        return bindlessData.bufferDescriptorTableManager.get();
    }
    DescriptorTableManager* get_texture_descriptor_table() const
    {
        return bindlessData.textureDescriptorTableManager.get();
    }

    // DeviceMemoryPool<unsigned> index_pool;
    // DeviceMemoryPool<float> vertex_pool;
    DeviceMemoryPool<GeometryInstanceData> instance_pool;
    DeviceMemoryPool<nvrhi::rt::InstanceDesc> rt_instance_pool;
    DeviceMemoryPool<MeshDesc> mesh_pool;
    DeviceMemoryPool<VolumeDesc> volume_pool;
    DeviceMemoryPool<MaterialDataBlob> material_pool;
    DeviceMemoryPool<MaterialHeader> material_header_pool;
    // Draw-indirect args table. Pure leaf (nothing references an args
    // slot — args are self-contained; startInstanceLocation points INTO
    // instance_pool, which is a stable-slot arena), so it uses the dense
    // output layout: flush() re-packs live args contiguously and
    // ExecuteIndirect reads them from offset 0. Replaces
    // DeviceMemoryPool<DrawIndirectArguments> draw_indirect_pool + its
    // compress() call in the rasterize node.
    SceneArray<nvrhi::DrawIndirectArguments, true> draw_indirects{
        nvrhi::BufferDesc{}.setIsDrawIndirectArgs(true)
    };
    // Light table. CPU-authoritative SceneArray: data lives in stable
    // slots (so GeometryInstanceData.geometryID = handle->slot() stays
    // valid forever), dense iteration goes through the indices buffer
    // (lightIndices[i]), removal is swap-remove on the index list. This
    // replaces DeviceMemoryPool<LightData> light_pool, whose compress()
    // both moved data (breaking geometryID references) and was required
    // before every dense read (holes otherwise showed stale lights).
    SceneArray<LightData> scene_lights;
    // Per-Hosek-dome-light cooked sky state (CPU-cooked turbidity/albedo/
    // elevation -> 30 floats). Indexed by LightData.hosekStateIndex. Row 0 is
    // reserved as a zeroed dummy ("hosekStateIndex = 0" = no Hosek sky). The
    // reservation is LAZY (first cook in light.cpp / first buffer bind in a
    // render node) — allocating in the constructor crashed the raster path.
    DeviceMemoryPool<ruzino::HosekSkyState> hosek_state_pool;
    // Keeps the reserved dummy row 0 alive so no cooked dome can land on it.
    DeviceMemoryPool<ruzino::HosekSkyState>::MemoryHandle hosek_dummy_row;

    /// Emissive mesh light registry: collects emissive triangles from
    /// Hd_RUZINO_Mesh::Sync() registrations and produces GPU buffers for NEE
    /// sampling + BSDF-hit-emissive MIS. Lives here alongside the other pools.
    EmissiveMeshRegistry emissive_registry;

    CommandListHandle command_list;

    struct BindlessData {
        BindlessData();
        std::shared_ptr<DescriptorTableManager> bufferDescriptorTableManager;
        std::shared_ptr<DescriptorTableManager> textureDescriptorTableManager;

        nvrhi::BindingLayoutHandle bufferBindlessLayout;
        nvrhi::BindingLayoutHandle textureBindlessLayout;
    };
    BindlessData bindlessData;

    void set_require_rebuild_tlas()
    {
        require_rebuild_tlas = true;
    }

    bool get_require_rebuild_tlas() const
    {
        return require_rebuild_tlas;
    }

    // Track material changes
    uint32_t material_version = 0;
    void mark_materials_dirty()
    {
        material_version++;
    }
    uint32_t get_material_version() const
    {
        return material_version;
    }

    // Track geometry/buffer changes
    uint32_t geometry_version = 0;
    void mark_geometry_dirty()
    {
        geometry_version++;
    }
    uint32_t get_geometry_version() const
    {
        return geometry_version;
    }

    // track light changes
    uint32_t light_version = 0;
    void mark_lights_dirty()
    {
        light_version++;
    }
    uint32_t get_light_version() const
    {
        return light_version;
    }

   private:
    nvrhi::rt::AccelStructHandle TLAS;
    // Dense staging for the TLAS build input (gathered live instance
    // descs; see rebuild_tlas).
    nvrhi::BufferHandle tlas_instance_staging;

    bool require_rebuild_tlas = true;
    void rebuild_tlas();
};

RUZINO_NAMESPACE_CLOSE_SCOPE
