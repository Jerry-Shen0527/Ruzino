#pragma once

#include "api.h"
#include "character/humanoid_roles.h"
#include "pxr/base/gf/vec3f.h"

RUZINO_NAMESPACE_OPEN_SCOPE

namespace character {

// Procedural locomotion pose. Rotations are joint-local Euler XYZ angles in
// degrees (positive pitch swings a bone forward, toward the character's
// facing direction +Y at rest); sway/bob are meters. All values are
// deltas relative to the rig's rest pose.
struct GaitPose {
    // Pelvis / torso
    float hips_yaw = 0.0f;
    float hips_sway = 0.0f;  // lateral offset, character-local X
    float spine_pitch = 0.0f;
    float spine_yaw = 0.0f;
    float chest_pitch = 0.0f;
    float chest_yaw = 0.0f;
    float neck_pitch = 0.0f;
    float head_yaw = 0.0f;

    // Legs (pitch about local X)
    float l_thigh = 0.0f;
    float l_shin = 0.0f;
    float l_foot = 0.0f;
    float r_thigh = 0.0f;
    float r_shin = 0.0f;
    float r_foot = 0.0f;

    // Arms (pitch about local X; forearm bends forward)
    float l_upperarm = 0.0f;
    float l_forearm = 0.0f;
    float r_upperarm = 0.0f;
    float r_forearm = 0.0f;

    // Root offsets applied to the character root prim (bob) in meters
    float root_bob = 0.0f;
};

// Tuning parameters for the procedural walk cycle. Defaults give a relaxed
// human stroll at ~3 m/s; scenes can override via USD attributes on the
// character root prim (character:strideLength, character:stepRate).
struct CHARACTER_API GaitParams {
    float thigh_swing_deg = 30.0f;  // hip amplitude at full weight
    float knee_swing_deg = 55.0f;   // swing-phase knee flexion
    float knee_stance_deg = 8.0f;   // small stance-phase flexion
    float arm_swing_deg = 24.0f;    // upper arm amplitude
    float elbow_bend_deg = 14.0f;   // constant elbow flexion base
    float torso_twist_deg = 6.0f;   // chest counter-rotation
    float pelvis_twist_deg = 4.0f;  // hips yaw amplitude
    float pelvis_sway_m = 0.03f;    // lateral sway amplitude
    float bob_m = 0.03f;            // vertical bob amplitude
    float lean_deg = 3.0f;          // forward torso lean at full weight
    float stride_length = 1.35f;    // meters per full cycle at walk speed
    float min_step_rate = 0.9f;     // cycles/s floor while moving
};

// Pure procedural locomotion math — no USD state, trivially unit-testable.

// Walk cycle at phase `phase` (radians, 2π = one full stride) scaled by
// `weight` in [0, 1] (0 = standing, 1 = full walk). Left/right legs are π
// out of phase; arms counter-swing against the same-side leg.
CHARACTER_API GaitPose
evaluate_walk(float phase, float weight, const GaitParams& params);

// Idle stance (breathing, weight shift, subtle look-around) at time `t`.
CHARACTER_API GaitPose evaluate_idle(float t);

CHARACTER_API GaitPose blend(const GaitPose& a, const GaitPose& b, float t);

// Step frequency (cycles/sec) for a given ground speed, honoring the
// stride length and the minimum step rate while moving.
CHARACTER_API float step_rate_for_speed(float speed, const GaitParams& params);

}  // namespace character

RUZINO_NAMESPACE_CLOSE_SCOPE
