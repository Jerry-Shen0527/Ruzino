// node_brush_wetbrush_step — streaming single-step Wetbrush paint node.
//
// Pipeline role:
//   [brush_capture] --Stroke Curves--> [mock_point_emitter] --BrushPoint-->
//       [simulation_in] --> [brush_wetbrush_step] --> [simulation_out]
//
// Unlike node_brush_paint_sim (the monolithic node that re-reads the ENTIRE
// "Brush Strokes" curve every frame and deposits all not-yet-deposited
// vertices), this node consumes EXACTLY ONE BrushPoint per frame — supplied
// by mock_point_emitter inside the simulation zone. It then advances the
// shared WetbrushState by a single physics step:
//     one BrushPoint -> deposit -> bristle sim -> rasterize -> fluid ->
//     particle update -> canvas commit.
//
// Cross-frame state lives in this node's own Node::storage (a
// WetbrushState), exactly like brush_paint_sim holds its PaintSimStorage. The
// simulation zone is used only as the frame pulse (delta_time / is_simulating
// via the global payload) and to ferry the per-frame BrushPoint in. See
// docs/simulation_mechanism.md §2/§3 for why state does NOT ride the zone's
// SimulationStorage::data.
//
// STATUS (Step 3c — minimal skeleton): the node registers, declares its
// sockets, lazily allocates the WetbrushState grid + bristle + particle
// buffers, and runs the frame lifecycle (init frame vs. advance frame). The
// actual Wetbrush physics dispatches are intentionally left as TODO hooks —
// they will be lifted from brush_paint_sim's update body incrementally, now
// that the streaming input path (one BrushPoint/frame) is proven end-to-end.

#include "GCore/Components/PointsComponent.h"
#include "GCore/GOP.h"
#include "GCore/geom_payload.hpp"
#include "brush_sim_common.hpp"  // BrushPoint, WetbrushState, SimConstants, helpers
#include "geom_node_base.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <cmath>
#include <vector>

NODE_DEF_OPEN_SCOPE

// ---------------------------------------------------------------------------
// Declaration — inputs mirror brush_paint_sim's parameter set (so the
// physics can be lifted verbatim later), but the brush input is a single
// BrushPoint instead of the whole "Brush Strokes" curve. Outputs are the
// same paint geometry + debug floats as brush_paint_sim so the fidelity
// tests can bind the same ports.
// ---------------------------------------------------------------------------
NODE_DECLARATION_FUNCTION(brush_wetbrush_step)
{
    // The per-frame brush sample, supplied by mock_point_emitter through the
    // simulation zone. Carries pos / pressure-proxy (active) / stroke_start.
    b.add_input<Ruzino::BrushPoint>("Brush Point");

    // --- Grid / canvas domain (matches brush_paint_sim) ---
    b.add_input<int>("Resolution").default_val(512).min(64).max(4096);
    b.add_input<int>("Resolution Z").default_val(32).min(4).max(128);
    b.add_input<float>("Paper Size").default_val(1.0f).min(0.1f).max(10.0f);
    b.add_input<float>("Canvas Center X").default_val(0.0f);
    b.add_input<float>("Canvas Center Y").default_val(0.0f);
    b.add_input<float>("Canvas Z").default_val(0.0f);
    b.add_input<float>("Canvas Height")
        .default_val(0.0f)
        .min(0.0f)
        .max(2.0f);  // 0 = auto (isotropic cells)
    // --- Brush / material ---
    b.add_input<float>("Brush Radius").default_val(0.02f).min(0.001f).max(0.5f);
    b.add_input<float>("Brush Pressure").default_val(1.0f).min(0.0f).max(4.0f);
    b.add_input<float>("Ink Amount").default_val(0.8f).min(0.0f).max(2.0f);
    b.add_input<float>("Viscosity").default_val(0.5f).min(0.0f).max(10.0f);
    b.add_input<float>("Oil Density").default_val(0.5f).min(0.0f).max(1.0f);
    b.add_input<float>("Diffusion Rate")
        .default_val(0.0001f)
        .min(0.0f)
        .max(0.01f);
    b.add_input<float>("Pickup Rate").default_val(0.1f).min(0.0f).max(1.0f);
    b.add_input<float>("Drying Rate").default_val(0.1f).min(0.0f).max(2.0f);
    // --- Ink color (RYB). The streaming path can't read it from a curve, so
    // it is an explicit input. vec3 sockets have no default_val support
    // (see node.hpp ValueTrait), so this port must be wired upstream; exec
    // applies a red fallback if it's missing. ---
    b.add_input<glm::vec3>("Ink Color");

    // --- Outputs (same port set as brush_paint_sim for test compatibility) ---
    b.add_output<Geometry>("Paint Particles");
    b.add_output<float>("Max Divergence");
    b.add_output<float>("Mean Divergence");
    b.add_output<float>("Total Density");
    b.add_output<float>("Total Color R");
    b.add_output<float>("Total Color Y");
    b.add_output<float>("Total Color B");
    b.add_output<int>("Particle Count");
    b.add_output<float>("Total Particle Mass");
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------
NODE_EXECUTION_FUNCTION(brush_wetbrush_step)
{
    using Ruzino::WetbrushState;
    using Ruzino::PaintSimStorage;  // alias in brush_sim_common.hpp

    auto& storage = params.get_storage<WetbrushState&>();
    auto& rc = get_resource_allocator();
    auto device = RHI::get_device();

    // --- Read inputs ---
    Ruzino::BrushPoint bp = params.get_input<Ruzino::BrushPoint>("Brush Point");
    int resolution = params.get_input<int>("Resolution");
    int resolution_z = params.get_input<int>("Resolution Z");
    float paper_size = params.get_input<float>("Paper Size");
    glm::vec2 canvas_center_xy(
        params.get_input<float>("Canvas Center X"),
        params.get_input<float>("Canvas Center Y"));
    float canvas_z_input = params.get_input<float>("Canvas Z");
    float canvas_height_input = params.get_input<float>("Canvas Height");
    float brush_radius = params.get_input<float>("Brush Radius");
    float brush_pressure = params.get_input<float>("Brush Pressure");
    float ink_amount = params.get_input<float>("Ink Amount");
    float viscosity = params.get_input<float>("Viscosity");
    float oil_density_in = params.get_input<float>("Oil Density");
    float diffusion = params.get_input<float>("Diffusion Rate");
    float drying_rate = params.get_input<float>("Drying Rate");
    glm::vec3 ink_color = params.has_input("Ink Color")
                              ? params.get_input<glm::vec3>("Ink Color")
                              : glm::vec3(1.0f, 0.0f, 0.0f);

    auto payload = params.get_global_payload<GeomPayload>();
    // dt comes from the Stage animation tick (see docs/simulation_mechanism.md
    // §1). The init frame has is_simulating == false.
    float dt = payload.delta_time > 0.0f ? payload.delta_time : (1.0f / 60.0f);

    // Helper: emit an empty particles geometry + zeroed debug ports, used on
    // all early-return / not-yet-implemented paths so downstream always sees
    // every output port set.
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

    // ======================================================================
    // LAZY GRID ALLOCATION
    // Anchored to a fixed canvas AABB (Wetbrush §4.2 "space above canvas").
    // Done once, when the grid dimensions are first known or when they
    // change. Mirrors brush_paint_sim's allocation block; the full Wetbrush
    // physics (below) will reuse these exact buffers.
    // ======================================================================
    bool grid_needs_alloc =
        !storage.center_initialized ||
        storage.grid_res != resolution ||
        storage.grid_res_z != resolution_z;

    if (grid_needs_alloc) {
        float height = canvas_height_input > 1e-6f
                           ? canvas_height_input
                           : paper_size *
                                 static_cast<float>(resolution_z) /
                                 static_cast<float>(resolution);
        storage.grid_center = canvas_center_xy;
        storage.grid_height = height;
        storage.grid_center_z = canvas_z_input + height * 0.5f;
        storage.grid_paper = paper_size;
        storage.grid_res = resolution;
        storage.grid_res_z = resolution_z;
        storage.center_initialized = true;

        // Invalidate per-frame brush continuity: a fresh grid has no prior
        // brush position to sweep from.
        storage.has_prev_brush_pos = false;
        storage.prev_brush_vel = glm::vec3(0.0f);
        storage.prev_angular_vel = glm::vec3(0.0f);

        // NOTE (Step 3c skeleton): full buffer allocation (3D window fields,
        // canvas layer, bristle, particle) goes here, lifted from
        // brush_paint_sim lines ~463-628. Left as a TODO for the physics
        // migration step. The skeleton only records the grid bookkeeping so
        // the lifecycle is wired correctly.
        spdlog::info(
            "brush_wetbrush_step: grid allocated {}x{}x{}, paper={:.3f}, "
            "height={:.3f}, canvas_z={:.3f} (buffer alloc TODO)",
            resolution,
            resolution,
            resolution_z,
            storage.grid_paper,
            storage.grid_height,
            canvas_z_input);
    }

    // ======================================================================
    // FRAME LIFECYCLE (docs/simulation_mechanism.md §1/§2)
    //   init frame (is_simulating == false): no BrushPoint is meaningful yet
    //     (emitter stays idle on frame 0) — just finish allocating and idle.
    //   advance frame (is_simulating == true): consume the BrushPoint and
    //     advance the paint by one step.
    // ======================================================================
    if (!payload.is_simulating) {
        // Init frame: allocation done above; emit empty and wait for the
        // simulation to actually start.
        emit_empty();
        params.set_storage(storage);
        return true;
    }

    // Advance frame. Pen-up BrushPoints (active == false) mean "brush is in
    // the air this frame" — skip deposit but still let the fluid relax/dry.
    // (TODO: even on pen-up we should run damp/dry + particle maintenance;
    //  for the skeleton we just idle to keep the path minimal.)
    if (!bp.active) {
        emit_empty();
        params.set_storage(storage);
        return true;
    }

    // ======================================================================
    // ONE WETBRUSH PHYSICS STEP (TODO — lift from brush_paint_sim)
    //
    // The dispatches to port here, in order, are:
    //   1. Derive brush transform (pos/vel/accel/omega) from THIS BrushPoint
    //      + storage.prev_brush_* (finite-differenced per frame, unlike
    //      paint_sim which differences along the curve's vertices).
    //      On bp.stroke_start, reset prev_* and skip velocity inheritance.
    //   2. Position the active window on the brush; commit+clear old window
    //      if it moved (paint_sim's position_window lambda).
    //   3. Bristle sub-step deposit (bristle_simulate -> density_constraint
    //      -> resample -> rasterize -> merge -> liquid transfer/emit).
    //   4. Particle emit + update + flip/pic + rasterize + compact.
    //   5. Fluid: advect -> diffuse (Jacobi) -> divergence -> pressure
    //      (Jacobi) -> gradient subtract -> damp/dry.
    //   6. Canvas commit: flush the live window into the 2D canvas layer.
    //   7. Readback + fidelity statistics into the debug output ports.
    //
    // For the skeleton we log the step and emit empty output so the whole
    // streaming pipeline (capture -> emitter -> zone -> step) can be
    // exercised end-to-end before the physics is wired in.
    // ======================================================================
    spdlog::info(
        "brush_wetbrush_step: advance pos=({:.3f},{:.3f},{:.3f}) t={:.3f} "
        "dt={:.4f} stroke_start={} (physics TODO)",
        bp.pos.x,
        bp.pos.y,
        bp.pos.z,
        bp.time,
        dt,
        bp.stroke_start);

    // Track a minimal per-frame brush history so the (TODO) physics step has
    // valid prev_* state to finite-difference against. stroke_start forces a
    // fresh pen-down: no inherited motion.
    if (bp.stroke_start) {
        storage.has_prev_brush_pos = false;
        storage.prev_brush_vel = glm::vec3(0.0f);
        storage.prev_angular_vel = glm::vec3(0.0f);
    }
    storage.prev_brush_pos = bp.pos;
    storage.has_prev_brush_pos = true;

    emit_empty();
    params.set_storage(storage);
    return true;
}

NODE_DECLARATION_UI(brush_wetbrush_step);

NODE_DECLARATION_ALWAYS_DIRTY(brush_wetbrush_step);
// ALWAYS_DIRTY is mandatory: the node's real inputs (delta_time,
// is_simulating) arrive via the global payload, not a graph socket, so
// dirty propagation alone would never re-cook it. See
// docs/simulation_mechanism.md §2.5.

NODE_DEF_CLOSE_SCOPE
