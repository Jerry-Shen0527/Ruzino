#include "character/character_controller.h"

#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>

RUZINO_NAMESPACE_OPEN_SCOPE

namespace character {

namespace {

    constexpr float kPi = 3.14159265358979323846f;

    template<typename T>
    T read_attr(const pxr::UsdPrim& prim, const char* name, T fallback)
    {
        auto attr = prim.GetAttribute(pxr::TfToken(name));
        if (!attr)
            return fallback;
        pxr::VtValue value;
        if (attr.Get(&value) && value.IsHolding<T>())
            return value.UncheckedGet<T>();
        return fallback;
    }

    // Euler (XYZ, degrees) composition rest * gait. Rest poses of the demo rig
    // are identity, so the fast path just returns the gait rotation.
    pxr::GfVec3f compose_rotation(
        const pxr::GfVec3f& rest,
        const pxr::GfVec3f& gait)
    {
        if (rest == pxr::GfVec3f(0.0f))
            return gait;

        auto quat_from_euler = [](const pxr::GfVec3f& euler) {
            pxr::GfRotation rotation(pxr::GfVec3d(1, 0, 0), euler[0]);
            rotation *= pxr::GfRotation(pxr::GfVec3d(0, 1, 0), euler[1]);
            rotation *= pxr::GfRotation(pxr::GfVec3d(0, 0, 1), euler[2]);
            return rotation.GetQuaternion();
        };

        pxr::GfQuaternion combined =
            quat_from_euler(rest) * quat_from_euler(gait);
        pxr::GfRotation decomposed(combined);
        pxr::GfVec3d angles = decomposed.Decompose(
            pxr::GfVec3d(1, 0, 0),
            pxr::GfVec3d(0, 1, 0),
            pxr::GfVec3d(0, 0, 1));
        return pxr::GfVec3f(
            static_cast<float>(angles[0]),
            static_cast<float>(angles[1]),
            static_cast<float>(angles[2]));
    }

    float max_pose_delta(const GaitPose& a, const GaitPose& b)
    {
        auto d = [](float x, float y) { return std::abs(x - y); };
        return std::max(
            { d(a.hips_yaw, b.hips_yaw),
              d(a.hips_sway, b.hips_sway) * 200.0f,  // meters -> ~degrees
              d(a.spine_pitch, b.spine_pitch),
              d(a.spine_yaw, b.spine_yaw),
              d(a.chest_pitch, b.chest_pitch),
              d(a.chest_yaw, b.chest_yaw),
              d(a.neck_pitch, b.neck_pitch),
              d(a.head_yaw, b.head_yaw),
              d(a.l_thigh, b.l_thigh),
              d(a.l_shin, b.l_shin),
              d(a.l_foot, b.l_foot),
              d(a.r_thigh, b.r_thigh),
              d(a.r_shin, b.r_shin),
              d(a.r_foot, b.r_foot),
              d(a.l_upperarm, b.l_upperarm),
              d(a.l_forearm, b.l_forearm),
              d(a.r_upperarm, b.r_upperarm),
              d(a.r_forearm, b.r_forearm),
              d(a.root_bob, b.root_bob) * 200.0f });
    }

    // Authors translate * rotateXYZ as a single matrix xformOp. The scene's
    // rest poses are authored the same way: some render paths evaluate
    // multi-op CommonAPI stacks (translate + rotateXYZ) incorrectly, while
    // every path handles one matrix op.
    void set_matrix_xform(
        const pxr::UsdPrim& prim,
        const pxr::GfVec3f& translation,
        const pxr::GfVec3f& rotation_euler)
    {
        pxr::UsdGeomXformable xformable(prim);
        bool resets = false;
        auto ops = xformable.GetOrderedXformOps(&resets);
        pxr::UsdGeomXformOp op;
        if (ops.size() == 1 &&
            ops[0].GetOpType() == pxr::UsdGeomXformOp::TypeTransform) {
            op = ops[0];
        }
        else {
            xformable.ClearXformOpOrder();
            op = xformable.MakeMatrixXform();
        }

        pxr::GfRotation rotation(pxr::GfVec3d(1, 0, 0), rotation_euler[0]);
        rotation *= pxr::GfRotation(pxr::GfVec3d(0, 1, 0), rotation_euler[1]);
        rotation *= pxr::GfRotation(pxr::GfVec3d(0, 0, 1), rotation_euler[2]);

        // Gf is row-vector convention: R * T composes to the rigid transform
        // "rotate, then translate" (T * R would rotate the translation itself).
        pxr::GfMatrix4d matrix(1.0);
        matrix.SetTranslate(
            pxr::GfVec3d(translation[0], translation[1], translation[2]));
        pxr::GfMatrix4d rotation_matrix(1.0);
        rotation_matrix.SetRotate(rotation);
        op.Set(rotation_matrix * matrix, pxr::UsdTimeCode::Default());
    }

}  // namespace

bool CharacterControllerSystem::is_character_prim(const pxr::UsdPrim& prim)
{
    return read_attr<bool>(prim, "character:controller", false);
}

pxr::SdfPath find_first_character_path(const pxr::UsdStageRefPtr& stage)
{
    if (!stage)
        return pxr::SdfPath();
    for (pxr::UsdPrim prim : stage->Traverse()) {
        if (CharacterControllerSystem::is_character_prim(prim))
            return prim.GetPath();
    }
    return pxr::SdfPath();
}

void CharacterControllerSystem::update(
    entt::registry& registry,
    const pxr::UsdStageRefPtr& stage,
    const pxr::SdfLayerHandle& modifier_layer,
    const input::InputState& input,
    float delta_time)
{
    if (!stage)
        return;

    for (pxr::UsdPrim prim : stage->Traverse()) {
        if (!is_character_prim(prim))
            continue;

        entt::entity entity = ensure_entity(registry, prim);
        auto& component = registry.get<CharacterControllerComponent>(entity);
        update_character(modifier_layer, input, delta_time, component);
    }
}

entt::entity CharacterControllerSystem::ensure_entity(
    entt::registry& registry,
    const pxr::UsdPrim& prim)
{
    auto it = entity_by_path_.find(prim.GetPath());
    if (it != entity_by_path_.end() && registry.valid(it->second) &&
        registry.all_of<CharacterControllerComponent>(it->second)) {
        return it->second;
    }

    entt::entity entity = registry.create();
    auto& component = registry.emplace<CharacterControllerComponent>(entity);
    component.root_prim = prim;
    init_character(component);
    entity_by_path_[prim.GetPath()] = entity;
    return entity;
}

void CharacterControllerSystem::init_character(
    CharacterControllerComponent& component) const
{
    const pxr::UsdPrim& prim = component.root_prim;

    component.skeleton = std::make_shared<Skeleton>();
    if (!component.skeleton->build_from_prim(prim)) {
        spdlog::warn(
            "[character] prim {} has character:controller but no joint "
            "Xform children; controller disabled",
            prim.GetPath().GetString());
        component.skeleton.reset();
        return;
    }

    component.move_speed = read_attr<float>(prim, "character:moveSpeed", 3.0f);
    component.run_multiplier =
        read_attr<float>(prim, "character:runMultiplier", 1.9f);
    component.gait_params.stride_length =
        read_attr<float>(prim, "character:strideLength", 1.35f);

    // Resume from the currently authored root transform (fresh scenes: the
    // origin) so reopening a stage does not teleport the character.
    pxr::UsdGeomXformCommonAPI xform_api(prim);
    pxr::GfVec3d translation(0.0);
    pxr::GfVec3f rotation(0.0f);
    pxr::GfVec3f scale(1.0f), pivot(0.0f);
    pxr::UsdGeomXformCommonAPI::RotationOrder rot_order;
    xform_api.GetXformVectors(
        &translation,
        &rotation,
        &scale,
        &pivot,
        &rot_order,
        pxr::UsdTimeCode::Default());
    component.position = pxr::GfVec2f(
        static_cast<float>(translation[0]), static_cast<float>(translation[1]));
    component.heading = rotation[2] * kPi / 180.0f;
}

void CharacterControllerSystem::update_character(
    const pxr::SdfLayerHandle& modifier_layer,
    const input::InputState& input,
    float delta_time,
    CharacterControllerComponent& component) const
{
    if (!component.skeleton || !component.root_prim)
        return;

    // ---- input -> desired velocity (camera-relative, ground plane) -------
    float axis_x = 0.0f, axis_y = 0.0f;
    input.move_axis(axis_x, axis_y);
    const pxr::GfVec2f view_forward(
        input.view_forward_x(), input.view_forward_y());
    const pxr::GfVec2f view_right(input.view_right_x(), input.view_right_y());

    pxr::GfVec2f desired_dir = view_forward * axis_y + view_right * axis_x;
    if (desired_dir.GetLength() > 1.0f)
        desired_dir.Normalize();

    const float speed_cap =
        component.move_speed *
        (input.run_held() ? component.run_multiplier : 1.0f);
    const pxr::GfVec2f desired_velocity = desired_dir * speed_cap;

    // ---- integrate ---------------------------------------------------------
    const float accel_alpha = std::min(1.0f, delta_time * component.accel);
    component.velocity += (desired_velocity - component.velocity) * accel_alpha;
    component.speed = component.velocity.GetLength();
    component.position += component.velocity * delta_time;

    // Face the movement direction: heading 0 faces +Y; rotateZ(heading) maps
    // +Y to (-sin, cos), so the heading for velocity v is atan2(-v.x, v.y).
    if (component.speed > 0.05f) {
        const float target_heading =
            std::atan2(-component.velocity[0], component.velocity[1]);
        float delta = target_heading - component.heading;
        while (delta > kPi)
            delta -= 2.0f * kPi;
        while (delta < -kPi)
            delta += 2.0f * kPi;
        component.heading +=
            delta * std::min(1.0f, delta_time * component.turn_smoothing);
        // Keep heading in [-pi, pi] so the authored rotateZ (and the resume
        // path reading it back) never drifts to equivalent-but-unbounded
        // representations like 270 deg instead of -90 deg.
        while (component.heading > kPi)
            component.heading -= 2.0f * kPi;
        while (component.heading < -kPi)
            component.heading += 2.0f * kPi;
    }

    // ---- gait state --------------------------------------------------------
    const float walk_weight =
        std::clamp(component.speed / component.move_speed, 0.0f, 1.0f);
    if (component.speed > 0.02f) {
        component.phase +=
            2.0f * kPi *
            step_rate_for_speed(component.speed, component.gait_params) *
            delta_time;
        component.idle_time = 0.0f;
    }
    else {
        component.idle_time += delta_time;
    }

    const GaitPose pose = blend(
        evaluate_idle(component.idle_time),
        evaluate_walk(component.phase, walk_weight, component.gait_params),
        walk_weight);

    // Skip USD writes when nothing visible changed (standing still): keeps
    // Hydra — and therefore the path tracer's accumulation — quiet.
    if (component.speed <= 0.02f &&
        max_pose_delta(pose, component.last_written_pose) < 0.05f) {
        return;
    }
    component.last_written_pose = pose;

    // ---- author transforms into the modifier (session) layer ---------------
    if (!modifier_layer)
        return;
    pxr::UsdStagePtr stage = component.root_prim.GetStage();
    pxr::UsdEditContext edit_context(stage, modifier_layer);

    set_matrix_xform(
        component.root_prim,
        pxr::GfVec3f(
            component.position[0], component.position[1], pose.root_bob),
        pxr::GfVec3f(0.0f, 0.0f, component.heading * 180.0f / kPi));

    for (const Joint& joint : component.skeleton->joints()) {
        pxr::GfVec3f translation = joint.rest_translation;
        pxr::GfVec3f gait_rotation(0.0f);

        switch (joint.role) {
            case HumanoidRole::Hips:
                translation[0] += pose.hips_sway;
                gait_rotation = pxr::GfVec3f(0.0f, 0.0f, pose.hips_yaw);
                break;
            case HumanoidRole::Spine:
                gait_rotation =
                    pxr::GfVec3f(pose.spine_pitch, 0.0f, pose.spine_yaw);
                break;
            case HumanoidRole::Chest:
                gait_rotation =
                    pxr::GfVec3f(pose.chest_pitch, 0.0f, pose.chest_yaw);
                break;
            case HumanoidRole::Neck:
                gait_rotation = pxr::GfVec3f(pose.neck_pitch, 0.0f, 0.0f);
                break;
            case HumanoidRole::Head:
                gait_rotation = pxr::GfVec3f(0.0f, 0.0f, pose.head_yaw);
                break;
            case HumanoidRole::L_Thigh:
                gait_rotation = pxr::GfVec3f(pose.l_thigh, 0.0f, 0.0f);
                break;
            case HumanoidRole::L_Shin:
                gait_rotation = pxr::GfVec3f(pose.l_shin, 0.0f, 0.0f);
                break;
            case HumanoidRole::L_Foot:
                gait_rotation = pxr::GfVec3f(pose.l_foot, 0.0f, 0.0f);
                break;
            case HumanoidRole::R_Thigh:
                gait_rotation = pxr::GfVec3f(pose.r_thigh, 0.0f, 0.0f);
                break;
            case HumanoidRole::R_Shin:
                gait_rotation = pxr::GfVec3f(pose.r_shin, 0.0f, 0.0f);
                break;
            case HumanoidRole::R_Foot:
                gait_rotation = pxr::GfVec3f(pose.r_foot, 0.0f, 0.0f);
                break;
            case HumanoidRole::L_UpperArm:
                gait_rotation = pxr::GfVec3f(pose.l_upperarm, 0.0f, 0.0f);
                break;
            case HumanoidRole::L_Forearm:
                gait_rotation = pxr::GfVec3f(pose.l_forearm, 0.0f, 0.0f);
                break;
            case HumanoidRole::R_UpperArm:
                gait_rotation = pxr::GfVec3f(pose.r_upperarm, 0.0f, 0.0f);
                break;
            case HumanoidRole::R_Forearm:
                gait_rotation = pxr::GfVec3f(pose.r_forearm, 0.0f, 0.0f);
                break;
            default: break;  // Unknown joints keep their rest pose
        }

        pxr::UsdPrim joint_prim =
            component.root_prim.GetStage()->GetPrimAtPath(joint.path);
        set_matrix_xform(
            joint_prim,
            translation,
            compose_rotation(joint.rest_rotation_euler, gait_rotation));
    }
}

}  // namespace character

RUZINO_NAMESPACE_CLOSE_SCOPE
