#ifdef GEOM_USD_EXTENSION

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usdGeom/mesh.h>

#include "GCore/Components/MeshComponent.h"
#include "GCore/Components/MeshViews.h"
#include "GCore/usd_extension.h"
#include "geom_node_base.h"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

static bool read_attr_from_layer(
    const pxr::SdfLayerHandle& layer,
    const pxr::SdfPath& prim_path,
    const std::string& attr_name,
    pxr::VtValue& out_value,
    pxr::UsdTimeCode time)
{
    auto prim_spec = layer->GetPrimAtPath(prim_path);
    if (!prim_spec)
        return false;

    auto attrs = prim_spec->GetAttributes();
    for (auto it = attrs.begin(); it != attrs.end(); ++it) {
        if ((*it)->GetName() == attr_name) {
            auto attr = *it;
            if (attr->HasDefaultValue()) {
                out_value = attr->GetDefaultValue();
                return true;
            }
            else if (attr->HasInfo(pxr::SdfFieldKeys->TimeSamples)) {
                layer->QueryTimeSample(
                    attr->GetPath(), time.GetValue(), &out_value);
                return !out_value.IsEmpty();
            }
        }
    }
    return false;
}

static bool generate_geometry_from_root_layer(
    Geometry& geometry,
    const pxr::SdfLayerHandle& root_layer,
    const pxr::SdfPath& prim_path,
    pxr::UsdTimeCode time)
{
    auto prim_spec = root_layer->GetPrimAtPath(prim_path);
    if (!prim_spec)
        return false;

    auto type_name = prim_spec->GetTypeName().GetString();
    spdlog::debug(
        "[input_geometry] Generating geometry for parametric prim type: {}",
        type_name);

    ParametricShapeParams params;

    pxr::VtValue radius_val, height_val, size_val;
    read_attr_from_layer(root_layer, prim_path, "radius", radius_val, time);
    read_attr_from_layer(root_layer, prim_path, "height", height_val, time);
    read_attr_from_layer(root_layer, prim_path, "size", size_val, time);

    if (radius_val.IsHolding<double>())
        params.radius = radius_val.Get<double>();
    else if (radius_val.IsHolding<float>())
        params.radius = radius_val.Get<float>();

    if (height_val.IsHolding<double>())
        params.height = height_val.Get<double>();
    else if (height_val.IsHolding<float>())
        params.height = height_val.Get<float>();

    if (size_val.IsHolding<double>())
        params.size = size_val.Get<double>();
    else if (size_val.IsHolding<float>())
        params.size = size_val.Get<float>();

    pxr::VtArray<pxr::GfVec3f> points;
    pxr::VtArray<int> face_counts;
    pxr::VtArray<int> face_indices;
    pxr::VtArray<pxr::GfVec3f> normals;

    if (!generate_parametric_mesh(
            type_name, params, points, face_counts, face_indices, normals)) {
        return false;
    }

    auto mesh_comp = geometry.get_component<MeshComponent>();
    if (!mesh_comp) {
        mesh_comp = std::make_shared<MeshComponent>(&geometry);
        geometry.attach_component(mesh_comp);
    }
    auto mesh_view = get_usd_view(*mesh_comp);

    mesh_view.set_vertices(points);
    mesh_view.set_face_topology(face_counts, face_indices);
    if (!normals.empty()) {
        mesh_view.set_normals(normals);
    }

    return true;
}

NODE_DECLARATION_FUNCTION(input_geometry)
{
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(input_geometry)
{
    auto& global_payload = params.get_global_payload<GeomPayload&>();

    if (!global_payload.stage) {
        spdlog::error("[input_geometry] No stage in global payload");
        return false;
    }

    pxr::SdfPath input_path = global_payload.modifier_input_path.IsEmpty()
                                  ? global_payload.prim_path
                                  : global_payload.modifier_input_path;

    Geometry geometry;

    bool should_read_from_root = false;

    if (global_payload.is_modifier_mode) {
        should_read_from_root = (global_payload.current_modifier_index == 0);
        spdlog::warn(
            "[input_geometry] is_modifier_mode={}, current_modifier_index={}, "
            "should_read_from_root={}, prim_path={}",
            global_payload.is_modifier_mode,
            global_payload.current_modifier_index,
            should_read_from_root,
            input_path.GetString());
    }
    else {
        spdlog::warn(
            "[input_geometry] NOT in modifier mode, prim_path={}",
            input_path.GetString());
        auto session_layer = global_payload.stage->GetSessionLayer();
        if (session_layer) {
            auto session_prim_spec = session_layer->GetPrimAtPath(input_path);
            if (session_prim_spec) {
                should_read_from_root = true;
                spdlog::warn(
                    "[input_geometry] Non-modifier mode but session layer has "
                    "over specs, reading from root layer");
            }
        }
    }

    if (should_read_from_root) {
        auto root_layer = global_payload.stage->GetRootLayer();
        auto root_prim_spec = root_layer->GetPrimAtPath(input_path);

        if (root_prim_spec) {
            auto prim = global_payload.stage->GetPrimAtPath(input_path);
            if (!prim) {
                spdlog::error(
                    "[input_geometry] Prim not found in stage: {}",
                    input_path.GetString());
                return false;
            }

            pxr::SdfAttributeSpecHandle points_attr_spec;
            pxr::SdfAttributeSpecHandle fvc_attr_spec;
            pxr::SdfAttributeSpecHandle fvi_attr_spec;
            pxr::SdfAttributeSpecHandle normals_attr_spec;

            auto attrs = root_prim_spec->GetAttributes();
            for (auto it = attrs.begin(); it != attrs.end(); ++it) {
                auto attr = *it;
                const auto& name = attr->GetName();
                if (name == "points") {
                    points_attr_spec = attr;
                }
                else if (name == "faceVertexCounts") {
                    fvc_attr_spec = attr;
                }
                else if (name == "faceVertexIndices") {
                    fvi_attr_spec = attr;
                }
                else if (name == "normals") {
                    normals_attr_spec = attr;
                }
            }

            if (points_attr_spec) {
                auto mesh_comp = geometry.get_component<MeshComponent>();
                if (!mesh_comp) {
                    mesh_comp = std::make_shared<MeshComponent>(&geometry);
                    geometry.attach_component(mesh_comp);
                }
                auto mesh_view = get_usd_view(*mesh_comp);

                pxr::VtValue points_val;
                if (points_attr_spec->HasDefaultValue()) {
                    points_val = points_attr_spec->GetDefaultValue();
                }
                else if (points_attr_spec->HasInfo(
                             pxr::SdfFieldKeys->TimeSamples)) {
                    root_layer->QueryTimeSample(
                        points_attr_spec->GetPath(),
                        global_payload.current_time.GetValue(),
                        &points_val);
                }

                if (!points_val.IsEmpty() &&
                    points_val.IsHolding<pxr::VtArray<pxr::GfVec3f>>()) {
                    mesh_view.set_vertices(
                        points_val.Get<pxr::VtArray<pxr::GfVec3f>>());
                }

                pxr::VtValue fvc_val;
                if (fvc_attr_spec) {
                    if (fvc_attr_spec->HasDefaultValue()) {
                        fvc_val = fvc_attr_spec->GetDefaultValue();
                    }
                    else if (fvc_attr_spec->HasInfo(
                                 pxr::SdfFieldKeys->TimeSamples)) {
                        root_layer->QueryTimeSample(
                            fvc_attr_spec->GetPath(),
                            global_payload.current_time.GetValue(),
                            &fvc_val);
                    }
                }

                pxr::VtValue fvi_val;
                if (fvi_attr_spec) {
                    if (fvi_attr_spec->HasDefaultValue()) {
                        fvi_val = fvi_attr_spec->GetDefaultValue();
                    }
                    else if (fvi_attr_spec->HasInfo(
                                 pxr::SdfFieldKeys->TimeSamples)) {
                        root_layer->QueryTimeSample(
                            fvi_attr_spec->GetPath(),
                            global_payload.current_time.GetValue(),
                            &fvi_val);
                    }
                }

                if (fvc_val.IsHolding<pxr::VtArray<int>>()) {
                    mesh_view.set_face_topology(
                        fvc_val.Get<pxr::VtArray<int>>(),
                        fvi_val.IsHolding<pxr::VtArray<int>>()
                            ? fvi_val.Get<pxr::VtArray<int>>()
                            : pxr::VtArray<int>());
                }

                pxr::VtValue normals_val;
                if (normals_attr_spec) {
                    if (normals_attr_spec->HasDefaultValue()) {
                        normals_val = normals_attr_spec->GetDefaultValue();
                    }
                    else if (normals_attr_spec->HasInfo(
                                 pxr::SdfFieldKeys->TimeSamples)) {
                        root_layer->QueryTimeSample(
                            normals_attr_spec->GetPath(),
                            global_payload.current_time.GetValue(),
                            &normals_val);
                    }
                }
                if (!normals_val.IsEmpty() &&
                    normals_val.IsHolding<pxr::VtArray<pxr::GfVec3f>>()) {
                    mesh_view.set_normals(
                        normals_val.Get<pxr::VtArray<pxr::GfVec3f>>());
                }

                spdlog::debug(
                    "[input_geometry] Read {} vertices from root layer",
                    mesh_view.get_vertices().size());
            }
            else {
                if (!generate_geometry_from_root_layer(
                        geometry,
                        root_layer,
                        input_path,
                        global_payload.current_time)) {
                    auto prim = global_payload.stage->GetPrimAtPath(input_path);
                    if (!prim ||
                        !read_geometry_from_usd(
                            geometry, prim, global_payload.current_time)) {
                        spdlog::error(
                            "[input_geometry] Failed to generate geometry: {}",
                            input_path.GetString());
                        return false;
                    }
                }

                auto mesh_comp = geometry.get_component<MeshComponent>();
                if (mesh_comp) {
                    spdlog::debug(
                        "[input_geometry] Generated {} vertices for parametric "
                        "prim",
                        mesh_comp->get_vertices().size());
                }
            }
        }
        else {
            spdlog::debug(
                "[input_geometry] Prim not in root layer, using composed "
                "stage");
            auto prim = global_payload.stage->GetPrimAtPath(input_path);
            if (!prim || !read_geometry_from_usd(
                             geometry, prim, global_payload.current_time)) {
                spdlog::error(
                    "[input_geometry] Failed to read geometry: {}",
                    input_path.GetString());
                return false;
            }
        }
    }
    else {
        auto prim = global_payload.stage->GetPrimAtPath(input_path);
        if (!prim) {
            spdlog::error(
                "[input_geometry] Prim not found: {}", input_path.GetString());
            return false;
        }

        if (global_payload.is_modifier_mode) {
            spdlog::debug(
                "[input_geometry] Reading from COMPOSED stage for modifier {}, "
                "prim={}",
                global_payload.current_modifier_index,
                input_path.GetString());
        }

        if (!read_geometry_from_usd(
                geometry, prim, global_payload.current_time)) {
            spdlog::error(
                "[input_geometry] Failed to read geometry: {}",
                input_path.GetString());
            return false;
        }
    }

    params.set_output("Geometry", std::move(geometry));

    return true;
}

NODE_DECLARATION_UI(input_geometry);

NODE_DEF_CLOSE_SCOPE

#endif
