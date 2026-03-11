#pragma once
#include "RHI/ResourceManager/resource_allocator.hpp"
#include "api.h"
#include "pxr/usd/sdf/path.h"

namespace Ruzino {
class Hd_RUZINO_RenderInstanceCollection;
class LensSystem;
class Hd_RUZINO_Light;
class Hd_RUZINO_Camera;
class Hd_RUZINO_Mesh;
class Hd_RUZINO_Material;
}  // namespace Ruzino

namespace Ruzino {

struct HD_RUZINO_API RenderGlobalPayload {
    RenderGlobalPayload();

    RenderGlobalPayload(
        std::vector<Hd_RUZINO_Camera*>* cameras,
        std::vector<Hd_RUZINO_Light*>* lights,
        pxr::TfHashMap<pxr::SdfPath, Hd_RUZINO_Material*, pxr::TfHash>*
            materials,
        nvrhi::IDevice* nvrhi_device);

    RenderGlobalPayload(const RenderGlobalPayload& rhs);

    RenderGlobalPayload& operator=(const RenderGlobalPayload& rhs);

    ResourceAllocator resource_allocator;
    ShaderFactory shader_factory;
    nvrhi::IDevice* nvrhi_device;
    Hd_RUZINO_RenderInstanceCollection* InstanceCollection;
    bool reset_accumulation = false;

    // Scene dirty flags for tracking changes
    enum class SceneDirtyBits : uint32_t {
        Clean = 0,
        DirtyMaterials = 1 << 0,  // Material shaders changed
        DirtyGeometry = 1 << 1,   // Mesh topology/vertices changed
        DirtyLights = 1 << 2,     // Light count or properties changed
        DirtyAll = 0xFFFFFFFF
    };

    uint32_t scene_dirty_flags =
        static_cast<uint32_t>(SceneDirtyBits::DirtyAll);

    void mark_dirty(SceneDirtyBits bits)
    {
        scene_dirty_flags |= static_cast<uint32_t>(bits);
    }

    void clear_dirty(SceneDirtyBits bits)
    {
        scene_dirty_flags &= ~static_cast<uint32_t>(bits);
    }

    bool is_dirty(SceneDirtyBits bits) const
    {
        return (scene_dirty_flags & static_cast<uint32_t>(bits)) != 0;
    }

    void clear_all_dirty()
    {
        scene_dirty_flags = static_cast<uint32_t>(SceneDirtyBits::Clean);
    }

    auto& get_cameras() const
    {
        return *cameras;
    }

    auto& get_lights() const
    {
        return *lights;
    }

    auto& get_materials() const
    {
        return *materials;
    }

    LensSystem* lens_system;

   private:
    std::vector<Hd_RUZINO_Camera*>* cameras;
    std::vector<Hd_RUZINO_Light*>* lights;
    pxr::TfHashMap<pxr::SdfPath, Hd_RUZINO_Material*, pxr::TfHash>* materials;
};

}  // namespace Ruzino
