#include "GCore/Components/CurveComponent.h"
#include "GCore/Components/PointsComponent.h"
#include "GCore/GOP.h"
#include "geom_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

struct BrushSimStorage {
    constexpr static bool has_storage = false;
    // TODO: Persistent GPU buffers for fluid simulation
    int frame_count = 0;
};

NODE_DECLARATION_FUNCTION(brush_simulate)
{
    b.add_input<Geometry>("Brush Stroke");
    b.add_input<Geometry>("Canvas State");
    b.add_input<float>("Brush Width").default_val(0.02f).min(0.001f).max(0.5f);
    b.add_input<float>("Ink Amount").default_val(0.8f).min(0.0f).max(1.0f);
    b.add_input<float>("Pickup Rate").default_val(0.1f).min(0.0f).max(1.0f);
    b.add_input<float>("Viscosity").default_val(0.5f).min(0.0f).max(1.0f);
    b.add_input<float>("Surface Tension").default_val(0.3f).min(0.0f).max(1.0f);

    b.add_output<Geometry>("Updated Canvas");
    b.add_output<Geometry>("Brush Particles");
}

NODE_EXECUTION_FUNCTION(brush_simulate)
{
    auto& storage = params.get_storage<BrushSimStorage&>();
    auto brush_stroke = params.get_input<Geometry>("Brush Stroke");
    auto canvas_state = params.get_input<Geometry>("Canvas State");
    float brush_width = params.get_input<float>("Brush Width");
    float ink_amount = params.get_input<float>("Ink Amount");
    float pickup_rate = params.get_input<float>("Pickup Rate");
    float viscosity = params.get_input<float>("Viscosity");
    float surface_tension = params.get_input<float>("Surface Tension");

    // Collect existing canvas particles
    std::vector<glm::vec3> canvas_vertices;
    auto existing_points = canvas_state.get_component<PointsComponent>();
    if (existing_points) {
        canvas_vertices = existing_points->get_vertices();
    }

    // Get brush stroke points and deposit them
    auto curve = brush_stroke.get_component<CurveComponent>();
    if (curve) {
        auto stroke_verts = curve->get_vertices();
        // Placeholder: deposit stroke points directly onto canvas
        // TODO: Replace with fluid simulation (SPH or particle-based)
        for (const auto& v : stroke_verts) {
            canvas_vertices.push_back(v);
        }
    }

    // Create updated canvas geometry
    auto updated_canvas = Geometry::CreatePoints();
    auto canvas_pts = std::make_shared<PointsComponent>(&updated_canvas);
    updated_canvas.attach_component(canvas_pts);
    canvas_pts->set_vertices(canvas_vertices);
    canvas_pts->set_width(
        std::vector<float>(canvas_vertices.size(), brush_width));
    canvas_pts->set_display_color(
        std::vector<glm::vec3>(
            canvas_vertices.size(),
            glm::vec3(ink_amount)));

    // Brush particles (placeholder - empty)
    auto brush_particles = Geometry::CreatePoints();
    auto brush_pts = std::make_shared<PointsComponent>(&brush_particles);
    brush_particles.attach_component(brush_pts);

    storage.frame_count++;
    spdlog::info(
        "brush_simulate: frame={}, canvas_particles={}",
        storage.frame_count,
        canvas_vertices.size());

    params.set_output("Updated Canvas", std::move(updated_canvas));
    params.set_output("Brush Particles", std::move(brush_particles));
    return true;
}

NODE_DECLARATION_UI(brush_simulate);

NODE_DEF_CLOSE_SCOPE
