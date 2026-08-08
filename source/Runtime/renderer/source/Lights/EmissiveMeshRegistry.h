#pragma once

#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "api.h"
#include "internal/memory/DeviceMemoryPool.hpp"
#include "nvrhi/nvrhi.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec3i.h"
#include "pxr/base/tf/hash.h"
#include "pxr/usd/sdf/path.h"

// SceneTypes (PackedEmissiveTriangle / EmissiveFlux / MeshLightData) -- shared
// host/device slang headers. These define their own RUZINO_NAMESPACE_OPEN_SCOPE
// / CLOSE_SCOPE, so they must be included OUTSIDE any open namespace.
#include "../../nodes/shaders/Scene/Lights/LightCollectionShared.slang"
#include "../../nodes/shaders/Scene/Lights/MeshLightData.slang"

RUZINO_NAMESPACE_OPEN_SCOPE
using namespace pxr;

class Hd_RUZINO_Material;

/// Per-mesh emissive metadata. This is the LIGHTWEIGHT entry registered by
/// Hd_RUZINO_Mesh::Sync() — it carries only the information needed to tell the
/// GPU compute pass WHERE to find this mesh's triangles, not the triangle data
/// itself. The actual vertex/index data is read directly from the mesh's
/// existing bindless vertexBuffer on the GPU (zero CPU→GPU copy).
struct EmissiveMeshEntry {
    uint32_t instanceSlot =
        0xffffffffu;  ///< instance_pool index (== InstanceID())
    uint32_t meshDescIndex =
        0xffffffffu;  ///< mesh_pool index (== GeometryInstanceData.geometryID)
    uint32_t triangleCount = 0;  ///< Number of triangles in this mesh
    uint32_t defaultMaterialLocation =
        0xffffffffu;  ///< material_header_pool index
};

/// Collects emissive mesh metadata and dispatches a GPU compute pass to build
/// the flat emissive triangle buffers. Lives on
/// Hd_RUZINO_RenderInstanceCollection.
class HD_RUZINO_API EmissiveMeshRegistry {
   public:
    EmissiveMeshRegistry() = default;
    ~EmissiveMeshRegistry() = default;

    void register_mesh(uint32_t instanceSlot, EmissiveMeshEntry entry)
    {
        entries_[instanceSlot] = std::move(entry);
        version_.fetch_add(1, std::memory_order_release);
    }

    void deregister_mesh(uint32_t instanceSlot)
    {
        entries_.erase(instanceSlot);
        version_.fetch_add(1, std::memory_order_release);
    }

    /// Build flat GPU buffers via a compute pass. The CPU side flattens the
    /// lightweight entries into a "mesh work list" (instanceSlot/meshDescIndex/
    /// triangleCount/materialLocation per mesh), then dispatches a GPU compute
    /// shader that reads each mesh's bindless vertexBuffer to compute world-
    /// space positions, normals, area, and packs them into
    /// PackedEmissiveTriangle. Flux is computed from emission radiance
    /// (CPU-provided per mesh) * area.
    ///
    /// `collection` provides access to instance_pool / mesh_pool / bindless
    /// descriptor table for the GPU compute pass. `materials` provides the
    /// MaterialMap for isEmissive() / getEmissionRadiance() filtering.
    ///
    /// Returns false if no emissive triangles exist.
    bool build_gpu_buffers(
        class Hd_RUZINO_RenderInstanceCollection* collection,
        pxr::TfHashMap<pxr::SdfPath, Hd_RUZINO_Material*, pxr::TfHash>*
            materials);

    uint32_t get_version() const
    {
        return version_.load(std::memory_order_acquire);
    }

    uint32_t get_triangle_count() const
    {
        return triangleCount_;
    }
    uint32_t get_mesh_count() const
    {
        return meshCount_;
    }

    DeviceMemoryPool<PackedEmissiveTriangle> emissiveTrianglePool;
    DeviceMemoryPool<EmissiveFlux> emissiveFluxPool;
    DeviceMemoryPool<MeshLightData> emissiveMeshPool;
    DeviceMemoryPool<uint32_t> emissivePerInstanceOffsetPool;

    DeviceMemoryPool<PackedEmissiveTriangle>::MemoryHandle triHandle;
    DeviceMemoryPool<EmissiveFlux>::MemoryHandle fluxHandle;
    DeviceMemoryPool<MeshLightData>::MemoryHandle meshHandle;
    DeviceMemoryPool<uint32_t>::MemoryHandle offsetHandle;

   private:
    std::unordered_map<uint32_t, EmissiveMeshEntry> entries_;
    std::atomic<uint32_t> version_{ 0 };
    uint32_t triangleCount_ = 0;
    uint32_t meshCount_ = 0;
};

RUZINO_NAMESPACE_CLOSE_SCOPE
