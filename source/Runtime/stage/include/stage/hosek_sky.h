#pragma once

#include <string>

#include "api.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/usd/stage.h"

RUZINO_NAMESPACE_OPEN_SCOPE

// Event emitted on Stage::events() whenever a HosekWilkieSky prim's
// sun-driving attributes change. Payload: pxr::SdfPath of the sky prim.
inline constexpr const char* HOSEK_SKY_SUN_EDITED = "hosek_sky_sun_edited";

// One-way sun linkage for the HosekWilkieSky rig: recompute the world-space
// sun direction from the sky prim's inputs:sunDirection (dome-local, +Y up)
// and its world transform, then point the first DistantLight CHILD of the
// sky prim along it (renderer convention: the light's transform row 2 is
// the travel direction, i.e. AWAY from the sun).
//
// The child light's transform is authored as a single xformOp:transform
// matrix into `layer` (nullptr = the stage's session layer, i.e. the usual
// modifier-layer semantics persisted by the _modifiers.usda sidecar;
// scene-generation scripts pass the root layer instead). Nothing else on
// the light is touched. A rig without a DistantLight child is valid and
// returns true.
//
// Returns false with *err set when the stage/sky prim is missing or not a
// HosekWilkieSky.
STAGE_API bool sync_sun_light(
    const pxr::UsdStageRefPtr& stage,
    const pxr::SdfPath& sky_path,
    std::string* err = nullptr,
    const pxr::SdfLayerHandle& layer = nullptr);

// World-space TOWARD-sun direction of a HosekWilkieSky prim (its dome-local
// inputs:sunDirection mapped through the prim's world transform). Returns
// false when the prim is missing, of the wrong type, or the attribute is
// authored as a zero vector.
STAGE_API bool world_sun_direction(
    const pxr::UsdStageRefPtr& stage,
    const pxr::SdfPath& sky_path,
    pxr::GfVec3f& out_dir,
    std::string* err = nullptr);

RUZINO_NAMESPACE_CLOSE_SCOPE
