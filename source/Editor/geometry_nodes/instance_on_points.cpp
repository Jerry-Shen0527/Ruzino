#include "GCore/Components/InstancerComponent.h"
#include "GCore/Components/PointsComponent.h"
#include "GCore/GOP.h"
#include "glm/ext/matrix_transform.hpp"
#include "glm/gtx/quaternion.hpp"
#include "nodes/core/def/node_def.hpp"

NODE_DEF_OPEN_SCOPE
NODE_DECLARATION_FUNCTION(instance_on_points)
{
    // Function content omitted

    b.add_input<Geometry>("Geometry");
    b.add_input<Geometry>("Points");
    // The prototype axis that gets rotated onto each point normal. False
    // keeps the historical Z-up assumption; true suits Y-up content (trees,
    // terrain, most of this codebase) so instances stand upright instead
    // of tipping flat.
    b.add_input<bool>("Y-up").default_val(false);
    // Points carry a per-point scale in their width (e.g. from
    // terrain_scatter_points); when enabled it becomes the per-instance
    // scale times the multiplier. Off by default: no scaling, as before.
    b.add_input<bool>("Use Width as Scale").default_val(false);
    b.add_input<float>("Scale Multiplier")
        .min(0.01)
        .max(100.0)
        .default_val(1.0);
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(instance_on_points)
{
    // Function content omitted
    auto points = params.get_input<Geometry>("Points");
    points.apply_transform();
    auto geometry = params.get_input<Geometry>("Geometry");
    geometry.apply_transform();

    const bool y_up = params.get_input<bool>("Y-up");
    const bool use_width_scale = params.get_input<bool>("Use Width as Scale");
    const float scale_multiplier = params.get_input<float>("Scale Multiplier");

    auto instancer = std::make_shared<InstancerComponent>(&geometry);
    geometry.attach_component(instancer);

    auto points_component = points.get_const_component<PointsComponent>();

    if (!points_component) {
        params.set_error("No points component found in input Points");
        return false;
    }

    auto points_vertices = points_component->get_vertices();
    auto points_normals = points_component->get_normals();
    auto points_width = points_component->get_width();

    // Check if we have normals to orient instances
    bool has_normals = !points_normals.empty() &&
                       points_normals.size() == points_vertices.size();
    bool has_scales =
        use_width_scale && points_width.size() == points_vertices.size();

    // Scales only travel to USD when rotations are enabled, so enable for
    // scaled instances even without normals (orientation stays identity).
    instancer->set_has_rotations_enabled(has_normals || has_scales);

    // Default up direction follows the Y-up flag (Z-axis otherwise)
    glm::vec3 default_up(0.0f, 0.0f, 1.0f);
    if (y_up) {
        default_up = glm::vec3(0.0f, 1.0f, 0.0f);
    }

    // Batch prepare all instances
    size_t num_points = points_vertices.size();
    std::vector<glm::vec3> positions;
    std::vector<glm::quat> orientations;
    std::vector<glm::vec3> scales;

    positions.reserve(num_points);
    if (has_normals) {
        orientations.reserve(num_points);
    }
    if (has_scales) {
        scales.reserve(num_points);
    }

    for (size_t i = 0; i < num_points; ++i) {
        const auto& point = points_vertices[i];
        positions.push_back(point);

        // Only compute orientations if we have normals
        if (has_normals) {
            const auto& normal = points_normals[i];
            glm::vec3 normalized_normal = glm::normalize(normal);

            // Calculate rotation from default up to normal
            float dot = glm::dot(default_up, normalized_normal);

            glm::quat orientation;
            if (std::abs(dot - 1.0f) < 1e-6f) {
                // Normal is already aligned with default up, no rotation needed
                orientation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
            }
            else if (std::abs(dot + 1.0f) < 1e-6f) {
                // Normal is opposite to default up, rotate 180 degrees around X
                orientation = glm::angleAxis(
                    glm::pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
            }
            else {
                // General case: create rotation from default up to normal
                glm::vec3 axis =
                    glm::normalize(glm::cross(default_up, normalized_normal));
                float angle = std::acos(glm::clamp(dot, -1.0f, 1.0f));
                orientation = glm::angleAxis(angle, axis);
            }
            orientations.push_back(orientation);
        }

        if (has_scales) {
            const float s = points_width[i] * scale_multiplier;
            scales.emplace_back(s);
        }
    }

    // Batch add all instances at once
    // orientations and scales can be empty
    instancer->add_instances(positions, orientations, scales);

    params.set_output("Geometry", std::move(geometry));

    return true;
}

NODE_DECLARATION_UI(instance_on_points);
NODE_DEF_CLOSE_SCOPE