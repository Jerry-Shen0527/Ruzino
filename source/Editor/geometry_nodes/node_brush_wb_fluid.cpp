// node_brush_wb_fluid — Wetbrush FLUID + PARTICLE sub-step (streaming zone
// chain).
//
// Receives the SimState from brush_wb_bristle and runs:
//   - FLIP/PIC particle emit + update + flip/pic + rasterize + compact
//   - stable-fluids solve on the 3D grid: advect -> divergence -> pressure
//     (Jacobi) -> gradient subtract -> damp/dry
//
// Physics (Stage 3 TODO, lifted from brush_paint_sim):
//   particle section lines ~1614-1980, fluid substep loop ~1994-2470.
//
// All velocity/pressure/divergence/*_tmp and the full particle field set are
// NODE-LOCAL here: created via the resource allocator every cook and
// auto-recycled (only this one node touches them). The bristle sample outputs
// (read by particle emit/update) come in on a socket. The live fields
// (density/color/wetness/oil_density/velocity) live in SimState — velocity is
// the one case where it's node-local in the resource sense BUT its result must
// persist for next frame's FLIP snapshot, so velocity is hoisted into SimState
// as an exception (it's effectively >=3-node if you count the FLIP history).
// For the skeleton we leave it out; Stage 3 will reconcile.

#include <memory>

#include "GCore/GOP.h"
#include "GCore/geom_payload.hpp"
#include "brush_sim_common.hpp"  // BrushPoint, WetbrushSimState, BristleSampleOutputs
#include "geom_node_base.h"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(brush_wb_fluid)
{
    b.add_input<Ruzino::BrushPoint>("Brush Point");
    b.add_input<std::shared_ptr<Ruzino::WetbrushSimState>>("SimState");
    b.add_input<Ruzino::BristleSampleOutputs>("Bristle Samples");
    // Fluid params.
    b.add_input<float>("Viscosity").default_val(0.5f).min(0.0f).max(10.0f);
    b.add_input<float>("Oil Density").default_val(0.5f).min(0.0f).max(1.0f);
    b.add_input<float>("Diffusion Rate")
        .default_val(0.0001f)
        .min(0.0f)
        .max(0.01f);
    b.add_input<float>("Drying Rate").default_val(0.1f).min(0.0f).max(2.0f);
    b.add_input<float>("Pickup Rate").default_val(0.1f).min(0.0f).max(1.0f);
    b.add_input<float>("Brush Radius").default_val(0.02f).min(0.001f).max(0.5f);

    b.add_output<std::shared_ptr<Ruzino::WetbrushSimState>>("SimState");
}

NODE_EXECUTION_FUNCTION(brush_wb_fluid)
{
    auto simstate =
        params.get_input<std::shared_ptr<Ruzino::WetbrushSimState>>("SimState");
    Ruzino::BrushPoint bp = params.get_input<Ruzino::BrushPoint>("Brush Point");

    auto payload = params.get_global_payload<GeomPayload>();
    float dt = payload.delta_time > 0.0f ? payload.delta_time : (1.0f / 60.0f);

    if (!simstate) {
        spdlog::warn(
            "brush_wb_fluid: no SimState from bristle (graph mis-wired)");
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

    params.set_output("SimState", simstate);
    return true;
}

NODE_DECLARATION_UI(brush_wb_fluid);

NODE_DECLARATION_ALWAYS_DIRTY(brush_wb_fluid);

NODE_DEF_CLOSE_SCOPE
