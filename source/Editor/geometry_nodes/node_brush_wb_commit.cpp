// node_brush_wb_commit — Wetbrush COMMIT + OUTPUT sub-step (streaming zone
// chain).
//
// Receives the final WetbrushFrame from brush_wb_fluid and:
//   - commits the live 3D window into the persistent 2D canvas layer
//     (canvas_commit shader, brush_paint_sim ~2712)
//   - reads back fidelity statistics into the debug output ports (~2568)
//   - emits the Paint Particles geometry (one point per painted canvas cell,
//     ~2762) so downstream consumers (write_usd, render) see the paint.
//
// The frame's SimState (carrying the canvas layer) is forwarded on the "Frame"
// output so the zone feeds it back simulation_out -> simulation_in for the
// next frame.

#include <memory>

#include "GCore/Components/PointsComponent.h"
#include "GCore/GOP.h"
#include "GCore/geom_payload.hpp"
#include "brush_sim_common.hpp"  // BrushPoint, WetbrushSimState, WetbrushFrame
#include "geom_node_base.h"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(brush_wb_commit)
{
    b.add_input<Ruzino::WetbrushFrame>("Frame");

    // Outputs mirror brush_paint_sim / brush_wetbrush_step so the existing
    // fidelity-test harness (read 8 debug ports) works unchanged.
    b.add_output<Geometry>("Paint Particles");
    b.add_output<float>("Max Divergence");
    b.add_output<float>("Mean Divergence");
    b.add_output<float>("Total Density");
    b.add_output<float>("Total Color R");
    b.add_output<float>("Total Color Y");
    b.add_output<float>("Total Color B");
    b.add_output<int>("Particle Count");
    b.add_output<float>("Total Particle Mass");
    // Frame forwarded for zone feedback (carries the persistent canvas).
    b.add_output<Ruzino::WetbrushFrame>("Frame");
}

NODE_EXECUTION_FUNCTION(brush_wb_commit)
{
    Ruzino::WetbrushFrame frame =
        params.get_input<Ruzino::WetbrushFrame>("Frame");
    auto& simstate = frame.state;
    const auto& bp = frame.bp;

    auto payload = params.get_global_payload<GeomPayload>();

    auto emit_empty = [&]() {
        Geometry geom;
        auto pts = std::make_shared<PointsComponent>(&geom);
        geom.attach_component(pts);
        params.set_output("Paint Particles", std::move(geom));
        params.set_output("Max Divergence", 0.0f);
        params.set_output("Mean Divergence", 0.0f);
        params.set_output("Total Density", 0.0f);
        params.set_output("Total Color R", 0.0f);
        params.set_output("Total Color Y", 0.0f);
        params.set_output("Total Color B", 0.0f);
        params.set_output("Particle Count", 0);
        params.set_output("Total Particle Mass", 0.0f);
    };

    if (!simstate) {
        spdlog::warn("brush_wb_commit: no SimState in frame (graph mis-wired)");
    }

    if (payload.is_simulating && bp.active && simstate) {
        spdlog::info(
            "brush_wb_commit: advance pos=({:.3f},{:.3f},{:.3f}) "
            "(canvas commit + readback + output TODO)",
            bp.pos.x,
            bp.pos.y,
            bp.pos.z);
    }

    // Stage 3 TODO: canvas_commit + readback + Paint Particles output.
    emit_empty();
    params.set_output("Frame", frame);
    return true;
}

NODE_DECLARATION_UI(brush_wb_commit);

NODE_DECLARATION_ALWAYS_DIRTY(brush_wb_commit);

NODE_DEF_CLOSE_SCOPE
