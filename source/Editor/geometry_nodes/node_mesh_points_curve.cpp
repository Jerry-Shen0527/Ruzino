#include "GCore/Components/CurveComponent.h"
#include "GCore/Components/MeshComponent.h"
#include "GCore/Components/PointsComponent.h"
#include "geom_node_base.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(mesh_to_points)
{
    b.add_input<Geometry>("Mesh");
    b.add_output<Geometry>("Points");
}

NODE_EXECUTION_FUNCTION(mesh_to_points)
{
    auto mesh_geometry = params.get_input<Geometry>("Mesh");
    mesh_geometry.apply_transform();
    auto mesh_component = mesh_geometry.get_const_component<MeshComponent>();

    if (!mesh_component) {
        return false;
    }

    const auto& vertices = mesh_component->get_vertices();
    if (vertices.empty()) {
        return false;
    }

    Geometry points_geometry;
    auto points_component = std::make_shared<PointsComponent>(&points_geometry);
    points_geometry.attach_component(points_component);

    points_component->set_vertices(vertices);
    points_component->set_width(std::vector<float>(vertices.size(), 0.1f));

    const auto& normals = mesh_component->get_normals();
    if (!normals.empty()) {
        points_component->set_normals(normals);
    }

    params.set_output("Points", std::move(points_geometry));
    return true;
}

NODE_DECLARATION_UI(mesh_to_points);

NODE_DECLARATION_FUNCTION(points_to_curve)
{
    b.add_input<Geometry>("Points");
    b.add_input<bool>("Cyclic").default_val(false);
    b.add_output<Geometry>("Curve");
}

NODE_EXECUTION_FUNCTION(points_to_curve)
{
    auto points_geometry = params.get_input<Geometry>("Points");
    auto points_component =
        points_geometry.get_const_component<PointsComponent>();

    if (!points_component) {
        return false;
    }

    const auto& vertices = points_component->get_vertices();
    if (vertices.empty()) {
        return false;
    }

    bool cyclic = params.get_input<bool>("Cyclic");

    Geometry curve_geometry;
    auto curve_component = std::make_shared<CurveComponent>(&curve_geometry);
    curve_geometry.attach_component(curve_component);

    curve_component->set_vertices(vertices);
    curve_component->set_vert_count({ static_cast<int>(vertices.size()) });
    curve_component->set_periodic(cyclic);
    curve_component->set_type(CurveComponent::CurveType::Linear);

    if (!points_component->get_width().empty()) {
        curve_component->set_width(points_component->get_width());
    }
    else {
        curve_component->set_width(std::vector<float>(vertices.size(), 1.0f));
    }

    const auto& normals = points_component->get_normals();
    if (!normals.empty()) {
        curve_component->set_curve_normals(normals);
    }

    params.set_output("Curve", std::move(curve_geometry));
    return true;
}

NODE_DECLARATION_UI(points_to_curve);

NODE_DEF_CLOSE_SCOPE
