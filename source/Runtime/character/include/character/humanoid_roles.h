#pragma once

#include <string>

#include "api.h"

RUZINO_NAMESPACE_OPEN_SCOPE

namespace character {

// Semantic roles for the joints of a humanoid rig. The skeleton is authored
// directly in USD as a prim hierarchy (one Xform prim per joint, e.g.
// /Character/Hips/L_Thigh); each joint prim's *name* selects its role. Joints
// whose name has no role keep their rest pose but are otherwise ignored by
// the gait evaluator, so rigs can carry extra bones (twist bones, props...)
// without confusing the controller.
enum class HumanoidRole {
    Unknown = 0,
    Hips,
    Spine,
    Chest,
    Neck,
    Head,
    L_UpperArm,
    L_Forearm,
    L_Hand,
    R_UpperArm,
    R_Forearm,
    R_Hand,
    L_Thigh,
    L_Shin,
    L_Foot,
    R_Thigh,
    R_Shin,
    R_Foot,
    Count
};

// Maps a USD joint prim name (case-insensitive) to its role. Accepts the
// plain name ("Hips", "L_Thigh", ...); returns Unknown when unmatched.
CHARACTER_API HumanoidRole humanoid_role_from_name(const std::string& name);

// Canonical name for a role (e.g. HumanoidRole::L_Thigh -> "L_Thigh").
CHARACTER_API const char* humanoid_role_name(HumanoidRole role);

}  // namespace character

RUZINO_NAMESPACE_CLOSE_SCOPE
