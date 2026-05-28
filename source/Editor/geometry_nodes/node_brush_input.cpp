#include "GCore/Components/CurveComponent.h"
#include "GCore/GOP.h"
#include "geom_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "spdlog/spdlog.h"

#include <chrono>

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(brush_input)
{
    b.add_input<Geometry>("Stroke Curves");
    b.add_input<float>("Brush Width").default_val(0.02f).min(0.001f).max(0.5f);
    b.add_input<float>("Brush Pressure").default_val(1.0f).min(0.0f).max(2.0f);
    b.add_input<float>("Ink Amount").default_val(0.8f).min(0.0f).max(1.0f);
    b.add_input<float>("Ink R (RYB)").default_val(1.0f).min(0.0f).max(1.0f);
    b.add_input<float>("Ink Y (RYB)").default_val(0.0f).min(0.0f).max(1.0f);
    b.add_input<float>("Ink B (RYB)").default_val(0.0f).min(0.0f).max(1.0f);

    b.add_output<Geometry>("Brush Stroke");
}

NODE_EXECUTION_FUNCTION(brush_input)
{
    auto stroke_curves = params.get_input<Geometry>("Stroke Curves");
    float brush_width = params.get_input<float>("Brush Width");
    float brush_pressure = params.get_input<float>("Brush Pressure");
    float ink_amount = params.get_input<float>("Ink Amount");
    float ink_r = params.get_input<float>("Ink R (RYB)");
    float ink_y = params.get_input<float>("Ink Y (RYB)");
    float ink_b = params.get_input<float>("Ink B (RYB)");

    auto curve = stroke_curves.get_component<CurveComponent>();
    if (!curve) {
        spdlog::warn("brush_input: no CurveComponent found, passing through");
        params.set_output("Brush Stroke", std::move(stroke_curves));
        return true;
    }

    auto vertices = curve->get_vertices();

    // Set width from brush size × pressure
    curve->set_width(
        std::vector<float>(
            vertices.size(), brush_width * brush_pressure));

    // Set displayColor from RYB ink color × ink_amount
    glm::vec3 ink_color(ink_r * ink_amount, ink_y * ink_amount, ink_b * ink_amount);
    curve->set_display_color(
        std::vector<glm::vec3>(vertices.size(), ink_color));

    // Timestamp stays in vertex_scalar_quantity("timestamp") — untouched

    spdlog::info(
        "brush_input: {} points, pressure={}, ink=({},{},{})*{}",
        vertices.size(), brush_pressure, ink_r, ink_y, ink_b, ink_amount);

    params.set_output("Brush Stroke", std::move(stroke_curves));
    return true;
}

NODE_DECLARATION_UI(brush_input);

NODE_DEF_CLOSE_SCOPE
