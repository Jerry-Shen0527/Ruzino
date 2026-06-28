// node_brush_wb_fluid — Wetbrush FLUID + PARTICLE sub-step (streaming zone
// chain).
//
// Receives a WetbrushFrame from brush_wb_bristle, runs the FLIP/PIC particle +
// stable-fluids solve, and forwards the updated frame.
//
// Physics (Stage 3 TODO, lifted from brush_paint_sim):
//   particle section ~1614-1980, fluid substep loop ~1994-2470.
//
// All velocity/pressure/divergence/*_tmp and the full particle field set are
// NODE-LOCAL here: created via the resource allocator every cook and
// auto-recycled.

#include <memory>

#include "GCore/GOP.h"
#include "GCore/geom_payload.hpp"
#include "brush_sim_common.hpp"  // BrushPoint, WetbrushSimState, WetbrushFrame, BristleSampleOutputs
#include "geom_node_base.h"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(brush_wb_fluid)
{
    b.add_input<Ruzino::WetbrushFrame>("Frame");
    b.add_input<Ruzino::BristleSampleOutputs>("Bristle Samples");
    b.add_input<float>("Viscosity").default_val(0.5f).min(0.0f).max(10.0f);
    b.add_input<float>("Oil Density").default_val(0.5f).min(0.0f).max(1.0f);
    b.add_input<float>("Diffusion Rate")
        .default_val(0.0001f)
        .min(0.0f)
        .max(0.01f);
    b.add_input<float>("Drying Rate").default_val(0.1f).min(0.0f).max(2.0f);
    b.add_input<float>("Pickup Rate").default_val(0.1f).min(0.0f).max(1.0f);
    b.add_input<float>("Brush Radius").default_val(0.02f).min(0.001f).max(0.5f);

    b.add_output<Ruzino::WetbrushFrame>("Frame");
}

NODE_EXECUTION_FUNCTION(brush_wb_fluid)
{
    Ruzino::WetbrushFrame frame =
        params.get_input<Ruzino::WetbrushFrame>("Frame");
    auto& simstate = frame.state;
    const auto& bp = frame.bp;

    auto payload = params.get_global_payload<GeomPayload>();
    float dt = payload.delta_time > 0.0f ? payload.delta_time : (1.0f / 60.0f);

    if (!simstate) {
        spdlog::warn("brush_wb_fluid: no SimState in frame (graph mis-wired)");
    }

    if (payload.is_simulating && bp.active && simstate) {
        spdlog::info(
            "brush_wb_fluid: advance pos=({:.3f},{:.3f},{:.3f}) dt={:.4f} "
            "(fluid + particle physics TODO)",
            bp.pos.x,
            bp.pos.y,
            bp.pos.z,
            dt);
    }

    params.set_output("Frame", frame);
    return true;
}

NODE_DECLARATION_UI(brush_wb_fluid);

NODE_DECLARATION_ALWAYS_DIRTY(brush_wb_fluid);

NODE_DEF_CLOSE_SCOPE
