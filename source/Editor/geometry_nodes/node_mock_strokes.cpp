// Mock two crossing strokes — a test fixture for multi-color paint mixing.
//
// Emits TWO curve segments:
//   stroke 0 (red,  RYB 1,0,0): horizontal along X, 0..0.5s
//   stroke 1 (blue, RYB 0,0,1): vertical along Y, crossing stroke 0 mid-canvas,
//                               delayed to 0.5..1.0s so it draws AFTER stroke 0
//                               completes.
//
// Each point carries an absolute `timestamp` (seconds since sim start) so the
// mock_point_emitter replays stroke 0 fully, THEN stroke 1 — exercising the
// wet-in-wet color mixing where blue crosses the still-wet red.

#include "GCore/Components/CurveComponent.h"
#include "GCore/GOP.h"
#include "geom_node_base.h"

#include <cmath>
#include <vector>

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(mock_strokes)
{
    b.add_input<int>("Num Points").default_val(30).min(2).max(200);
    b.add_input<float>("Length").default_val(0.3f).min(0.01f).max(2.0f);
    b.add_input<float>("Stroke 0 Duration").default_val(0.5f).min(0.05f).max(5.0f);
    b.add_input<float>("Stroke 1 Start").default_val(0.5f).min(0.0f).max(5.0f);
    b.add_output<Geometry>("Stroke Curves");
}

NODE_EXECUTION_FUNCTION(mock_strokes)
{
    int num_points = params.get_input<int>("Num Points");
    float length = params.get_input<float>("Length");
    float s0_dur = params.get_input<float>("Stroke 0 Duration");
    float s1_start = params.get_input<float>("Stroke 1 Start");

    auto geometry = Geometry::CreateCurve();
    auto curve = geometry.get_component<CurveComponent>();

    std::vector<glm::vec3> vertices;
    std::vector<glm::vec3> colors;
    std::vector<float> timestamps;
    std::vector<float> widths;

    const glm::vec3 red(1.0f, 0.0f, 0.0f);   // RYB red
    const glm::vec3 blue(0.0f, 0.0f, 1.0f);  // RYB blue

    // Stroke 0: red, horizontal along X, centered at origin, gently wavy.
    // Draws over [0, s0_dur).
    for (int i = 0; i < num_points; i++) {
        float t = static_cast<float>(i) / static_cast<float>(num_points - 1);
        float x = t * length - length * 0.5f;
        float y = 0.02f * std::sin(t * 4.0f * 3.14159265f);
        vertices.emplace_back(x, y, 0.0f);
        colors.push_back(red);
        timestamps.push_back(t * s0_dur);
        widths.push_back(0.02f);
    }

    // Stroke 1: blue, vertical along Y, crossing stroke 0 at the origin.
    // Draws over [s1_start, s1_start + s0_dur) — delayed so stroke 0 finishes
    // first, then blue is laid across the still-wet red.
    for (int i = 0; i < num_points; i++) {
        float t = static_cast<float>(i) / static_cast<float>(num_points - 1);
        float y = t * length - length * 0.5f;
        float x = 0.02f * std::sin(t * 4.0f * 3.14159265f);
        vertices.emplace_back(x, y, 0.0f);
        colors.push_back(blue);
        timestamps.push_back(s1_start + t * s0_dur);
        widths.push_back(0.02f);
    }

    curve->set_vertices(vertices);
    // Two segments, num_points each.
    curve->set_vert_count({num_points, num_points});
    curve->set_display_color(colors);
    curve->set_width(widths);
    curve->add_vertex_scalar_quantity("timestamp", timestamps);

    params.set_output("Stroke Curves", std::move(geometry));
    return true;
}

NODE_DECLARATION_UI(mock_strokes);

NODE_DEF_CLOSE_SCOPE
