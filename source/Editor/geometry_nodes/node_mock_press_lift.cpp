// Press-and-lift trajectory generator — blob settling test fixture.
//
// The existing mock_stroke keeps z = 0 for the whole curve, i.e. the brush is
// born fully pressed and never approaches or leaves the canvas. This node
// authors a proper Z profile so the blob test can observe what the deposited
// paint does AFTER the brush lifts:
//   phase 1 (press):   hold at Press Z for Press Duration  (dip + deposit)
//   phase 2 (lift):    rise linearly to Lift Z over Lift Duration
//   phase 3 (hover):   trajectory ends — the emitter freezes here and goes
//                      pen-up (active=false), the active window stays at the
//                      press XY (window follows the frozen brush position).
//
// Geometry format is identical to mock_stroke (vertices + display_color +
// timestamp + width, one curve = one stroke) so mock_point_emitter consumes
// it unchanged.

#include <cmath>
#include <vector>

#include "GCore/Components/CurveComponent.h"
#include "GCore/GOP.h"
#include "geom_node_base.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(mock_press_lift)
{
    b.add_input<float>("Center X").default_val(0.0f).min(-1.0f).max(1.0f);
    b.add_input<float>("Center Y").default_val(0.0f).min(-1.0f).max(1.0f);
    b.add_input<float>("Press Z").default_val(0.0f).min(-0.1f).max(0.5f);
    b.add_input<float>("Lift Z").default_val(0.08f).min(0.0f).max(1.0f);
    b.add_input<float>("Press Duration")
        .default_val(0.3f)
        .min(0.05f)
        .max(5.0f);  // seconds
    b.add_input<float>("Lift Duration")
        .default_val(0.15f)
        .min(0.05f)
        .max(5.0f);  // seconds
    b.add_input<float>("Ink R (RYB)").default_val(1.0f).min(0.0f).max(1.0f);
    b.add_input<float>("Ink Y (RYB)").default_val(0.0f).min(0.0f).max(1.0f);
    b.add_input<float>("Ink B (RYB)").default_val(0.0f).min(0.0f).max(1.0f);
    b.add_output<Geometry>("Stroke Curves");
}

NODE_EXECUTION_FUNCTION(mock_press_lift)
{
    float cx = params.get_input<float>("Center X");
    float cy = params.get_input<float>("Center Y");
    float press_z = params.get_input<float>("Press Z");
    float lift_z = params.get_input<float>("Lift Z");
    float press_dur = params.get_input<float>("Press Duration");
    float lift_dur = params.get_input<float>("Lift Duration");
    float ink_r = params.get_input<float>("Ink R (RYB)");
    float ink_y = params.get_input<float>("Ink Y (RYB)");
    float ink_b = params.get_input<float>("Ink B (RYB)");

    auto geometry = Geometry::CreateCurve();
    auto curve = geometry.get_component<CurveComponent>();

    // 30 samples/second (same pacing convention as mock_stroke's timestamps).
    const float fps = 30.0f;
    int n_press = std::max(2, static_cast<int>(press_dur * fps));
    int n_lift = std::max(2, static_cast<int>(lift_dur * fps));
    int num_points = n_press + n_lift;

    std::vector<glm::vec3> vertices;
    std::vector<glm::vec3> colors;
    std::vector<float> timestamps;
    std::vector<float> widths;

    for (int i = 0; i < num_points; i++) {
        float t = static_cast<float>(i) / fps;
        float z = press_z;
        if (i >= n_press) {
            float lift_t = static_cast<float>(i - n_press) /
                           static_cast<float>(n_lift - 1);
            z = press_z + (lift_z - press_z) * lift_t;
        }
        vertices.push_back(glm::vec3(cx, cy, z));
        colors.push_back(glm::vec3(ink_r, ink_y, ink_b));
        timestamps.push_back(t);
        widths.push_back(0.02f);
    }

    curve->set_vertices(vertices);
    curve->set_vert_count({ num_points });
    curve->set_display_color(colors);
    curve->set_width(widths);
    curve->add_vertex_scalar_quantity("timestamp", timestamps);

    params.set_output("Stroke Curves", std::move(geometry));
    return true;
}

NODE_DECLARATION_UI(mock_press_lift);

NODE_DEF_CLOSE_SCOPE
