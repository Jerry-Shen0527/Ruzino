//
// Point/Sphere Geometry Support for Ray Tracing
//
#ifndef Hd_RUZINO_POINTS_H
#define Hd_RUZINO_POINTS_H

#include <string>

#include "../DescriptorTableManager.h"
#include "../api.h"
#include "internal/memory/DeviceMemoryPool.hpp"
#include "nvrhi/nvrhi.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/imaging/hd/points.h"
#include "pxr/pxr.h"
// SceneTypes
#include "../nodes/shaders/Scene/SceneTypes.slang"

RUZINO_NAMESPACE_OPEN_SCOPE
class Hd_RUZINO_RenderParam;
using namespace pxr;

class HD_RUZINO_API Hd_RUZINO_Points final : public HdPoints {
   public:
    HF_MALLOC_TAG_NEW("new Hd_RUZINO_Points");

    Hd_RUZINO_Points(const SdfPath& id);
    ~Hd_RUZINO_Points() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;
    void Sync(
        HdSceneDelegate* sceneDelegate,
        HdRenderParam* renderParam,
        HdDirtyBits* dirtyBits,
        const TfToken& reprToken) override;

    void Finalize(HdRenderParam* renderParam) override;

    // Debug zero-copy refresh, driven OUTSIDE Hydra's SyncAll by
    // Hd_RUZINO_Renderer::Render each frame (it polls the registry versions,
    // like the wetbrush_paint_field poll). Self-dirtying from inside Sync
    // (MarkRprimDirty during SyncAll) corrupted the sync pass — uniform
    // single-pixel black speckle across the frame — so per-frame rebuilds
    // must not ride Hydra's dirty propagation.
    static void refresh_debug_prims(Hd_RUZINO_RenderParam* render_param);

    nvrhi::rt::AccelStructHandle BLAS;
    CommandListHandle command_list;

   protected:
    nvrhi::BufferHandle vertexBuffer;
    DescriptorHandle descriptor_handle;
    CommandListHandle copy_commandlist;

    DeviceMemoryPool<GeometryInstanceData>::MemoryHandle instanceBuffer;
    DeviceMemoryPool<nvrhi::rt::InstanceDesc>::MemoryHandle rt_instanceBuffer;
    DeviceMemoryPool<MeshDesc>::MemoryHandle mesh_desc_buffer;

    GfMatrix4f transform;
    VtArray<GfVec3f> points;
    VtFloatArray widths;
    // CPU path: primvars:displayColor, one RGB per point. Uploaded as a third
    // block after the radii; hit groups shade with it directly when present
    // (MeshFlags::HasPerPointColor).
    VtVec3fArray colors;

    // Debug zero-copy registry mode (render_wetbrush_debug.py): the prim's
    // "debugKey" primvar names a SharedGPUBufferRegistry entry packed each
    // frame by brush_wb_commit (debug_pack_*.slang). We never upload point
    // data; the BLAS is built straight from the shared GPU buffer. Layout
    // (blocks spaced by capacity, see DebugDrawMeta in node_brush_wb_commit):
    //   points:   [C pos float3][C radius float][C rgb float3]
    //   segments: [C A float3][C B float3][C radius float][C rgb float3]
    std::string debug_key;
    uint64_t debug_registry_version = 0;
    nvrhi::BufferHandle debug_buffer;  // keeps the shared buffer alive
    nvrhi::IBuffer* debug_descriptor_buffer = nullptr;  // last buffer a
                                                        // bindless descriptor
                                                        // was created for
    uint32_t debug_count = 0;
    uint32_t debug_capacity = 0;
    bool debug_segments = false;

    void create_gpu_resources(Hd_RUZINO_RenderParam* render_param);
    void create_gpu_resources_registry(Hd_RUZINO_RenderParam* render_param);
    void updateTLAS(
        Hd_RUZINO_RenderParam* render_param,
        HdSceneDelegate* sceneDelegate,
        HdDirtyBits* dirtyBits);

    void _InitRepr(const TfToken& reprToken, HdDirtyBits* dirtyBits) override;
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override;

    // This class does not support copying.
    Hd_RUZINO_Points(const Hd_RUZINO_Points&) = delete;
    Hd_RUZINO_Points& operator=(const Hd_RUZINO_Points&) = delete;

   private:
    bool _pointsValid;

    // GPU-computed AABB buffer (needs to be kept alive)
    nvrhi::BufferHandle aabbBuffer;

    struct PrimvarSource {
        VtValue data;
        HdInterpolation interpolation;
    };

    TfHashMap<TfToken, PrimvarSource, TfToken::HashFunctor> _primvarSourceMap;
};

RUZINO_NAMESPACE_CLOSE_SCOPE

#endif  // Hd_RUZINO_POINTS_H
