
#include "GCore/Components/MeshComponent.h"
#include "GCore/Components/PointsComponent.h"
#include "GCore/GOP.h"
#include "nodes/core/def/node_def.hpp"

NODE_DEF_OPEN_SCOPE
NODE_DECLARATION_FUNCTION(node_merge_geometry)
{
    // Function content omitted

    b.add_input_group<Geometry>("Geometries").set_runtime_dynamic(true);

    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(node_merge_geometry)
{
    // Function content omitted
    auto geometries = params.get_input_group<Geometry>("Geometries");

    Geometry merged_geometry;
    auto mesh = std::make_shared<MeshComponent>(&merged_geometry);
    auto points = std::make_shared<PointsComponent>(&merged_geometry);

    bool has_mesh = false;
    bool has_points = false;

    for (auto& geometry : geometries) {
        geometry.apply_transform();

        auto mesh_component = geometry.get_component<MeshComponent>();
        if (mesh_component) {
            mesh->append_mesh(mesh_component);
            has_mesh = true;
        }

        auto points_component = geometry.get_component<PointsComponent>();
        if (points_component) {
            // Merge points
            auto this_vertices = points->get_vertices();
            auto that_vertices = points_component->get_vertices();
            this_vertices.insert(
                this_vertices.end(),
                that_vertices.begin(),
                that_vertices.end());
            points->set_vertices(this_vertices);

            // Merge normals if they exist
            auto this_normals = points->get_normals();
            auto that_normals = points_component->get_normals();
            if (!that_normals.empty()) {
                this_normals.insert(
                    this_normals.end(),
                    that_normals.begin(),
                    that_normals.end());
                points->set_normals(this_normals);
            }

            // Merge display colors if they exist
            auto this_colors = points->get_display_color();
            auto that_colors = points_component->get_display_color();
            if (!that_colors.empty()) {
                this_colors.insert(
                    this_colors.end(), that_colors.begin(), that_colors.end());
                points->set_display_color(this_colors);
            }

            // Merge widths if they exist
            auto this_widths = points->get_width();
            auto that_widths = points_component->get_width();
            if (!that_widths.empty()) {
                this_widths.insert(
                    this_widths.end(), that_widths.begin(), that_widths.end());
                points->set_width(this_widths);
            }

            has_points = true;
        }
    }

    if (has_mesh) {
        merged_geometry.attach_component(mesh);
    }
    if (has_points) {
        merged_geometry.attach_component(points);
    }

    params.set_output("Geometry", std::move(merged_geometry));

    return true;
}

NODE_DECLARATION_UI(node_merge_geometry);
NODE_DEF_CLOSE_SCOPE
