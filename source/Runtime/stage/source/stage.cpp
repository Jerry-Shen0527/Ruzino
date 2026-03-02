#include "stage/stage.hpp"

#include <pxr/base/gf/rotation.h>
#include <pxr/pxr.h>
#include <pxr/usd/usd/payloads.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/cylinder.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdMtlx/reader.h>
#include <pxr/usd/usdMtlx/utils.h>
#include <pxr/usd/usdShade/material.h>
#include <spdlog/spdlog.h>

#include <filesystem>

#include "GCore/GOP.h"
#include "MaterialXFormat/File.h"
#include "MaterialXFormat/Util.h"
#include "stage/animation.h"
#include "stage_listener/stage_listener.h"

RUZINO_NAMESPACE_OPEN_SCOPE
#define SAVE_ALL_THE_TIME 0

Stage::Stage()
{
    std::string stage_path = "../../Assets/demo_stroke.usdc";
    stage_path = "../../Assets/stage.usdc";

    std::filesystem::path executable_path;

#ifdef _WIN32
    char p[MAX_PATH];
    GetModuleFileNameA(NULL, p, MAX_PATH);
    executable_path = std::filesystem::path(p).parent_path();
#else
    char p[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", p, PATH_MAX);
    if (count != -1) {
        p[count] = '\0';
        executable_path = std::filesystem::path(path).parent_path();
    }
    else {
        throw std::runtime_error("Failed to get executable path.");
    }
#endif

    std::filesystem::path abs_path;
    if (!stage_path.empty()) {
        abs_path = std::filesystem::path(stage_path);
    }
    else {
        spdlog::error("Path is empty.");
        return;
    }
    if (!abs_path.is_absolute()) {
        abs_path = executable_path / abs_path;
    }
    abs_path = abs_path.lexically_normal();
    m_stage_path = abs_path.string();

    // if stage.usda exists, load it
    stage = pxr::UsdStage::Open(abs_path.string());
    if (stage) {
        initialize_ecs_systems();
        return;
    }

    stage = pxr::UsdStage::CreateNew(abs_path.string());
    stage->SetMetadata(pxr::UsdGeomTokens->metersPerUnit, 1.0);
    stage->SetMetadata(pxr::UsdGeomTokens->upAxis, pxr::TfToken("Z"));

    initialize_ecs_systems();
}

Stage::Stage(const std::string& stage_path)
{
    std::filesystem::path abs_path;
    if (!stage_path.empty()) {
        abs_path = std::filesystem::path(stage_path);
    }
    else {
        spdlog::error("Path is empty.");
        return;
    }
    abs_path = abs_path.lexically_normal();
    m_stage_path = abs_path.string();
    // if stage.usda exists, load it
    stage = pxr::UsdStage::Open(abs_path.string());
    if (stage) {
        initialize_ecs_systems();
        load_modifier_layer();
        return;
    }
    stage = pxr::UsdStage::CreateNew(abs_path.string());
    stage->SetMetadata(pxr::UsdGeomTokens->metersPerUnit, 1.0);
    stage->SetMetadata(pxr::UsdGeomTokens->upAxis, pxr::TfToken("Z"));

    initialize_ecs_systems();
}

Stage::~Stage()
{
    remove_prim(pxr::SdfPath("/scratch_buffer"));
    if (stage && !m_stage_path.empty() && save_on_destruct) {
        stage->GetRootLayer()->Save();
        save_modifier_layer();
    }

    animatable_prims.clear();
}

void Stage::tick(float ellapsed_time)
{
    // Update animation system
    if (animation_system_) {
        animation_system_->update(registry_, ellapsed_time);
    }

    // Update physics system
    if (physics_system_) {
        physics_system_->update(registry_, ellapsed_time);
    }

    // Keep legacy animation logic for backward compatibility
    // (can be migrated to ECS gradually)
    for (auto&& prim : stage->Traverse()) {
        if (animation::WithDynamicLogicPrim::is_animatable(prim)) {
            if (animatable_prims.find(prim.GetPath()) ==
                animatable_prims.end()) {
                animatable_prims.emplace(
                    prim.GetPath(),
                    std::move(animation::WithDynamicLogicPrim(prim, this)));
            }

            animatable_prims.at(prim.GetPath()).update(ellapsed_time);
        }
    }

    // Update global time code
    auto current = current_time_code.GetValue();
    current += ellapsed_time;
    current_time_code = pxr::UsdTimeCode(current);
}

void Stage::finish_tick()
{
}

pxr::UsdTimeCode Stage::get_current_time()
{
    return current_time_code;
}

void Stage::set_current_time(pxr::UsdTimeCode time)
{
    current_time_code = time;
}

pxr::UsdTimeCode Stage::get_render_time()
{
    return render_time_code;
}

void Stage::set_render_time(pxr::UsdTimeCode time)
{
    render_time_code = time;
}

template<typename T>
T Stage::create_prim(const pxr::SdfPath& path, const std::string& baseName)
    const
{
    int id = 0;
    while (stage->GetPrimAtPath(
        path.AppendPath(pxr::SdfPath(baseName + "_" + std::to_string(id))))) {
        id++;
    }
    auto a = T::Define(
        stage,
        path.AppendPath(pxr::SdfPath(baseName + "_" + std::to_string(id))));
#if SAVE_ALL_THE_TIME
    stage->Save();
#endif
    return a;
}

pxr::UsdPrim Stage::add_prim(const pxr::SdfPath& path)
{
    return stage->DefinePrim(path);
}

pxr::UsdShadeMaterial Stage::create_material(const pxr::SdfPath& path)
{
    auto material = create_prim<pxr::UsdShadeMaterial>(path, "material");

    // Add custom shader_path attribute for material callable shader
    auto shader_path_attr = material.GetPrim().CreateAttribute(
        pxr::TfToken("shader_path"), pxr::SdfValueTypeNames->String, false);
    shader_path_attr.Set(std::string(""));  // Empty by default

    return material;
}

pxr::UsdGeomSphere Stage::create_sphere(const pxr::SdfPath& path)
{
    auto sphere = create_prim<pxr::UsdGeomSphere>(path, "sphere");
    auto prim = sphere.GetPrim();

    // Create corresponding entity directly
    // Prevent USD notice mechanism from also triggering creation (through
    // duplicate check)
    if (prim.IsValid() && find_entity_by_path(prim.GetPath()) == entt::null) {
        on_prim_added(prim);
    }

    return sphere;
}

pxr::UsdGeomCylinder Stage::create_cylinder(const pxr::SdfPath& path)
{
    auto cylinder = create_prim<pxr::UsdGeomCylinder>(path, "cylinder");
    auto prim = cylinder.GetPrim();
    if (prim.IsValid() && find_entity_by_path(prim.GetPath()) == entt::null) {
        on_prim_added(prim);
    }
    return cylinder;
}

pxr::UsdGeomCube Stage::create_cube(const pxr::SdfPath& path)
{
    auto cube = create_prim<pxr::UsdGeomCube>(path, "cube");
    auto prim = cube.GetPrim();
    if (prim.IsValid() && find_entity_by_path(prim.GetPath()) == entt::null) {
        on_prim_added(prim);
    }
    return cube;
}

pxr::UsdGeomXform Stage::create_xform(const pxr::SdfPath& path)
{
    auto xform = create_prim<pxr::UsdGeomXform>(path, "xform");
    auto prim = xform.GetPrim();
    if (prim.IsValid() && find_entity_by_path(prim.GetPath()) == entt::null) {
        on_prim_added(prim);
    }
    return xform;
}

pxr::UsdGeomMesh Stage::create_mesh(const pxr::SdfPath& path)
{
    auto mesh = create_prim<pxr::UsdGeomMesh>(path, "mesh");
    auto prim = mesh.GetPrim();
    if (prim.IsValid() && find_entity_by_path(prim.GetPath()) == entt::null) {
        on_prim_added(prim);
    }
    return mesh;
}

pxr::UsdLuxRectLight Stage::create_rect_light(const pxr::SdfPath& path) const
{
    auto light = create_prim<pxr::UsdLuxRectLight>(path, "rect_light");
    light.GetIntensityAttr().Set(1.0f);
    light.GetWidthAttr().Set(2.0f);
    light.GetHeightAttr().Set(2.0f);

    auto xform = pxr::UsdGeomXformable(light);
    pxr::GfMatrix4d matrix;
    matrix.SetTranslate(pxr::GfVec3d(0.0, 0.0, 1.0));
    xform.MakeMatrixXform().Set(matrix);

    return light;
}

pxr::UsdLuxDistantLight Stage::create_distant_light(
    const pxr::SdfPath& path) const
{
    auto light = create_prim<pxr::UsdLuxDistantLight>(path, "distant_light");
    light.GetIntensityAttr().Set(1.0f);
    light.GetAngleAttr().Set(0.5f);

    auto xform = pxr::UsdGeomXformable(light);
    pxr::GfMatrix4d matrix;
    matrix.SetRotate(pxr::GfRotation(pxr::GfVec3d(1.0, 0.0, 0.0), 180.0));
    xform.MakeMatrixXform().Set(matrix);

    return light;
}

pxr::UsdLuxDiskLight Stage::create_disk_light(const pxr::SdfPath& path) const
{
    auto light = create_prim<pxr::UsdLuxDiskLight>(path, "disk_light");
    light.GetIntensityAttr().Set(1.0f);
    light.GetRadiusAttr().Set(1.0f);

    auto xform = pxr::UsdGeomXformable(light);
    pxr::GfMatrix4d matrix;
    matrix.SetTranslate(pxr::GfVec3d(0.0, 0.0, 2.0));
    xform.MakeMatrixXform().Set(matrix);

    return light;
}

pxr::UsdLuxDomeLight Stage::create_dome_light(const pxr::SdfPath& path) const
{
    auto light = create_prim<pxr::UsdLuxDomeLight>(path, "dome_light");
    light.GetIntensityAttr().Set(1.0f);

    light.GetTextureFileAttr().Set(
        pxr::SdfAssetPath("usd/hd_RUZINO/resources/textures/default_dome.png"));

    // Add custom shader_path attribute for dome light callable shader
    auto shader_path_attr = light.GetPrim().CreateAttribute(
        pxr::TfToken("shader_path"), pxr::SdfValueTypeNames->String, false);
    shader_path_attr.Set(std::string(""));  // Empty by default

    return light;
}

void Stage::remove_prim(const pxr::SdfPath& path)
{
    if (animatable_prims.find(path) != animatable_prims.end()) {
        animatable_prims.erase(path);
    }
    stage->RemovePrim(path);  // This operation is in fact not recommended! In
                              // Omniverse applications, they set the prim to
                              // invisible instead of removing it.

#if SAVE_ALL_THE_TIME
    stage->Save();
#endif
}

std::string Stage::stage_content() const
{
    std::string str;
    stage->GetRootLayer()->ExportToString(&str);
    return str;
}

pxr::UsdStageRefPtr Stage::get_usd_stage() const
{
    return stage;
}

void Stage::create_editor_at_path(const pxr::SdfPath& sdf_path)
{
    create_editor_pending_path = sdf_path;
}

bool Stage::consume_editor_creation(pxr::SdfPath& json_path, bool fully_consume)
{
    if (create_editor_pending_path.IsEmpty()) {
        return false;
    }

    json_path = create_editor_pending_path;
    if (fully_consume) {
        create_editor_pending_path = pxr::SdfPath::EmptyPath();
    }
    return true;
}

void Stage::save_string_to_usd(
    const pxr::SdfPath& path,
    const std::string& data)
{
    auto prim = stage->GetPrimAtPath(path);
    if (!prim) {
        return;
    }

    auto attr = prim.CreateAttribute(
        pxr::TfToken("node_json"), pxr::SdfValueTypeNames->String);
    attr.Set(data);
#if SAVE_ALL_THE_TIME
    stage->Save();
#endif
}

std::string Stage::load_string_from_usd(const pxr::SdfPath& path)
{
    auto prim = stage->GetPrimAtPath(path);
    if (!prim) {
        return "";
    }

    auto attr = prim.GetAttribute(pxr::TfToken("node_json"));
    if (!attr) {
        return "";
    }

    std::string data;
    attr.Get(&data);
    return data;
}

void Stage::import_usd_as_payload(
    const std::string& path_string,
    const pxr::SdfPath& sdf_path)
{
    auto prim = stage->GetPrimAtPath(sdf_path);
    if (!prim) {
        return;
    }

    // bring the usd file into the stage with payload

    auto paylaods = prim.GetPayloads();
    paylaods.AddPayload(pxr::SdfPayload(path_string));
#if SAVE_ALL_THE_TIME
    stage->Save();
#endif
}

void Stage::import_usd_as_reference(
    const std::string& path_string,
    const pxr::SdfPath& sdf_path)
{
    auto prim = stage->GetPrimAtPath(sdf_path);
    if (!prim) {
        return;
    }

    // bring the usd file into the stage with reference

    auto references = prim.GetReferences();
    references.AddReference(pxr::SdfReference(path_string));
}

void Stage::import_materialx(
    const std::string& path_string,
    const pxr::SdfPath& sdf_path)
{
    MaterialX::FilePath path(path_string);
}

std::unique_ptr<Stage> create_global_stage(const std::string& usd_name)
{
    return std::make_unique<Stage>(usd_name);
}

std::unique_ptr<Stage> create_custom_global_stage(const std::string& filename)
{
    return std::make_unique<Stage>(filename);
}

bool Stage::get_prim_time_info(
    const pxr::SdfPath& path,
    pxr::UsdTimeCode& current_time,
    pxr::UsdTimeCode& render_time) const
{
    auto it = animatable_prims.find(path);
    if (it == animatable_prims.end()) {
        return false;
    }

    current_time = it->second.get_prim_current_time();
    render_time = it->second.get_prim_render_time();
    return true;
}

void Stage::set_prim_render_time(
    const pxr::SdfPath& path,
    pxr::UsdTimeCode time)
{
    auto it = animatable_prims.find(path);
    if (it != animatable_prims.end()) {
        it->second.set_prim_render_time(time);
    }
}

void Stage::Save()
{
    if (stage && !m_stage_path.empty()) {
        stage->GetRootLayer()->Save();
        spdlog::info("Stage saved to: {}", m_stage_path);

        save_modifier_layer();
    }
}

void Stage::SaveAs(const std::string& new_path)
{
    if (!stage) {
        spdlog::error("No stage to save");
        return;
    }

    std::filesystem::path abs_path =
        std::filesystem::path(new_path).lexically_normal();

    // 1. Export current session layer to NEW modifier path BEFORE switching
    // stages
    auto session_layer = stage->GetSessionLayer();
    if (session_layer) {
        // Calculate new modifier path based on new_path
        std::filesystem::path new_modifier_path = abs_path;
        std::string stem = abs_path.stem().string();
        std::string ext = abs_path.extension().string();
        new_modifier_path =
            abs_path.parent_path() / (stem + "_modifiers" + ext);

        session_layer->Export(new_modifier_path.string());
        spdlog::info(
            "[Stage] Exported session layer to: {}",
            new_modifier_path.string());
    }

    // 2. Export root layer to new location
    stage->GetRootLayer()->Export(abs_path.string());
    m_stage_path = abs_path.string();

    // 3. Reset and reopen stage
    modifier_layer_ = nullptr;
    stage = pxr::UsdStage::Open(m_stage_path);
    load_modifier_layer();

    spdlog::info("Stage saved as: {}", m_stage_path);
}

std::string Stage::get_modifier_layer_path() const
{
    if (m_stage_path.empty()) {
        return "";
    }

    std::filesystem::path stage_path(m_stage_path);
    std::filesystem::path modifier_path = stage_path;
    std::string stem = stage_path.stem().string();
    std::string ext = stage_path.extension().string();
    modifier_path = stage_path.parent_path() / (stem + "_modifiers" + ext);
    return modifier_path.string();
}

pxr::SdfLayerHandle Stage::get_modifier_layer()
{
    if (!stage) {
        spdlog::error("[Stage] get_modifier_layer: stage is null");
        return nullptr;
    }

    if (!modifier_layer_) {
        modifier_layer_ = stage->GetSessionLayer();
        if (modifier_layer_) {
            spdlog::info("[Stage] Using session layer as modifier layer");
        }
        else {
            spdlog::error("[Stage] Failed to get session layer");
        }
    }

    return modifier_layer_;
}

void Stage::save_modifier_layer()
{
    if (!modifier_layer_) {
        return;
    }

    std::string modifier_path = get_modifier_layer_path();
    if (modifier_path.empty()) {
        return;
    }

    modifier_layer_->Export(modifier_path);
    spdlog::info("[Stage] Exported modifier layer to: {}", modifier_path);
}

void Stage::load_modifier_layer()
{
    std::string modifier_path = get_modifier_layer_path();

    if (modifier_path.empty() || !std::filesystem::exists(modifier_path)) {
        spdlog::info("[Stage] No modifier file to load: {}", modifier_path);
        return;
    }

    auto file_layer = pxr::SdfLayer::FindOrOpen(modifier_path);
    if (!file_layer) {
        spdlog::warn(
            "[Stage] Failed to load modifier layer: {}", modifier_path);
        return;
    }

    auto session_layer = stage->GetSessionLayer();
    if (!session_layer) {
        spdlog::error("[Stage] Failed to get session layer");
        return;
    }

    std::string content;
    file_layer->ExportToString(&content);
    session_layer->ImportFromString(content);

    spdlog::info(
        "[Stage] Imported modifier layer into session layer: {}",
        modifier_path);
}

bool Stage::OpenStage(const std::string& path)
{
    std::filesystem::path abs_path =
        std::filesystem::path(path).lexically_normal();

    if (!std::filesystem::exists(abs_path)) {
        spdlog::error("Stage file does not exist: {}", abs_path.string());
        return false;
    }

    // Clear existing animatable prims
    animatable_prims.clear();

    // Open the new stage
    auto new_stage = pxr::UsdStage::Open(abs_path.string());
    if (!new_stage) {
        spdlog::error("Failed to open stage: {}", abs_path.string());
        return false;
    }

    // Replace the current stage
    stage = new_stage;
    m_stage_path = abs_path.string();

    // Clear and load modifier layer
    modifier_layer_ = nullptr;
    load_modifier_layer();

    // Reset time codes
    current_time_code = pxr::UsdTimeCode(0.0f);
    render_time_code = pxr::UsdTimeCode(0.0f);

    spdlog::info("Opened stage: {}", m_stage_path);
    return true;
}

// ============================================================================
// ECS Interface Implementation
// ============================================================================

entt::entity Stage::create_entity_from_prim(const pxr::UsdPrim& prim)
{
    if (!usd_sync_system_) {
        spdlog::error("[Stage] UsdSyncSystem not initialized");
        return entt::null;
    }

    if (!prim.IsValid()) {
        spdlog::error("[Stage] Invalid prim passed to create_entity_from_prim");
        return entt::null;
    }

    // Prevent duplicate creation: check if this prim's path is already in the
    // mapping
    auto it = path_to_entity_.find(prim.GetPath());
    if (it != path_to_entity_.end()) {
        return it->second;  // Already exists, return existing entity
    }

    auto entity = usd_sync_system_->create_entity_from_prim(registry_, prim);

    if (entity == entt::null) {
        spdlog::error(
            "[Stage] UsdSyncSystem failed to create entity for prim: {}",
            prim.GetPath().GetString());
        return entt::null;
    }

    // Establish mapping relationship
    entity_to_path_[entity] = prim.GetPath();
    path_to_entity_[prim.GetPath()] = entity;

    spdlog::debug(
        "[Stage] Created entity {} for prim {}",
        static_cast<uint32_t>(entity),
        prim.GetPath().GetString());

    return entity;
}

pxr::UsdPrim Stage::get_prim_from_entity(entt::entity entity)
{
    auto it = entity_to_path_.find(entity);
    if (it == entity_to_path_.end()) {
        return pxr::UsdPrim();
    }

    return stage->GetPrimAtPath(it->second);
}

entt::entity Stage::find_entity_by_path(const pxr::SdfPath& path)
{
    auto it = path_to_entity_.find(path);
    if (it == path_to_entity_.end()) {
        return entt::null;
    }

    return it->second;
}

void Stage::sync_entities_to_usd()
{
    if (!usd_sync_system_) {
        return;
    }

    // Set flag: syncing to USD, prevent circular loop from notice
    is_syncing_to_usd_ = true;
    usd_sync_system_->sync(registry_, render_time_code);
    // Clear flag
    is_syncing_to_usd_ = false;
}

void Stage::load_prims_to_ecs()
{
    if (!stage) {
        return;
    }

    // Clear existing ECS data
    registry_.clear();
    entity_to_path_.clear();
    path_to_entity_.clear();

    // Traverse all prims and create corresponding entities
    for (const auto& prim : stage->Traverse()) {
        create_entity_from_prim(prim);
    }
}

// ============================================================================
// ECS callback function implementation
// ============================================================================

void Stage::initialize_ecs_systems()
{
    // Initialize ECS systems
    animation_system_ = std::make_unique<ecs::AnimationSystem>(this);
    usd_sync_system_ = std::make_unique<ecs::UsdSyncSystem>(this);
    physics_system_ = std::make_unique<ecs::PhysicsSystem>();
    scene_query_system_ =
        std::make_unique<ecs::SceneQuerySystem>(physics_system_.get());

    // Initialize StageListener - fully depends on USD notice mechanism
    if (stage) {
        stage_listener_ = std::make_unique<StageListener>(stage);
        stage_listener_->SetPrimAddedCallback(
            [this](const pxr::UsdPrim& prim) { on_prim_added(prim); });
        stage_listener_->SetPrimRemovedCallback(
            [this](const pxr::SdfPath& path) { on_prim_removed(path); });
        stage_listener_->SetPrimChangedCallback(
            [this](const pxr::SdfPath& path) { on_prim_changed(path); });
        // Don't call CapturePrimSnapshot - fully depend on USD notice mechanism
        // This way a newly created empty stage won't have any pre-existing
        // entities
    }
}

void Stage::on_prim_added(const pxr::UsdPrim& prim)
{
    // Prevent duplicate creation: check path_to_entity_ map directly
    if (path_to_entity_.find(prim.GetPath()) != path_to_entity_.end()) {
        return;
    }

    // Create entity for newly added prim
    create_entity_from_prim(prim);
}

void Stage::on_prim_removed(const pxr::SdfPath& path)
{
    // Delete corresponding entity
    auto entity = find_entity_by_path(path);
    if (entity != entt::null) {
        registry_.destroy(entity);
        entity_to_path_.erase(entity);
        path_to_entity_.erase(path);
    }
}

void Stage::on_prim_changed(const pxr::SdfPath& path)
{
    // Prevent circular loop: if syncing to USD, don't handle notice (because
    // this change was triggered by sync)
    if (is_syncing_to_usd_) {
        return;
    }

    auto prim = stage->GetPrimAtPath(path);
    if (!prim) {
        return;
    }

    // Find or create entity
    auto entity = find_entity_by_path(path);
    if (entity == entt::null) {
        // If it's a geometry prim, automatically create entity
        if (prim.IsA<pxr::UsdGeomMesh>()) {
            entity = create_entity_from_prim(prim);
            spdlog::info(
                "[Stage] Auto-created entity for changed geometry prim: {}",
                path.GetString());
        }
        else {
            return;
        }
    }

    // If it's a geometry prim, sync from USD to GeometryComponent
    if (prim.IsA<pxr::UsdGeomMesh>()) {
        auto& usd_comp = registry_.get<ecs::UsdPrimComponent>(entity);

        // Create or get GeometryComponent
        if (!registry_.all_of<ecs::GeometryComponent>(entity)) {
            auto geometry = std::make_shared<Geometry>();
            registry_.emplace<ecs::GeometryComponent>(entity, geometry);
        }

        auto& geom_comp = registry_.get<ecs::GeometryComponent>(entity);
        if (!geom_comp.geometry) {
            geom_comp.geometry = std::make_shared<Geometry>();
        }

        // Sync from USD to Geometry
        usd_comp.sync_to_geometry(*geom_comp.geometry);

        // Mark as dirty, needs geometry update
        if (!registry_.all_of<ecs::DirtyComponent>(entity)) {
            registry_.emplace<ecs::DirtyComponent>(entity);
        }
        auto& dirty = registry_.get<ecs::DirtyComponent>(entity);
        dirty.needs_geometry_update = true;

        spdlog::info(
            "[Stage] Synced geometry from USD for prim: {}", path.GetString());
    }

    // For other types of prims (such as Sphere, Cube, etc.), also mark as dirty
    else if (prim.IsA<pxr::UsdGeomGprim>()) {
        if (!registry_.all_of<ecs::DirtyComponent>(entity)) {
            registry_.emplace<ecs::DirtyComponent>(entity);
        }
        auto& dirty = registry_.get<ecs::DirtyComponent>(entity);
        dirty.needs_geometry_update = true;

        spdlog::info("[Stage] Marked prim as dirty: {}", path.GetString());
    }
}

std::string Stage::traverse_stage(int max_depth) const
{
    if (!stage) {
        return "[ERROR] Stage is null";
    }

    std::stringstream ss;
    ss << "=== Stage Traverse Report ===\n";
    ss << "Stage path: " << m_stage_path << "\n";
    ss << "Max depth: "
       << (max_depth < 0 ? "unlimited" : std::to_string(max_depth)) << "\n\n";

    // Get the root prim
    pxr::UsdPrim root = stage->GetPseudoRoot();

    std::function<void(const pxr::UsdPrim&, int)> traverse_prim;
    traverse_prim = [&](const pxr::UsdPrim& prim, int depth) {
        if (max_depth >= 0 && depth > max_depth) {
            return;
        }

        std::string indent(depth * 2, ' ');
        std::string type_name = prim.GetTypeName().GetString();
        std::string specifier;

        switch (prim.GetSpecifier()) {
            case pxr::SdfSpecifierDef: specifier = "def"; break;
            case pxr::SdfSpecifierOver: specifier = "over"; break;
            case pxr::SdfSpecifierClass: specifier = "class"; break;
            default: specifier = "unknown";
        }

        ss << indent << specifier << " " << type_name << " \""
           << prim.GetPath().GetString() << "\"";

        // Check if prim is active
        if (!prim.IsActive()) {
            ss << " [INACTIVE]";
        }

        // Check if prim is defined (has specs)
        if (prim.HasAuthoredReferences()) {
            ss << " [HAS_REFS]";
        }

        // Check for geometry
        if (prim.IsA<pxr::UsdGeomMesh>()) {
            pxr::UsdGeomMesh mesh(prim);
            pxr::VtIntArray face_vertex_counts;
            pxr::UsdAttribute fvc_attr = mesh.GetFaceVertexCountsAttr();
            if (fvc_attr.HasAuthoredValue()) {
                fvc_attr.Get(&face_vertex_counts);
                ss << " [MESH: " << face_vertex_counts.size() << " faces]";
            }
        }

        ss << "\n";

        // Traverse children
        for (const auto& child : prim.GetFilteredChildren(
                 pxr::UsdPrimIsActive && pxr::UsdPrimIsDefined &&
                 pxr::UsdPrimIsLoaded)) {
            traverse_prim(child, depth + 1);
        }
    };

    traverse_prim(root, 0);

    ss << "\n=== Summary ===\n";
    ss << "Entity-to-path mappings: " << entity_to_path_.size() << "\n";
    ss << "Path-to-entity mappings: " << path_to_entity_.size() << "\n";

    return ss.str();
}

RUZINO_NAMESPACE_CLOSE_SCOPE
