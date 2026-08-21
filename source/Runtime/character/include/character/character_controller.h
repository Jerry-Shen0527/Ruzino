#pragma once

#include <entt/entt.hpp>
#include <memory>
#include <unordered_map>

#include "api.h"
#include "character/gait.h"
#include "character/skeleton.h"
#include "input/input_state.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/stage.h"

RUZINO_NAMESPACE_OPEN_SCOPE

namespace character {

// Per-character controller state. Attached to an entity in the Stage's
// registry for every prim carrying `character:controller = true`. Movement
// is kinematic on the ground plane (Z = ground height + bob); a future
// PhysicsSystem backend can replace the integration without touching the
// gait/USD layer.
struct CHARACTER_API CharacterControllerComponent {
    pxr::UsdPrim root_prim;
    std::shared_ptr<Skeleton> skeleton;
    GaitParams gait_params;

    // Tunables mirrored from USD attributes on the root prim.
    float move_speed = 3.0f;       // m/s
    float run_multiplier = 1.9f;   // shift-held speed boost
    float accel = 12.0f;           // velocity smoothing (1/s)
    float turn_smoothing = 10.0f;  // heading smoothing (1/s)

    // Runtime state.
    pxr::GfVec2f position = pxr::GfVec2f(0.0f);  // ground plane
    pxr::GfVec2f velocity = pxr::GfVec2f(0.0f);
    float speed = 0.0f;
    float heading = 0.0f;  // radians; 0 faces +Y, rotates about +Z
    float phase = 0.0f;    // walk cycle phase
    float idle_time = 0.0f;

    // Last pose authored to USD; used to suppress no-op writes so the
    // renderer's accumulation stays quiet while the character stands still.
    GaitPose last_written_pose;
};

// ECS system driving every character in the stage. Called once per tick
// from Stage::tick with the stage's modifier (session) layer as write
// target: root/joint transforms are authored there as over specs, so the
// scene file stays pristine while Hydra picks the new transforms up
// through ordinary stage change processing — the same mechanism the
// simulation zone modifiers use.
class CHARACTER_API CharacterControllerSystem {
   public:
    void update(
        entt::registry& registry,
        const pxr::UsdStageRefPtr& stage,
        const pxr::SdfLayerHandle& modifier_layer,
        const input::InputState& input,
        float delta_time);

    // A prim is a character root when it carries `character:controller`
    // set to true (custom bool attribute, same discovery pattern as the
    // legacy `Animatable` marker).
    static bool is_character_prim(const pxr::UsdPrim& prim);

   private:
    entt::entity ensure_entity(
        entt::registry& registry,
        const pxr::UsdPrim& prim);
    void init_character(CharacterControllerComponent& component) const;
    void update_character(
        const pxr::SdfLayerHandle& modifier_layer,
        const input::InputState& input,
        float delta_time,
        CharacterControllerComponent& component) const;

    std::unordered_map<pxr::SdfPath, entt::entity, pxr::SdfPath::Hash>
        entity_by_path_;
};

// First character root prim path on the stage (used by the viewport to
// offer a follow-camera target). Empty path when the stage has none.
CHARACTER_API pxr::SdfPath find_first_character_path(
    const pxr::UsdStageRefPtr& stage);

}  // namespace character

RUZINO_NAMESPACE_CLOSE_SCOPE
