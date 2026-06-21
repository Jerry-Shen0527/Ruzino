#include "GCore/Components/CurveComponent.h"
#include "GCore/GOP.h"
#include "GCore/geom_payload.hpp"
#include "geom_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "spdlog/spdlog.h"

#include <sstream>

NODE_DEF_OPEN_SCOPE

struct BrushCaptureStorage {
    std::vector<glm::vec3> points;
    std::vector<float> timestamps;
    std::vector<int> stroke_lengths;  // vert_count per stroke
    bool in_stroke = false;

    static constexpr bool has_storage = false;
};

NODE_DECLARATION_FUNCTION(brush_capture)
{
    b.add_output<Geometry>("Stroke Curves");
}

NODE_EXECUTION_FUNCTION(brush_capture)
{
    auto& storage = params.get_storage<BrushCaptureStorage&>();

    auto payload = params.get_global_payload<GeomPayload>();

    if (payload.brush_new_point) {
        if (payload.brush_active) {
            storage.points.push_back(payload.brush_point);
            storage.timestamps.push_back(payload.brush_time);
            if (!storage.in_stroke) {
                storage.in_stroke = true;
                storage.stroke_lengths.push_back(0);
            }
            storage.stroke_lengths.back()++;
        }
        else if (storage.in_stroke) {
            // Pen up — finalize current stroke
            storage.in_stroke = false;
        }
    }

    // Build output curve with all accumulated strokes
    auto geometry = Geometry::CreateCurve();
    auto curve = geometry.get_component<CurveComponent>();

    if (!storage.points.empty()) {
        curve->set_vertices(storage.points);
        curve->set_vert_count(storage.stroke_lengths);
        curve->set_width(
            std::vector<float>(storage.points.size(), 0.01f));
        curve->add_vertex_scalar_quantity(
            "timestamp", storage.timestamps);
        curve->set_display_color(
            std::vector<glm::vec3>(
                storage.points.size(), glm::vec3(0.1f, 0.1f, 0.9f)));
    }

    params.set_output("Stroke Curves", geometry);
    params.set_storage(storage);
    return true;
}

NODE_DECLARATION_UI(brush_capture);
NODE_DECLARATION_ALWAYS_DIRTY(brush_capture);
// brush_capture must always re-cook: its real input is the live mouse
// payload (GeomPayload::brush_*), which is not a graph socket and so does
// not propagate dirty state through the executor. Without ALWAYS_DIRTY the
// node cooks once (empty), caches that empty result, and never picks up
// subsequent mouse points — the symptom of "brush capture not working".

NODE_DEF_CLOSE_SCOPE
