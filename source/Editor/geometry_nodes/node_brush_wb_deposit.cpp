// node_brush_wb_deposit — Wetbrush DEPOSIT sub-step (streaming zone chain).
//
// Pipeline (inside the simulation zone):
//   mock_stroke -> brush_capture -> mock_point_emitter --BrushPoint-->
//     [simulation_in] --SimState--> brush_wb_deposit --SimState+height-->
//       brush_wb_bristle --SimState+samples--> brush_wb_fluid --SimState-->
//         brush_wb_commit --Paint Particles--> [simulation_out]
//
// This node owns the FIRST physics stage: position the active window on the
// brush and deposit paint (density + RYB color) from the per-frame BrushPoint.
// On the init frame (is_simulating == false) it allocates the shared
// WetbrushSimState (live fields + canvas layer + control bookkeeping) so the
// downstream nodes receive a valid SimState.
//
// Cross-frame state lives on the socket value (shared_ptr<WetbrushSimState>)
// that flows simulation_in -> deposit -> bristle -> fluid -> commit ->
// simulation_out; the zone executor feeds simulation_out's value back to
// simulation_in next frame. Node-local transient fields use the resource
// allocator (auto-recycled); 2-node fields (height_field, bristle samples) are
// regular socket outputs.
//
// STATUS: skeleton. SimState allocation + window/deposit physics lifted from
// brush_paint_sim is a TODO (Stage 3). For now it allocates an empty SimState
// on the init frame and passes it through, so the streaming path is exercised
// end-to-end.

#include <memory>

#include "GCore/GOP.h"
#include "GCore/geom_payload.hpp"
#include "brush_sim_common.hpp"  // BrushPoint, WetbrushSimState
#include "geom_node_base.h"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(brush_wb_deposit)
{
    // Per-frame brush sample (from mock_point_emitter via simulation_in).
    b.add_input<Ruzino::BrushPoint>("Brush Point");
    // The shared cross-node state (live fields + canvas + control). On the init
    // frame this is empty (simulation_in forwards upstream's slot, which is
    // nothing) and we allocate it.
    b.add_input<std::shared_ptr<Ruzino::WetbrushSimState>>("SimState");
    // Grid / canvas domain params (mirror brush_paint_sim /
    // brush_wetbrush_step).
    b.add_input<int>("Resolution").default_val(512).min(64).max(4096);
    b.add_input<int>("Resolution Z").default_val(32).min(4).max(128);
    b.add_input<float>("Paper Size").default_val(1.0f).min(0.1f).max(10.0f);
    b.add_input<float>("Canvas Center X").default_val(0.0f);
    b.add_input<float>("Canvas Center Y").default_val(0.0f);
    b.add_input<float>("Canvas Z").default_val(0.0f);
    b.add_input<float>("Canvas Height").default_val(0.0f).min(0.0f).max(2.0f);
    // Brush / material params used by deposit.
    b.add_input<float>("Brush Radius").default_val(0.02f).min(0.001f).max(0.5f);
    b.add_input<float>("Brush Pressure").default_val(1.0f).min(0.0f).max(4.0f);
    b.add_input<float>("Ink Amount").default_val(0.8f).min(0.0f).max(2.0f);
    b.add_input<glm::vec3>(
        "Ink Color");  // RYB; exec applies red fallback if unset

    // The (possibly newly-allocated) SimState passed downstream.
    b.add_output<std::shared_ptr<Ruzino::WetbrushSimState>>("SimState");
    // height_field is written here, read by brush_wb_bristle (2-node field).
    b.add_output<nvrhi::BufferHandle>("Height Field");
}

NODE_EXECUTION_FUNCTION(brush_wb_deposit)
{
    using Ruzino::WetbrushSimState;

    Ruzino::BrushPoint bp = params.get_input<Ruzino::BrushPoint>("Brush Point");
    auto simstate =
        params.get_input<std::shared_ptr<WetbrushSimState>>("SimState");
    int resolution = params.get_input<int>("Resolution");
    int resolution_z = params.get_input<int>("Resolution Z");
    float paper_size = params.get_input<float>("Paper Size");
    glm::vec2 canvas_center_xy(
        params.get_input<float>("Canvas Center X"),
        params.get_input<float>("Canvas Center Y"));
    float canvas_z = params.get_input<float>("Canvas Z");
    float canvas_height_in = params.get_input<float>("Canvas Height");

    auto payload = params.get_global_payload<GeomPayload>();
    float dt = payload.delta_time > 0.0f ? payload.delta_time : (1.0f / 60.0f);

    // --- Init frame: allocate the shared SimState (live fields + canvas) ---
    // The init frame has is_simulating == false. Downstream nodes need a valid
    // (allocated) SimState from frame 1 onward, so we allocate it here on the
    // first cook. (Stage 3 TODO: lift the full buffer allocation + zero-init
    // from brush_paint_sim lines ~533-628, restricted to the WetbrushSimState
    // subset.)
    if (!simstate || !simstate->center_initialized) {
        simstate = std::make_shared<WetbrushSimState>();
        float height = canvas_height_in > 1e-6f
                           ? canvas_height_in
                           : paper_size * static_cast<float>(resolution_z) /
                                 static_cast<float>(resolution);
        simstate->grid_center = canvas_center_xy;
        simstate->grid_height = height;
        simstate->grid_center_z = canvas_z + height * 0.5f;
        simstate->grid_paper = paper_size;
        simstate->grid_res = resolution;
        simstate->grid_res_z = resolution_z;
        simstate->center_initialized = true;
        simstate->has_prev_brush_pos = false;
        simstate->prev_brush_vel = glm::vec3(0.0f);
        simstate->prev_angular_vel = glm::vec3(0.0f);

        spdlog::info(
            "brush_wb_deposit: allocated SimState {}x{}x{}, paper={:.3f}, "
            "height={:.3f} (buffer alloc TODO)",
            resolution,
            resolution_z,
            resolution,
            paper_size,
            height);
    }

    // On the init frame (or pen-up) we just pass the SimState through without
    // depositing. The advance-frame deposit physics is a Stage 3 TODO.
    if (!payload.is_simulating || !bp.active) {
        params.set_output("SimState", simstate);
        params.set_output("Height Field", nvrhi::BufferHandle{});
        return true;
    }

    spdlog::info(
        "brush_wb_deposit: advance pos=({:.3f},{:.3f},{:.3f}) t={:.3f} "
        "dt={:.4f} "
        "stroke_start={} (window+deposit physics TODO)",
        bp.pos.x,
        bp.pos.y,
        bp.pos.z,
        bp.time,
        dt,
        bp.stroke_start);

    // Track per-frame brush kinematics for the (TODO) finite-difference step.
    if (bp.stroke_start) {
        simstate->has_prev_brush_pos = false;
        simstate->prev_brush_vel = glm::vec3(0.0f);
        simstate->prev_angular_vel = glm::vec3(0.0f);
    }
    simstate->prev_brush_pos = bp.pos;
    simstate->has_prev_brush_pos = true;

    params.set_output("SimState", simstate);
    params.set_output("Height Field", nvrhi::BufferHandle{});
    return true;
}

NODE_DECLARATION_UI(brush_wb_deposit);

NODE_DECLARATION_ALWAYS_DIRTY(brush_wb_deposit);
// ALWAYS_DIRTY is mandatory: the node's real inputs (delta_time, is_simulating)
// arrive via the global payload, not graph sockets. See
// docs/simulation_mechanism.md §2.5.

NODE_DEF_CLOSE_SCOPE
