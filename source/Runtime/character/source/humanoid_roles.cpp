#include "character/humanoid_roles.h"

#include <algorithm>
#include <cctype>

RUZINO_NAMESPACE_OPEN_SCOPE

namespace character {

HumanoidRole humanoid_role_from_name(const std::string& name)
{
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) {
        lower.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (lower == "hips" || lower == "pelvis")
        return HumanoidRole::Hips;
    if (lower == "spine" || lower == "spine1")
        return HumanoidRole::Spine;
    if (lower == "chest" || lower == "spine2")
        return HumanoidRole::Chest;
    if (lower == "neck")
        return HumanoidRole::Neck;
    if (lower == "head")
        return HumanoidRole::Head;

    if (lower == "l_upperarm" || lower == "leftupperarm" || lower == "l_arm")
        return HumanoidRole::L_UpperArm;
    if (lower == "l_forearm" || lower == "leftforearm" || lower == "l_elbow")
        return HumanoidRole::L_Forearm;
    if (lower == "l_hand" || lower == "lefthand")
        return HumanoidRole::L_Hand;
    if (lower == "r_upperarm" || lower == "rightupperarm" || lower == "r_arm")
        return HumanoidRole::R_UpperArm;
    if (lower == "r_forearm" || lower == "rightforearm" || lower == "r_elbow")
        return HumanoidRole::R_Forearm;
    if (lower == "r_hand" || lower == "righthand")
        return HumanoidRole::R_Hand;

    if (lower == "l_thigh" || lower == "leftupleg" || lower == "l_hip" ||
        lower == "l_leg")
        return HumanoidRole::L_Thigh;
    if (lower == "l_shin" || lower == "leftleg" || lower == "l_knee")
        return HumanoidRole::L_Shin;
    if (lower == "l_foot" || lower == "leftfoot" || lower == "l_ankle")
        return HumanoidRole::L_Foot;
    if (lower == "r_thigh" || lower == "rightupleg" || lower == "r_hip" ||
        lower == "r_leg")
        return HumanoidRole::R_Thigh;
    if (lower == "r_shin" || lower == "rightleg" || lower == "r_knee")
        return HumanoidRole::R_Shin;
    if (lower == "r_foot" || lower == "rightfoot" || lower == "r_ankle")
        return HumanoidRole::R_Foot;

    return HumanoidRole::Unknown;
}

const char* humanoid_role_name(HumanoidRole role)
{
    switch (role) {
        case HumanoidRole::Hips: return "Hips";
        case HumanoidRole::Spine: return "Spine";
        case HumanoidRole::Chest: return "Chest";
        case HumanoidRole::Neck: return "Neck";
        case HumanoidRole::Head: return "Head";
        case HumanoidRole::L_UpperArm: return "L_UpperArm";
        case HumanoidRole::L_Forearm: return "L_Forearm";
        case HumanoidRole::L_Hand: return "L_Hand";
        case HumanoidRole::R_UpperArm: return "R_UpperArm";
        case HumanoidRole::R_Forearm: return "R_Forearm";
        case HumanoidRole::R_Hand: return "R_Hand";
        case HumanoidRole::L_Thigh: return "L_Thigh";
        case HumanoidRole::L_Shin: return "L_Shin";
        case HumanoidRole::L_Foot: return "L_Foot";
        case HumanoidRole::R_Thigh: return "R_Thigh";
        case HumanoidRole::R_Shin: return "R_Shin";
        case HumanoidRole::R_Foot: return "R_Foot";
        default: return "Unknown";
    }
}

}  // namespace character

RUZINO_NAMESPACE_CLOSE_SCOPE
