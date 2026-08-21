#include "character/gait.h"

#include <algorithm>
#include <cmath>

RUZINO_NAMESPACE_OPEN_SCOPE

namespace character {

namespace {
    constexpr float kPi = 3.14159265358979323846f;
}  // namespace

GaitPose evaluate_walk(float phase, float weight, const GaitParams& p)
{
    weight = std::clamp(weight, 0.0f, 1.0f);
    GaitPose pose;

    const float l = std::sin(phase);        // left leg forward-ness
    const float l_swing = std::cos(phase);  // >0 while left leg swings
    const float r = std::sin(phase + kPi);  // right leg, half cycle off
    const float r_swing = std::cos(phase + kPi);

    // Legs: hip swings sinuisoidally; the knee flexes during swing phase
    // (leg off the ground, moving forward) and keeps a slight bend during
    // stance. The foot roughly counter-rotates to stay level.
    pose.l_thigh = p.thigh_swing_deg * weight * l;
    pose.r_thigh = p.thigh_swing_deg * weight * r;
    pose.l_shin = -p.knee_swing_deg * weight * std::max(0.0f, l_swing) -
                  p.knee_stance_deg * weight * std::max(0.0f, -l_swing);
    pose.r_shin = -p.knee_swing_deg * weight * std::max(0.0f, r_swing) -
                  p.knee_stance_deg * weight * std::max(0.0f, -r_swing);
    pose.l_foot = -(pose.l_thigh + pose.l_shin) * 0.45f;
    pose.r_foot = -(pose.r_thigh + pose.r_shin) * 0.45f;

    // Arms counter-swing against the same-side leg; elbows bend a little
    // more as the arm swings forward.
    pose.l_upperarm = -p.arm_swing_deg * weight * l;
    pose.r_upperarm = -p.arm_swing_deg * weight * r;
    pose.l_forearm =
        p.elbow_bend_deg * weight * (0.6f + 0.4f * std::max(0.0f, -l));
    pose.r_forearm =
        p.elbow_bend_deg * weight * (0.6f + 0.4f * std::max(0.0f, -r));

    // Pelvis/torso: hips rotate with the legs, chest counter-rotates, mild
    // forward lean scales with weight. Bob fires twice per cycle (each
    // step), sway once.
    pose.hips_yaw = p.pelvis_twist_deg * weight * l;
    pose.chest_yaw = -p.torso_twist_deg * weight * l;
    pose.spine_pitch = p.lean_deg * weight;
    pose.hips_sway = p.pelvis_sway_m * weight * std::sin(phase + kPi * 0.5f);
    pose.root_bob = p.bob_m * weight * (0.5f - 0.5f * std::cos(2.0f * phase));

    return pose;
}

GaitPose evaluate_idle(float t)
{
    GaitPose pose;

    // Slow breathing through the chest, subtle weight shift, occasional
    // look-around. Amplitudes deliberately small so the blend to walking
    // stays smooth.
    pose.chest_pitch = 1.2f * std::sin(t * 2.0f * kPi * 0.25f);
    pose.head_yaw = 4.0f * std::sin(t * 0.31f);
    pose.neck_pitch = 0.6f * std::sin(t * 0.23f + 1.0f);
    pose.l_upperarm = 1.5f * std::sin(t * 0.8f);
    pose.r_upperarm = 1.5f * std::sin(t * 0.8f + kPi);
    pose.l_forearm = 4.0f;
    pose.r_forearm = 4.0f;
    pose.hips_sway = 0.008f * std::sin(t * 0.2f);

    return pose;
}

GaitPose blend(const GaitPose& a, const GaitPose& b, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    auto mix = [t](float x, float y) { return x + (y - x) * t; };

    GaitPose pose;
    pose.hips_yaw = mix(a.hips_yaw, b.hips_yaw);
    pose.hips_sway = mix(a.hips_sway, b.hips_sway);
    pose.spine_pitch = mix(a.spine_pitch, b.spine_pitch);
    pose.spine_yaw = mix(a.spine_yaw, b.spine_yaw);
    pose.chest_pitch = mix(a.chest_pitch, b.chest_pitch);
    pose.chest_yaw = mix(a.chest_yaw, b.chest_yaw);
    pose.neck_pitch = mix(a.neck_pitch, b.neck_pitch);
    pose.head_yaw = mix(a.head_yaw, b.head_yaw);
    pose.l_thigh = mix(a.l_thigh, b.l_thigh);
    pose.l_shin = mix(a.l_shin, b.l_shin);
    pose.l_foot = mix(a.l_foot, b.l_foot);
    pose.r_thigh = mix(a.r_thigh, b.r_thigh);
    pose.r_shin = mix(a.r_shin, b.r_shin);
    pose.r_foot = mix(a.r_foot, b.r_foot);
    pose.l_upperarm = mix(a.l_upperarm, b.l_upperarm);
    pose.l_forearm = mix(a.l_forearm, b.l_forearm);
    pose.r_upperarm = mix(a.r_upperarm, b.r_upperarm);
    pose.r_forearm = mix(a.r_forearm, b.r_forearm);
    pose.root_bob = mix(a.root_bob, b.root_bob);
    return pose;
}

float step_rate_for_speed(float speed, const GaitParams& p)
{
    return std::max(speed / p.stride_length, p.min_step_rate);
}

}  // namespace character

RUZINO_NAMESPACE_CLOSE_SCOPE
