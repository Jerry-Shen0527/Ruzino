//
// Point/Sphere Geometry Support for Ray Tracing
//
#ifndef Hd_RUZINO_POINTS_H
#define Hd_RUZINO_POINTS_H

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

    void create_gpu_resources(Hd_RUZINO_RenderParam* render_param);
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
