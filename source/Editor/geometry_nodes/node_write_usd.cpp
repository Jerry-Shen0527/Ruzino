#ifdef GEOM_USD_EXTENSION

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>

#include <string>

#include "GCore/Components/CurveComponent.h"
#include "GCore/geom_payload.hpp"
#include "GCore/usd_extension.h"
#include "geom_node_base.h"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE
bool legal(const std::string& string)
{
    if (string.empty()) {
        return false;
    }
    if (std::find_if(string.begin(), string.end(), [](char val) {
            return val == '(' || val == ')' || val == ',';
        }) == string.end()) {
        return true;
    }
    return false;
}

static pxr::SdfLayerHandle get_or_create_modifier_layer(
    pxr::UsdStageRefPtr stage,
    const pxr::SdfPath& prim_path)
{
    // Use session layer for modifiers - it's automatically part of the stage
    // and persists during the session, but won't be saved to disk
    auto session_layer = stage->GetSessionLayer();

    if (session_layer) {
        spdlog::debug(
            "[MODIFIER] Using session layer for prim '{}', layer ID: {}",
            prim_path.GetString(),
            session_layer->GetIdentifier());
        return session_layer;
    }

    // Fallback: create anonymous layer
    auto modifier_layer =
        pxr::SdfLayer::CreateAnonymous("modifier_layer_" + prim_path.GetName());

    spdlog::debug(
        "[MODIFIER] Created anonymous modifier layer for prim '{}': {}",
        prim_path.GetString(),
        modifier_layer->GetIdentifier());

    return modifier_layer;
}

NODE_DECLARATION_FUNCTION(write_usd)
{
    b.add_input<Geometry>("Geometry");
    b.add_input<std::string>("Sub Path").optional(true);
}

NODE_EXECUTION_FUNCTION(write_usd)
{
    auto& global_payload = params.get_global_payload<GeomPayload&>();

    auto geometry = params.get_input<Geometry>("Geometry");

    pxr::UsdTimeCode time = global_payload.current_time;

    pxr::UsdStageRefPtr stage = global_payload.stage;
    pxr::SdfPath sdf_path = global_payload.prim_path;

    spdlog::debug(
        "[MODIFIER] write_usd called: prim_path='{}', "
        "is_modifier_mode={}",
        sdf_path.GetString(),
        global_payload.is_modifier_mode);

    auto sub_path = params.get_input<std::string>("Sub Path");
    if (!std::string(sub_path.c_str()).empty()) {
        if (!legal(sub_path)) {
            spdlog::error("[write_usd] Illegal sub path");
            return false;
        }
        sdf_path = sdf_path.AppendPath(pxr::SdfPath(sub_path.c_str()));
    }

    bool write_success = false;

    pxr::SdfLayerHandle modifier_layer = global_payload.modifier_layer;
    if (!modifier_layer && stage) {
        modifier_layer = get_or_create_modifier_layer(stage, sdf_path);
    }

    if (modifier_layer) {
        pxr::SdfPath output_path = global_payload.modifier_output_path.IsEmpty()
                                       ? sdf_path
                                       : global_payload.modifier_output_path;

        spdlog::debug(
            "[MODIFIER] Writing to modifier layer, output_path='{}'",
            output_path.GetString());

        write_success = write_geometry_as_over_spec(
            geometry, stage, output_path, time, modifier_layer);

        spdlog::debug(
            "[MODIFIER] write_geometry_as_over_spec result: {}", write_success);
    }
    else {
        spdlog::debug(
            "[MODIFIER] No modifier layer available, using direct write");
        write_success = write_geometry_to_usd(geometry, stage, sdf_path, time);

        if (global_payload.has_simulation) {
            pxr::UsdPrim prim = stage->GetPrimAtPath(sdf_path);
            if (prim) {
                prim.CreateAttribute(
                        pxr::TfToken("Animatable"),
                        pxr::SdfValueTypeNames->Bool)
                    .Set(true);
            }
        }
        else {
            pxr::UsdPrim prim = stage->GetPrimAtPath(sdf_path);
            if (prim) {
                prim.CreateAttribute(
                        pxr::TfToken("Animatable"),
                        pxr::SdfValueTypeNames->Bool)
                    .Set(false);
            }
        }
    }

    if (!write_success) {
        spdlog::error("[write_usd] Failed to write geometry");
        return false;
    }

    if (stage) {
        pxr::SdfPath visible_path =
            global_payload.modifier_output_path.IsEmpty()
                ? sdf_path
                : global_payload.modifier_output_path;

        auto prim = stage->GetPrimAtPath(visible_path);
        if (prim) {
            pxr::UsdGeomImageable(prim).MakeVisible();
        }
    }

    return true;
}

NODE_DECLARATION_REQUIRED(write_usd);

NODE_DECLARATION_UI(write_usd);
NODE_DEF_CLOSE_SCOPE
#endif
