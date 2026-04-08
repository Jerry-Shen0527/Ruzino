#include <algorithm>
#include <glm/glm.hpp>
#include <limits>

#include "GCore/Components/CurveComponent.h"
#include "GCore/Components/InstancerComponent.h"
#include "GCore/Components/MeshComponent.h"
#include "GCore/Components/PointsComponent.h"
#include "GCore/GOP.h"
#include "nodes/core/def/node_def.hpp"
#include "spdlog/spdlog.h"

using namespace Ruzino;

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(get_bounds)
{
    b.add_input<Geometry>("Geometry");
    b.add_output<float>("Min X");
    b.add_output<float>("Min Y");
    b.add_output<float>("Min Z");
    b.add_output<float>("Max X");
    b.add_output<float>("Max Y");
    b.add_output<float>("Max Z");
}

NODE_EXECUTION_FUNCTION(get_bounds)
{
    Geometry input_geometry = params.get_input<Geometry>("Geometry");

    // Apply transform to get world space coordinates
    input_geometry.apply_transform();

    std::vector<glm::vec3> vertices;

    // Try to get vertices from different component types
    auto mesh_component = input_geometry.get_component<MeshComponent>();
    if (mesh_component) {
        vertices = mesh_component->get_vertices();
    }
    else {
        auto points_component = input_geometry.get_component<PointsComponent>();
        if (points_component) {
            vertices = points_component->get_vertices();
        }
        else {
            auto curve_component =
                input_geometry.get_component<CurveComponent>();
            if (curve_component) {
                vertices = curve_component->get_vertices();
            }
            else {
                auto instancer_component =
                    input_geometry.get_component<InstancerComponent>();
                if (instancer_component) {
                    vertices = instancer_component->get_positions();
                }
            }
        }
    }

    if (vertices.empty()) {
        spdlog::error("No vertices found in geometry");
        return false;
    }

    // Initialize bounds
    glm::vec3 min_bounds(std::numeric_limits<float>::max());
    glm::vec3 max_bounds(std::numeric_limits<float>::lowest());

    // Calculate bounding box
    for (const auto& vertex : vertices) {
        min_bounds.x = std::min(min_bounds.x, vertex.x);
        min_bounds.y = std::min(min_bounds.y, vertex.y);
        min_bounds.z = std::min(min_bounds.z, vertex.z);

        max_bounds.x = std::max(max_bounds.x, vertex.x);
        max_bounds.y = std::max(max_bounds.y, vertex.y);
        max_bounds.z = std::max(max_bounds.z, vertex.z);
    }

    // Output bounds
    params.set_output("Min X", min_bounds.x);
    params.set_output("Min Y", min_bounds.y);
    params.set_output("Min Z", min_bounds.z);
    params.set_output("Max X", max_bounds.x);
    params.set_output("Max Y", max_bounds.y);
    params.set_output("Max Z", max_bounds.z);

    return true;
}

NODE_DECLARATION_UI(get_bounds);

NODE_DEF_CLOSE_SCOPE
