#pragma once
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdLux/sphereLight.h>

#include "pxr/usd/usdGeom/cube.h"
#include "pxr/usd/usdGeom/cylinder.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/sphere.h"
#include "pxr/usd/usdGeom/xform.h"
#include "pxr/usd/usdLux/diskLight.h"
#include "pxr/usd/usdLux/distantLight.h"
#include "pxr/usd/usdLux/domeLight.h"
#include "pxr/usd/usdLux/rectLight.h"
#include "pxr/usd/usdShade/material.h"
#include "stage/api.h"
#include "stage/ecs_systems.hpp"

RUZINO_NAMESPACE_OPEN_SCOPE
namespace animation {
class WithDynamicLogicPrim;
}

class STAGE_API Stage {
   public:
    Stage();
    ~Stage();
    // Add a new initializer for custom stage file
    Stage(const std::string& stage_path);

    bool should_simulate() const
    {
        return render_time_code >= current_time_code;
    }

    void tick(float ellapsed_time);
    void finish_tick();

    pxr::UsdTimeCode get_current_time();
    void set_current_time(pxr::UsdTimeCode time);

    pxr::UsdTimeCode get_render_time();
    void set_render_time(pxr::UsdTimeCode time);

    pxr::UsdPrim add_prim(const pxr::SdfPath& path);
    pxr::UsdShadeMaterial create_material(const pxr::SdfPath& path);

    pxr::UsdGeomSphere create_sphere(
        const pxr::SdfPath& path = pxr::SdfPath::EmptyPath());
    pxr::UsdGeomCylinder create_cylinder(
        const pxr::SdfPath& path = pxr::SdfPath::EmptyPath());
    pxr::UsdGeomCube create_cube(
        const pxr::SdfPath& path = pxr::SdfPath::EmptyPath());
    pxr::UsdGeomXform create_xform(
        const pxr::SdfPath& path = pxr::SdfPath::EmptyPath());
    pxr::UsdGeomMesh create_mesh(
        const pxr::SdfPath& path = pxr::SdfPath::EmptyPath());

    pxr::UsdLuxRectLight create_rect_light(
        const pxr::SdfPath& path = pxr::SdfPath::EmptyPath()) const;

    pxr::UsdLuxDistantLight create_distant_light(
        const pxr::SdfPath& path = pxr::SdfPath::EmptyPath()) const;

    pxr::UsdLuxDiskLight create_disk_light(
        const pxr::SdfPath& path = pxr::SdfPath::EmptyPath()) const;

    pxr::UsdLuxDomeLight create_dome_light(
        const pxr::SdfPath& path = pxr::SdfPath::EmptyPath()) const;

    pxr::UsdLuxSphereLight create_sphere_light(const pxr::SdfPath& path) const
    {
        return create_prim<pxr::UsdLuxSphereLight>(path, "sphere_light");
    }

    void remove_prim(const pxr::SdfPath& path);

    [[nodiscard]] std::string stage_content() const;

    [[nodiscard]] pxr::UsdStageRefPtr get_usd_stage() const;

    void create_editor_at_path(const pxr::SdfPath& sdf_path);
    bool consume_editor_creation(
        pxr::SdfPath& json_path,
        bool fully_consume = true);
    void save_string_to_usd(const pxr::SdfPath& path, const std::string& data);
    std::string load_string_from_usd(const pxr::SdfPath& path);
    void import_usd_as_payload(
        const std::string& path_string,
        const pxr::SdfPath& sdf_path);

    void import_usd_as_reference(
        const std::string& path_string,
        const pxr::SdfPath& sdf_path);

    void import_materialx(
        const std::string& path_string,
        const pxr::SdfPath& sdf_path);
    const std::string& GetStagePath()
    {
        return m_stage_path;
    }

    // File operations
    void Save();
    void SaveAs(const std::string& new_path);
    bool OpenStage(const std::string& path);

    // ========================================================================
    // Modifier Layer (Persistent Sidecar File)
    // ========================================================================
    pxr::SdfLayerHandle get_modifier_layer();
    std::string get_modifier_layer_path() const;
    void save_modifier_layer();
    void load_modifier_layer();

    // ========================================================================
    // ECS Interface
    // ========================================================================

    // Get ECS registry
    entt::registry& get_registry()
    {
        return registry_;
    }
    const entt::registry& get_registry() const
    {
        return registry_;
    }

    // Get USD prim from entity
    pxr::UsdPrim get_prim_from_entity(entt::entity entity);

    // Find entity by SdfPath
    entt::entity find_entity_by_path(const pxr::SdfPath& path);

    // Sync all entities to USD
    void sync_entities_to_usd();

    // Load all prims from USD to ECS
    void load_prims_to_ecs();

    // Traverse stage and return prim information for debugging
    std::string traverse_stage(int max_depth = -1) const;

    // Get systems
    ecs::AnimationSystem* get_animation_system()
    {
        return animation_system_.get();
    }
    ecs::UsdSyncSystem* get_usd_sync_system()
    {
        return usd_sync_system_.get();
    }
    ecs::PhysicsSystem* get_physics_system()
    {
        return physics_system_.get();
    }

    // Get stage listener
    class StageListener* get_stage_listener()
    {
        return stage_listener_.get();
    }

    bool save_on_destruct = true;

   private:
    // Get prim time info (legacy animation system, internal use)
    bool get_prim_time_info(
        const pxr::SdfPath& path,
        pxr::UsdTimeCode& current_time,
        pxr::UsdTimeCode& render_time) const;

    // Set prim render time (legacy animation system, internal use)
    void set_prim_render_time(const pxr::SdfPath& path, pxr::UsdTimeCode time);

    std::string m_stage_path;
    pxr::UsdStageRefPtr stage;
    pxr::SdfPath create_editor_pending_path;
    pxr::UsdTimeCode current_time_code = pxr::UsdTimeCode(0.0f);
    pxr::UsdTimeCode render_time_code = pxr::UsdTimeCode(0.0f);
    template<typename T>
    T create_prim(const pxr::SdfPath& path, const std::string& baseName) const;

    // Legacy animatable_prims for backward compatibility (can be migrated
    // gradually)
    std::unordered_map<
        pxr::SdfPath,
        animation::WithDynamicLogicPrim,
        pxr::SdfPath::Hash>
        animatable_prims;

    // ========================================================================
    // ECS Members
    // ========================================================================

    // ECS registry
    entt::registry registry_;

    // Entity -> SdfPath mapping
    std::unordered_map<entt::entity, pxr::SdfPath> entity_to_path_;

    // SdfPath -> Entity mapping
    std::unordered_map<pxr::SdfPath, entt::entity, pxr::SdfPath::Hash>
        path_to_entity_;

    // ECS Systems
    std::unique_ptr<ecs::AnimationSystem> animation_system_;
    std::unique_ptr<ecs::UsdSyncSystem> usd_sync_system_;
    std::unique_ptr<ecs::PhysicsSystem> physics_system_;
    std::unique_ptr<ecs::SceneQuerySystem> scene_query_system_;

    // Stage listener
    std::unique_ptr<class StageListener> stage_listener_;

    // Persistent modifier layer (sidecar file)
    pxr::SdfLayerHandle modifier_layer_;

    // Prevent circular loop: sync writes to USD -> notice -> on_prim_changed ->
    // mark dirty -> next sync writes again
    bool is_syncing_to_usd_ = false;

    // Create entity from USD prim (internal use)
    entt::entity create_entity_from_prim(const pxr::UsdPrim& prim);

    // ECS callbacks - called by StageListener
    void on_prim_added(const pxr::UsdPrim& prim);
    void on_prim_removed(const pxr::SdfPath& path);
    void on_prim_changed(const pxr::SdfPath& path);

    // Initialize ECS and StageListener
    void initialize_ecs_systems();
};

STAGE_API std::unique_ptr<Stage> create_global_stage(
    const std::string& usd_name = "../../Assets/stage.usdc");
STAGE_API std::unique_ptr<Stage> create_custom_global_stage(
    const std::string& filename);
RUZINO_NAMESPACE_CLOSE_SCOPE
