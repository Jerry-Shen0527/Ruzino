#pragma once

#include <string>
#include <vector>

#include "api.h"
#include "character/humanoid_roles.h"
#include "pxr/base/gf/matrix4f.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/prim.h"

RUZINO_NAMESPACE_OPEN_SCOPE

namespace character {

// One discovered joint of a USD-authored skeleton.
struct Joint {
    pxr::SdfPath path;
    std::string name;
    HumanoidRole role = HumanoidRole::Unknown;

    // Rest pose authored in the scene (root layer), captured once when the
    // skeleton is discovered. The controller re-authors translate/rotate
    // every frame as rest + gait offsets, so the original pose survives.
    pxr::GfVec3f rest_translation = pxr::GfVec3f(0.0f);
    pxr::GfVec3f rest_rotation_euler = pxr::GfVec3f(0.0f);  // XYZ, degrees
};

// A skeleton read from a USD prim hierarchy. The character root prim (marked
// with the `character:controller` attribute) contains one Xform prim per
// joint; leaf Mesh prims hang below their driving joint and follow it for
// free through USD instancing of the parent transform — the renderer needs
// no skinning support at all.
//
// Joint order is depth-first (parents before children), matching USD
// traversal, which is also a valid forward-kinematics evaluation order. The
// interface intentionally exposes joint world matrices (evaluate_fk) so a
// future GPU-skinning pipeline can consume the same rig as a matrix palette
// without changing how scenes are authored.
class CHARACTER_API Skeleton {
   public:
    // Walks the prim subtree below `character_root` (excluding the root
    // itself) and collects every Xform prim as a joint. Returns false when
    // no joint was found (likely not a rig).
    bool build_from_prim(const pxr::UsdPrim& character_root);

    const std::vector<Joint>& joints() const
    {
        return joints_;
    }
    const Joint* find_by_role(HumanoidRole role) const;

    // Evaluates rest-pose-relative local transforms into a flat matrix
    // palette (world space, one matrix per joint, same order as joints()).
    // `root_matrix` is the character root prim's world transform. Intended
    // as the entry point for a future skinning pipeline; the walking demo
    // itself only needs per-joint local rotations.
    std::vector<pxr::GfMatrix4f> evaluate_fk(
        const std::vector<pxr::GfVec3f>& local_translations,
        const std::vector<pxr::GfVec3f>& local_rotations_euler,
        const pxr::GfMatrix4f& root_matrix) const;

   private:
    std::vector<Joint> joints_;
};

}  // namespace character

RUZINO_NAMESPACE_CLOSE_SCOPE
