// Mock stroke generator — creates a known planar curve for testing.
// No input required, outputs a Geometry with CurveComponent.

#include "GCore/Components/CurveComponent.h"
#include "GCore/GOP.h"
#include "geom_node_base.h"

#include <cmath>
#include <vector>

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(mock_stroke)
{
    b.add_input<int>("Num Points").default_val(20).min(2).max(200);
    b.add_input<float>("Amplitude").default_val(0.05f).min(0.0f).max(1.0f);
    b.add_input<float>("Length").default_val(0.3f).min(0.01f).max(2.0f);
    b.add_input<float>("Ink R (RYB)").default_val(1.0f).min(0.0f).max(1.0f);
    b.add_input<float>("Ink Y (RYB)").default_val(0.0f).min(0.0f).max(1.0f);
    b.add_input<float>("Ink B (RYB)").default_val(0.0f).min(0.0f).max(1.0f);
    b.add_output<Geometry>("Stroke Curves");
}

NODE_EXECUTION_FUNCTION(mock_stroke)
{
    int num_points = params.get_input<int>("Num Points");
    float amplitude = params.get_input<float>("Amplitude");
    float length = params.get_input<float>("Length");
    float ink_r = params.get_input<float>("Ink R (RYB)");
    float ink_y = params.get_input<float>("Ink Y (RYB)");
    float ink_b = params.get_input<float>("Ink B (RYB)");

    auto geometry = Geometry::CreateCurve();
    auto curve = geometry.get_component<CurveComponent>();

    std::vector<glm::vec3> vertices;
    std::vector<glm::vec3> colors;
    std::vector<float> timestamps;
    std::vector<float> widths;

    for (int i = 0; i < num_points; i++) {
        float t = static_cast<float>(i) / static_cast<float>(num_points - 1);
        float x = t * length - length * 0.5f;
        float y = amplitude * std::sin(t * 4.0f * 3.14159265f);
        float z = 0.0f;
        vertices.push_back(glm::vec3(x, y, z));
        colors.push_back(glm::vec3(ink_r, ink_y, ink_b));
        timestamps.push_back(static_cast<float>(i) / 30.0f);
        widths.push_back(0.02f);
    }

    curve->set_vertices(vertices);
    curve->set_vert_count({num_points});
    curve->set_display_color(colors);
    curve->set_width(widths);
    curve->add_vertex_scalar_quantity("timestamp", timestamps);

    params.set_output("Stroke Curves", std::move(geometry));
    return true;
}

NODE_DECLARATION_UI(mock_stroke);

NODE_DEF_CLOSE_SCOPE
