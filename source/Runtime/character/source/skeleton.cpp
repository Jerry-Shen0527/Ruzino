#include "character/skeleton.h"

#include <pxr/base/gf/rotation.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>

#include <functional>

RUZINO_NAMESPACE_OPEN_SCOPE

namespace character {

namespace {

    Joint collect_joint(const pxr::UsdPrim& prim)
    {
        Joint joint;
        joint.path = prim.GetPath();
        joint.name = prim.GetName().GetString();
        joint.role = humanoid_role_from_name(joint.name);

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
        joint.rest_translation = pxr::GfVec3f(
            static_cast<float>(translation[0]),
            static_cast<float>(translation[1]),
            static_cast<float>(translation[2]));
        joint.rest_rotation_euler = rotation;
        return joint;
    }

}  // namespace

bool Skeleton::build_from_prim(const pxr::UsdPrim& character_root)
{
    joints_.clear();
    if (!character_root || !character_root.IsA<pxr::UsdGeomXformable>())
        return false;

    // Depth-first collection: a joint always precedes its children, which is
    // also the order evaluate_fk relies on (parent world matrices are
    // complete before their children consume them).
    std::function<void(const pxr::UsdPrim&)> collect =
        [&](const pxr::UsdPrim& parent) {
            for (pxr::UsdPrim child : parent.GetAllChildren()) {
                if (!child.IsA<pxr::UsdGeomXform>())
                    continue;
                joints_.push_back(collect_joint(child));
                collect(child);
            }
        };
    collect(character_root);

    return !joints_.empty();
}

const Joint* Skeleton::find_by_role(HumanoidRole role) const
{
    for (const auto& joint : joints_) {
        if (joint.role == role)
            return &joint;
    }
    return nullptr;
}

std::vector<pxr::GfMatrix4f> Skeleton::evaluate_fk(
    const std::vector<pxr::GfVec3f>& local_translations,
    const std::vector<pxr::GfVec3f>& local_rotations_euler,
    const pxr::GfMatrix4f& root_matrix) const
{
    std::vector<pxr::GfMatrix4f> world(joints_.size());

    for (size_t i = 0; i < joints_.size(); ++i) {
        pxr::GfVec3f t = i < local_translations.size()
                             ? local_translations[i]
                             : joints_[i].rest_translation;
        pxr::GfVec3f r = i < local_rotations_euler.size()
                             ? local_rotations_euler[i]
                             : pxr::GfVec3f(0.0f);

        // Rigid transform in Gf's row-vector convention: rotate first, then
        // translate (R * T — T * R would rotate the translation itself).
        pxr::GfMatrix4f translation_matrix(1.0f);
        translation_matrix.SetTranslate(t);
        pxr::GfRotation rotation(pxr::GfVec3d(1, 0, 0), r[0]);
        rotation *= pxr::GfRotation(pxr::GfVec3d(0, 1, 0), r[1]);
        rotation *= pxr::GfRotation(pxr::GfVec3d(0, 0, 1), r[2]);
        pxr::GfMatrix4d rotation_matrix(1.0);
        rotation_matrix.SetRotate(rotation);
        pxr::GfMatrix4f local =
            pxr::GfMatrix4f(rotation_matrix) * translation_matrix;

        size_t parent = static_cast<size_t>(-1);
        pxr::SdfPath parent_path = joints_[i].path.GetParentPath();
        for (size_t j = 0; j < i; ++j) {
            if (joints_[j].path == parent_path) {
                parent = j;
                break;
            }
        }
        world[i] = (parent == static_cast<size_t>(-1)) ? root_matrix * local
                                                       : world[parent] * local;
    }

    return world;
}

}  // namespace character

RUZINO_NAMESPACE_CLOSE_SCOPE
