// node_brush_wb_bristle — Wetbrush BRISTLE sub-step (streaming zone chain).
//
// Receives a WetbrushFrame (BrushPoint + shared SimState) from
// brush_wb_deposit, runs the bristle dynamics + paint transfer stage, and
// forwards the updated frame. The zone carries WetbrushFrame as its single
// typed boundary value.
//
// Physics (Stage 3 TODO, lifted from brush_paint_sim):
//   bristle_simulate -> density_constraint (PBD) -> resample -> rasterize ->
//   merge (reads grid density/color/wetness/oil_density) ->
//   bristle<->particle liquid transfer/emit (reads grid color).

#include <memory>

#include "GCore/GOP.h"
#include "GCore/geom_payload.hpp"
#include "brush_sim_common.hpp"  // BrushPoint, WetbrushSimState, WetbrushFrame, BristleSampleOutputs
#include "geom_node_base.h"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(brush_wb_bristle)
{
    b.add_input<Ruzino::WetbrushFrame>("Frame");
    // 2-node field: height written by deposit, read here for bristle vars.
    b.add_input<nvrhi::BufferHandle>("Height Field");
    b.add_input<float>("Brush Radius").default_val(0.02f).min(0.001f).max(0.5f);
    b.add_input<float>("Brush Pressure").default_val(1.0f).min(0.0f).max(4.0f);
    b.add_input<float>("Viscosity").default_val(0.5f).min(0.0f).max(10.0f);
    b.add_input<float>("Oil Density").default_val(0.5f).min(0.0f).max(1.0f);
    // Optional: vec3 sockets can't carry a default_val; exec falls back to red.
    b.add_input<glm::vec3>("Ink Color").optional(true);

    b.add_output<Ruzino::WetbrushFrame>("Frame");
    // 2-node field: written here, read by brush_wb_fluid (particle
    // emit/update).
    b.add_output<Ruzino::BristleSampleOutputs>("Bristle Samples");
}

NODE_EXECUTION_FUNCTION(brush_wb_bristle)
{
    Ruzino::WetbrushFrame frame =
        params.get_input<Ruzino::WetbrushFrame>("Frame");
    auto& simstate = frame.state;
    const auto& bp = frame.bp;

    auto payload = params.get_global_payload<GeomPayload>();
    float dt = payload.delta_time > 0.0f ? payload.delta_time : (1.0f / 60.0f);

    if (!simstate) {
        spdlog::warn(
            "brush_wb_bristle: no SimState in frame (graph mis-wired)");
    }

    if (payload.is_simulating && bp.active && simstate) {
        spdlog::info(
            "brush_wb_bristle: advance pos=({:.3f},{:.3f},{:.3f}) dt={:.4f} "
            "(bristle physics TODO)",
            bp.pos.x,
            bp.pos.y,
            bp.pos.z,
            dt);
    }

    params.set_output("Frame", frame);
    params.set_output("Bristle Samples", Ruzino::BristleSampleOutputs{});
    return true;
}

NODE_DECLARATION_UI(brush_wb_bristle);

NODE_DECLARATION_ALWAYS_DIRTY(brush_wb_bristle);

NODE_DEF_CLOSE_SCOPE
