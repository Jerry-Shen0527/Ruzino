#ifdef GEOM_USD_EXTENSION

#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/relationshipSpec.h>
#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>

#include <string>

#include "GCore/Components/CurveComponent.h"
#include "GCore/Components/InstancerComponent.h"
#include "GCore/Components/MeshComponent.h"
#include "GCore/Components/PointsComponent.h"
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

// PointInstancer over-spec into an arbitrary modifier layer (Sdf-spec
// authoring, the write_geometry_as_over_spec style). Attributes carry
// DEFAULTS only — no time samples — so default-time reads (pxr Get(),
// Hydra ingest) resolve them. Mirrors the direct-write instancer layout in
// write_geometry_to_usd: instancer at `instancer_path`, prototype mesh
// expected at `proto_path`.
static bool write_instancer_over_spec(
    Geometry& geometry,
    pxr::SdfLayerHandle modifier_layer,
    const pxr::SdfPath& instancer_path,
    const pxr::SdfPath& proto_path)
{
    auto inst = geometry.get_const_component<InstancerComponent>();
    if (!inst) {
        return false;
    }
    if (!pxr::SdfJustCreatePrimInLayer(modifier_layer, instancer_path)) {
        spdlog::error(
            "[write_usd] instancer: failed to create prim at {}",
            instancer_path.GetString());
        return false;
    }
    auto prim_spec = modifier_layer->GetPrimAtPath(instancer_path);
    if (!prim_spec) {
        return false;
    }
    prim_spec->SetSpecifier(pxr::SdfSpecifierOver);
    prim_spec->SetTypeName(pxr::TfToken("PointInstancer"));

    const auto set_attr = [&prim_spec](
                              const pxr::TfToken& name,
                              const pxr::SdfValueTypeName& type,
                              const pxr::VtValue& value) {
        auto attr_spec = pxr::SdfAttributeSpec::New(
            prim_spec, name, type, pxr::SdfVariabilityVarying);
        return attr_spec && attr_spec->SetDefaultValue(value);
    };

    const auto& pos = inst->get_positions();
    const size_t n = pos.size();
    pxr::VtVec3fArray positions(n);
    for (size_t i = 0; i < n; ++i)
        positions[i] = pxr::GfVec3f(pos[i].x, pos[i].y, pos[i].z);
    if (!set_attr(
            pxr::TfToken("positions"),
            pxr::SdfValueTypeNames->Point3fArray,
            pxr::VtValue(positions))) {
        return false;
    }
    // Per-instance prototype selection when the component provides it;
    // all zeros otherwise (prototype 0 = the own mesh, legacy mode).
    pxr::VtIntArray proto_indices(n, 0);
    const auto& proto_idx_store = inst->get_proto_indices_storage();
    if (proto_idx_store.size() == n) {
        for (size_t i = 0; i < n; ++i)
            proto_indices[i] = proto_idx_store[i];
    }
    if (!set_attr(
            pxr::TfToken("protoIndices"),
            pxr::SdfValueTypeNames->IntArray,
            pxr::VtValue(proto_indices))) {
        return false;
    }

    if (inst->has_rotations_enabled()) {
        const auto& ori = inst->get_orientations();
        if (!ori.empty()) {
            pxr::VtQuathArray orientations(n);
            for (size_t i = 0; i < n && i < ori.size(); ++i)
                orientations[i] =
                    pxr::GfQuath(ori[i].w, ori[i].x, ori[i].y, ori[i].z);
            set_attr(
                pxr::TfToken("orientations"),
                pxr::SdfValueTypeNames->QuathArray,
                pxr::VtValue(orientations));
        }
    }
    const auto& scl = inst->get_scales();
    if (!scl.empty()) {
        pxr::VtVec3fArray scales(n);
        for (size_t i = 0; i < n && i < scl.size(); ++i)
            scales[i] = pxr::GfVec3f(scl[i].x, scl[i].y, scl[i].z);
        set_attr(
            pxr::TfToken("scales"),
            pxr::SdfValueTypeNames->Float3Array,
            pxr::VtValue(scales));
    }

    auto rel =
        pxr::SdfRelationshipSpec::New(prim_spec, pxr::TfToken("prototypes"));
    if (!rel) {
        return false;
    }
    // Multi-prototype: author each inline prototype as a child mesh
    // over-spec and target all of them. Legacy: target the single
    // own-mesh prototype path handed in by the caller.
    const auto& prototypes = inst->get_prototypes();
    if (!prototypes.empty()) {
        for (size_t pi = 0; pi < prototypes.size(); ++pi) {
            const auto& proto = prototypes[pi];
            auto proto_child = instancer_path.AppendChild(
                pxr::TfToken("Prototype_" + std::to_string(pi)));
            if (!pxr::SdfJustCreatePrimInLayer(modifier_layer, proto_child)) {
                return false;
            }
            auto child_spec = modifier_layer->GetPrimAtPath(proto_child);
            if (!child_spec) {
                return false;
            }
            child_spec->SetSpecifier(pxr::SdfSpecifierOver);
            child_spec->SetTypeName(pxr::TfToken("Mesh"));

            pxr::VtVec3fArray pts(proto.vertices.size());
            for (size_t i = 0; i < proto.vertices.size(); ++i)
                pts[i] = pxr::GfVec3f(
                    proto.vertices[i].x,
                    proto.vertices[i].y,
                    proto.vertices[i].z);
            pxr::VtIntArray fvc(
                proto.face_vertex_counts.begin(),
                proto.face_vertex_counts.end());
            pxr::VtIntArray fvi(
                proto.face_vertex_indices.begin(),
                proto.face_vertex_indices.end());
            auto set_child_attr = [&child_spec](
                                      const pxr::TfToken& name,
                                      const pxr::SdfValueTypeName& type,
                                      const pxr::VtValue& value) {
                auto attr_spec = pxr::SdfAttributeSpec::New(
                    child_spec, name, type, pxr::SdfVariabilityVarying);
                return attr_spec && attr_spec->SetDefaultValue(value);
            };
            if (!set_child_attr(
                    pxr::TfToken("points"),
                    pxr::SdfValueTypeNames->Point3fArray,
                    pxr::VtValue(pts)) ||
                !set_child_attr(
                    pxr::TfToken("faceVertexCounts"),
                    pxr::SdfValueTypeNames->IntArray,
                    pxr::VtValue(fvc)) ||
                !set_child_attr(
                    pxr::TfToken("faceVertexIndices"),
                    pxr::SdfValueTypeNames->IntArray,
                    pxr::VtValue(fvi))) {
                return false;
            }
            rel->GetTargetPathList().Add(proto_child);
        }
    }
    else {
        rel->GetTargetPathList().Add(proto_path);
    }

    spdlog::info(
        "[write_usd] instancer: {} instances of {} at {}",
        n,
        proto_path.GetString(),
        instancer_path.GetString());
    return true;
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

    auto curve = geometry.get_component<CurveComponent>();
    auto mesh_c = geometry.get_component<MeshComponent>();
    auto pts_c = geometry.get_component<PointsComponent>();
    spdlog::info(
        "[write_usd] input geometry: mesh={}, curve={}, points={}",
        (bool)mesh_c,
        (bool)curve,
        curve ? curve->get_vertices().size() : 0);

    pxr::UsdTimeCode time = global_payload.current_time;

    pxr::UsdStageRefPtr stage = global_payload.stage;
    pxr::SdfPath sdf_path = global_payload.prim_path;

    spdlog::info(
        "[write_usd] called: prim_path='{}', is_modifier_mode={}",
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
        // Curves and points use direct write (modifier over spec doesn't
        // handle them). A multi-prototype instancer carries its own
        // meshes, so it goes through the over-spec path without an
        // own-mesh to park.
        bool is_mesh = geometry.get_component<MeshComponent>() != nullptr;
        const bool multi_proto_inst = [&]() -> bool {
            auto inst = geometry.get_const_component<InstancerComponent>();
            return inst && !inst->get_prototypes().empty();
        }();
        if (!is_mesh && !multi_proto_inst) {
            spdlog::info(
                "[write_usd] non-mesh, direct write to '{}'",
                sdf_path.GetString());
            write_success =
                write_geometry_to_usd(geometry, stage, sdf_path, time);
            spdlog::info(
                "[write_usd] write_geometry_to_usd result: {}", write_success);
        }
        else if (multi_proto_inst) {
            write_success = write_instancer_over_spec(
                geometry, modifier_layer, sdf_path, sdf_path);
            spdlog::info(
                "[write_usd] multi-proto instancer over-spec result: {}",
                write_success);
        }
        else {
            pxr::SdfPath output_path =
                global_payload.modifier_output_path.IsEmpty()
                    ? sdf_path
                    : global_payload.modifier_output_path;

            spdlog::debug(
                "[MODIFIER] Writing to modifier layer, output_path='{}'",
                output_path.GetString());

            // Instanced meshes render through a PointInstancer: park the
            // mesh at <path>/Prototype and author the instancer at <path>
            // (same layout as write_geometry_to_usd's direct-write branch).
            const bool instanced =
                geometry.get_component<InstancerComponent>() != nullptr;
            const pxr::SdfPath mesh_path =
                instanced ? output_path.AppendPath(pxr::SdfPath("Prototype"))
                          : output_path;

            write_success = write_geometry_as_over_spec(
                geometry, stage, mesh_path, time, modifier_layer);

            spdlog::info(
                "[write_usd] write_geometry_as_over_spec result: {}, path={}",
                write_success,
                mesh_path.GetString());

            if (write_success && instanced) {
                write_success = write_instancer_over_spec(
                    geometry, modifier_layer, output_path, mesh_path);
            }

            // Set Animatable attribute on root layer (not modifier layer)
            pxr::UsdPrim prim = stage->GetPrimAtPath(sdf_path);
            if (prim) {
                prim.CreateAttribute(
                        pxr::TfToken("Animatable"),
                        pxr::SdfValueTypeNames->Bool)
                    .Set(global_payload.has_simulation);
            }
        }
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
