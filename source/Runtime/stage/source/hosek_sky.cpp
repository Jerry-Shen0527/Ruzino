#include "stage/hosek_sky.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/editTarget.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <spdlog/spdlog.h>

RUZINO_NAMESPACE_OPEN_SCOPE

namespace {

inline const pxr::TfToken hosek_type_token{ "HosekWilkieSky" };
inline const pxr::TfToken sun_direction_attr{ "inputs:sunDirection" };

}  // namespace

bool world_sun_direction(
    const pxr::UsdStageRefPtr& stage,
    const pxr::SdfPath& sky_path,
    pxr::GfVec3f& out_dir,
    std::string* err)
{
    auto set_err = [&](const std::string& msg) {
        if (err)
            *err = msg;
        return false;
    };

    if (!stage) {
        return set_err("stage is null");
    }
    pxr::UsdPrim sky = stage->GetPrimAtPath(sky_path);
    if (!sky) {
        return set_err("prim not found: " + sky_path.GetString());
    }
    if (sky.GetTypeName() != hosek_type_token) {
        return set_err(
            "prim is not a HosekWilkieSky: " + sky_path.GetString() + " (" +
            sky.GetTypeName().GetString() + ")");
    }

    pxr::UsdAttribute attr = sky.GetAttribute(sun_direction_attr);
    pxr::GfVec3f sun_local(0.5f, 0.7f, 0.5f);
    if (attr && attr.HasAuthoredValue()) {
        attr.Get<pxr::GfVec3f>(&sun_local, pxr::UsdTimeCode::Default());
    }

    // The dome evaluates in its LOCAL frame; the same world->local mapping
    // the sky shader applies means the inverse maps the local sun back to
    // world space.
    pxr::UsdGeomXformable xform(sky);
    pxr::GfMatrix4d world_xf =
        xform.ComputeLocalToWorldTransform(pxr::UsdTimeCode::Default());
    pxr::GfVec3d world(sun_local[0], sun_local[1], sun_local[2]);
    world = world_xf.GetInverse().TransformDir(world);

    pxr::GfVec3f dir(
        static_cast<float>(world[0]),
        static_cast<float>(world[1]),
        static_cast<float>(world[2]));
    float len = dir.GetLength();
    if (len < 1e-6f) {
        return set_err("sunDirection resolves to a zero vector in world space");
    }
    out_dir = dir / len;
    return true;
}

bool sync_sun_light(
    const pxr::UsdStageRefPtr& stage,
    const pxr::SdfPath& sky_path,
    std::string* err,
    const pxr::SdfLayerHandle& layer)
{
    auto set_err = [&](const std::string& msg) {
        if (err)
            *err = msg;
        return false;
    };

    if (!stage) {
        return set_err("stage is null");
    }
    pxr::UsdPrim sky = stage->GetPrimAtPath(sky_path);
    if (!sky) {
        return set_err("prim not found: " + sky_path.GetString());
    }
    if (sky.GetTypeName() != hosek_type_token) {
        return set_err(
            "prim is not a HosekWilkieSky: " + sky_path.GetString() + " (" +
            sky.GetTypeName().GetString() + ")");
    }

    // Find the rig's sun: the first direct DistantLight child. Structure is
    // the contract — anything else in the stage is never touched.
    pxr::UsdPrim sun;
    for (const pxr::UsdPrim& child : sky.GetAllChildren()) {
        if (child.GetTypeName() == pxr::TfToken("DistantLight")) {
            sun = child;
            break;
        }
    }
    if (!sun) {
        // A sky without a sun child is a valid rig.
        return true;
    }

    pxr::GfVec3f world_sun;
    if (!world_sun_direction(stage, sky_path, world_sun, err)) {
        return false;
    }

    // The renderer reads the DistantLight's travel direction from row 2 of
    // its WORLD transform (= away from the sun). Hydra hands the delegate
    // the composed local-to-world matrix, so the child's LOCAL rotation must
    // cancel its parent chain: express -world_sun in the light's parent
    // frame and rotate local +Z onto that. For a light directly under the
    // sky this is simply the dome-local -sunDirection.
    pxr::UsdGeomXformable sun_xform(sun);
    pxr::GfMatrix4d parent_xf =
        sun_xform.ComputeParentToWorldTransform(pxr::UsdTimeCode::Default());
    pxr::GfVec3d local_travel = parent_xf.GetInverse().TransformDir(
        pxr::GfVec3d(-world_sun[0], -world_sun[1], -world_sun[2]));
    {
        double len = local_travel.GetLength();
        if (len < 1e-9) {
            return set_err(
                "sun direction degenerate in the light's parent frame");
        }
        local_travel /= len;
    }

    // Row-vector convention: (0,0,1) * M picks row 2 of M.
    pxr::GfMatrix4d light_xf(1.0);
    light_xf.SetRotate(
        pxr::GfRotation(pxr::GfVec3d(0.0, 0.0, 1.0), local_travel));

    // Single matrix xformOp into the session (modifier) layer — same
    // non-destructive convention as every editor-side transform authoring.

    // Early-out: every edit to the sky prim (even an unrelated attribute)
    // re-runs this sync; skip the authoring when the child already points
    // exactly where the sky says. Read-only check — do not author anything
    // outside the session-layer edit below.
    pxr::UsdAttribute xform_attr =
        sun.GetAttribute(pxr::TfToken("xformOp:transform"));
    pxr::GfMatrix4d current;
    if (xform_attr &&
        xform_attr.Get<pxr::GfMatrix4d>(
            &current, pxr::UsdTimeCode::Default()) &&
        current == light_xf) {
        return true;
    }

    pxr::SdfLayerHandle target_layer = layer ? layer : stage->GetSessionLayer();
    pxr::UsdEditTarget previous = stage->GetEditTarget();
    stage->SetEditTarget(pxr::UsdEditTarget(target_layer));
    sun_xform.ClearXformOpOrder();
    pxr::UsdGeomXformOp op = sun_xform.MakeMatrixXform();
    bool ok = op.Set(light_xf, pxr::UsdTimeCode::Default());
    stage->SetEditTarget(previous);

    if (!ok) {
        return set_err(
            "failed to author xformOp on " + sun.GetPath().GetString());
    }
    spdlog::info(
        "[hosek_sky] synced sun light {}: world toward-sun = ({:.3f}, {:.3f}, "
        "{:.3f})",
        sun.GetPath().GetString(),
        world_sun[0],
        world_sun[1],
        world_sun[2]);
    return true;
}

RUZINO_NAMESPACE_CLOSE_SCOPE
