// node_brush_wb_fluid — Wetbrush FLUID + PARTICLE sub-step.
//
// Receives the field from brush_wb_bristle and runs the stable-fluids solve +
// FLIP/PIC particle cycle, lifted 1:1 from brush_paint_sim:
//   * Particle cycle (~1614-1833): emit (sample + grid modes) -> update ->
//     clear accum grids -> rasterize -> merge into main grids.
//   * Fluid solve (~1957-2468): per substep — velocity diffuse (Jacobi),
//     pressure projection (divergence -> Jacobi -> gradient subtract, x2),
//     advect velocity, re-project, advect scalars, diffuse scalars,
//     damp+dry, FLIP/PIC velocity update.
//   * Post-fluid particle maintenance (~2470-2566): particle_to_grid,
//     grid_to_particle, compaction.
//
// All velocity/pressure/divergence/*_tmp and particle accum grids are the
// PERSISTENT field buffers (allocated by deposit) — momentum, pressure and
// particle state must survive frame-to-frame, so they are NOT recreated here.

#include <algorithm>
#include <cmath>
#include <memory>

#include "GCore/GOP.h"
#include "GCore/geom_payload.hpp"
#include "brush_sim_common.hpp"  // WetbrushSimState, WetbrushZoneState, brush_* helpers
#include "geom_node_base.h"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(brush_wb_fluid)
{
    // Per-frame brush sample, forwarded from deposit (so this node knows when
    // the pen is up — pen-up frames still relax the fluid but skip emission).
    b.add_input<Ruzino::BrushPoint>("Brush Point");
    b.add_input<Ruzino::WetbrushZoneState>("State");
    b.add_input<float>("Viscosity").default_val(0.5f).min(0.0f).max(10.0f);
    b.add_input<float>("Oil Density").default_val(0.5f).min(0.0f).max(1.0f);
    b.add_input<float>("Diffusion Rate")
        .default_val(0.0001f)
        .min(0.0f)
        .max(0.01f);
    b.add_input<float>("Drying Rate").default_val(0.1f).min(0.0f).max(2.0f);
    b.add_input<float>("Ink Amount").default_val(0.8f).min(0.0f).max(2.0f);
    b.add_input<float>("Brush Radius").default_val(0.02f).min(0.001f).max(0.5f);
    b.add_input<float>("Brush Pressure").default_val(1.0f).min(0.0f).max(4.0f);

    b.add_output<Ruzino::WetbrushZoneState>("State");
}

NODE_EXECUTION_FUNCTION(brush_wb_fluid)
{
    using Ruzino::WetbrushSimState;
    using Ruzino::WetbrushZoneState;

    Ruzino::BrushPoint bp = params.get_input<Ruzino::BrushPoint>("Brush Point");
    WetbrushZoneState zs = params.get_input<WetbrushZoneState>("State");
    auto& field = zs.state;
    float viscosity = params.get_input<float>("Viscosity");
    float oil_density_in = params.get_input<float>("Oil Density");
    float diffusion = params.get_input<float>("Diffusion Rate");
    float drying_rate = params.get_input<float>("Drying Rate");
    float ink_amount = params.get_input<float>("Ink Amount");
    float brush_radius = params.get_input<float>("Brush Radius");

    auto& rc = get_resource_allocator();
    auto device = RHI::get_device();
    auto payload = params.get_global_payload<GeomPayload>();

    if (!field || !payload.is_simulating) {
        params.set_output("State", zs);
        return true;
    }

    // Pen-up frames still relax the fluid (damp/dry + particle maintenance)
    // exactly like the monolith's pen-up handling — paint keeps settling.

    const int WIN_XY =
        std::min(WetbrushSimState::WIN_ALLOC_XY, field->grid_res);
    const int WIN_Z = field->grid_res_z;
    const int win_n3d = WIN_XY * WIN_XY * WIN_Z;
    const int window_total = win_n3d;
    // Full-grid cell count for global drying (§4.2: "increase the dryness of
    // EVERY grid cell"). Drying must be global so paint that the active window
    // has moved past still dries — otherwise previously painted strokes never
    // harden and can't act as solid cells that deflect later strokes.
    const int global_n3d = field->grid_res * field->grid_res * WIN_Z;
    const float cell_sz =
        field->grid_paper / static_cast<float>(field->grid_res);
    const int Nb = WetbrushSimState::NUM_BRISTLES;
    const int S = WetbrushSimState::SAMPLES_PER_BRISTLE;
    const int max_ptcl = WetbrushSimState::MAX_PARTICLES;

    // Brush pose (grid-local) — needed for the particle CBs.
    glm::vec3 brush_pos_3d = bp.pos;
    brush_pos_3d.x -= field->grid_center.x;
    brush_pos_3d.y -= field->grid_center.y;
    glm::vec3 brush_accel_3d(0.0f);
    glm::vec3 brush_angular_accel(0.0f);

    // Lazily compile the fluid + particle shaders.
    auto ensure_prog = [&](ProgramHandle& slot, const char* fn) {
        if (!slot)
            slot = Ruzino::brush_compile_shader(rc, fn);
    };
    ensure_prog(field->advect_program, "fluid_advect.slang");
    ensure_prog(field->jacobi_program, "fluid_jacobi.slang");
    ensure_prog(field->divergence_program, "fluid_divergence.slang");
    ensure_prog(field->gradient_program, "fluid_gradient.slang");
    ensure_prog(field->damp_dry_program, "fluid_damp_dry.slang");
    ensure_prog(field->field_clear_program, "field_clear.slang");
    ensure_prog(field->ptcl_emit_program, "particle_emit.slang");
    ensure_prog(field->ptcl_update_program, "particle_update.slang");
    ensure_prog(field->ptcl_raster_program, "particle_rasterize.slang");
    ensure_prog(field->bristle_merge_program, "bristle_merge.slang");
    ensure_prog(field->ptcl_flip_pic_program, "particle_flip_pic.slang");
    ensure_prog(field->ptcl_compact_program, "particle_compact.slang");
    ensure_prog(field->ptcl_to_grid_program, "particle_to_grid.slang");
    ensure_prog(field->grid_to_ptcl_program, "grid_to_particle.slang");

    // ======================================================================
    // PARTICLE EMIT + UPDATE (brush_paint_sim ~1614-1833)
    // Only when there is active deposit this frame. The monolith gates this on
    // new_count > 0; in streaming each active frame is "new".
    // ======================================================================
    if (field->particles_initialized && bp.active) {
        Ruzino::ParticleConstants pc = {};
        pc.max_particles = max_ptcl;
        pc.dt = 0.016f;
        pc.D0 = brush_radius * 3.0f;
        pc.friction_delta = 5.0f / pc.D0;
        pc.flip_gamma = 0.8f;
        pc.grid_res = field->grid_res;
        pc.grid_res_z = WIN_Z;
        pc.height_extent = field->grid_height;
        pc.grid_center_z = field->grid_center_z;
        pc.cell_size = cell_sz;
        pc.paper_size = field->grid_paper;
        pc.grid_center_x = field->grid_center.x;
        pc.grid_center_y = field->grid_center.y;
        pc.window_origin_x = field->win_origin_x;
        pc.window_origin_y = field->win_origin_y;
        pc.window_origin_z = 0;
        pc.window_size_x = WIN_XY;
        pc.window_size_z = WIN_Z;
        pc.brush_pos_x = brush_pos_3d.x;
        pc.brush_pos_y = brush_pos_3d.y;
        pc.brush_pos_z = brush_pos_3d.z;
        pc.brush_radius = brush_radius;
        pc.D1 = brush_radius * 0.9f;
        pc.num_bristles = Nb;
        pc.samples_per_bristle = S;
        pc.brush_accel_x = brush_accel_3d.x;
        pc.brush_accel_y = brush_accel_3d.y;
        pc.brush_accel_z = brush_accel_3d.z;
        pc.brush_angular_accel_x = brush_angular_accel.x;
        pc.brush_angular_accel_y = brush_angular_accel.y;
        pc.brush_angular_accel_z = brush_angular_accel.z;

        nvrhi::BufferHandle ptcl_cb;
        Ruzino::brush_upload_cb(
            rc, device, &pc, sizeof(pc), "wb_ptcl_cb", ptcl_cb);

        // Do NOT reset the counter here. Particles are PERSISTENT (paper §4.3:
        // 200K-1M particles survive across frames). The counter carries the
        // alive-particle count from last frame's compact step; emit appends
        // past it via InterlockedAdd, wrapping into the dead slots that
        // compact freed. Resetting here made every frame's emit overwrite
        // slot 0 and destroy the survivors.
        // Emit mode 0 (from bristle samples)
        pc.emit_mode = 0;
        nvrhi::BufferHandle emit0_cb;
        Ruzino::brush_upload_cb(
            rc, device, &pc, sizeof(pc), "wb_emit0_cb", emit0_cb);
        Ruzino::brush_dispatch(
            rc,
            field->ptcl_emit_program,
            { { "sample_pos", field->sample_pos },
              { "sample_color", field->sample_color },
              { "density", field->density } },
            { { "ptcl_counter", field->ptcl_counter },
              { "ptcl_pos", field->ptcl_pos },
              { "ptcl_vel", field->ptcl_vel },
              { "ptcl_color", field->ptcl_color },
              { "ptcl_alive", field->ptcl_alive } },
            emit0_cb,
            Nb * S);
        rc.destroy(emit0_cb);

        // Emit mode 1 (from grid cells) — DISABLED.
        //
        // particle_emit.slang mode 1 reads a grid cell's density and mints a
        // particle whose color is HARDCODED to (0,0,0,density) — it carries
        // DENSITY as mass but ZERO pigment. The particle is not consumed by
        // grid_to_particle (which only fires within D0 of the brush), so
        // outside the brush footprint these zero-color particles survive and
        // are re-rasterized + re-merged into the grid every frame. In
        // bristle_merge.slang the zero-color particle goes through color_mix
        // as new_ryb=(0,0,0), which dilutes the cell's real RYB color toward
        // (0,0,0). In the Gossett&Chen RYB model (0,0,0) = WHITE PAPER, so
        // this dilution rendered saturated strokes white — especially blue,
        // whose RYB->RGB corner (0.163,0.373,0.6) is dim and goes gray/white
        // fastest as the RYB value shrinks. Yellow stayed readable because
        // its corner (1,1,0) is bright. Emit mode 0 (from bristle samples,
        // which carry the real ink pigment) is retained and is the only
        // particle source now.
        //
        // (particle_emit.slang is outside this change set, so the fix is
        // applied here at the dispatch site in the diff'd node file.)

        // Update particles (ping-pong)
        Ruzino::brush_dispatch(
            rc,
            field->ptcl_update_program,
            { { "ptcl_pos", field->ptcl_pos },
              { "ptcl_vel", field->ptcl_vel },
              { "ptcl_color", field->ptcl_color },
              { "ptcl_alive", field->ptcl_alive },
              { "sample_pos", field->sample_pos },
              { "sample_frame", field->sample_frame },
              { "grid_vel_x", field->vel_x },
              { "grid_vel_y", field->vel_y },
              { "grid_vel_z", field->vel_z } },
            { { "ptcl_pos_out", field->ptcl_pos_b },
              { "ptcl_vel_out", field->ptcl_vel_b },
              { "ptcl_alive_out", field->ptcl_alive_b } },
            ptcl_cb,
            max_ptcl);
        std::swap(field->ptcl_pos, field->ptcl_pos_b);
        std::swap(field->ptcl_vel, field->ptcl_vel_b);
        std::swap(field->ptcl_alive, field->ptcl_alive_b);

        // Clear particle accum grids (variadic — MSVC init-list chokes on
        // RefCountPtr<IBuffer>* element types, see deposit.cpp).
        auto clear_grid = [&](auto& buf) {
            Ruzino::brush_dispatch(
                rc,
                field->field_clear_program,
                {},
                { { "field", buf } },
                nullptr,
                win_n3d);
        };
        clear_grid(field->ptcl_density);
        clear_grid(field->ptcl_vel_x);
        clear_grid(field->ptcl_vel_y);
        clear_grid(field->ptcl_vel_z);
        clear_grid(field->ptcl_rast_r);
        clear_grid(field->ptcl_rast_y);
        clear_grid(field->ptcl_rast_b);

        // Rasterize particles
        Ruzino::brush_dispatch(
            rc,
            field->ptcl_raster_program,
            { { "ptcl_pos", field->ptcl_pos },
              { "ptcl_color", field->ptcl_color },
              { "ptcl_vel", field->ptcl_vel },
              { "ptcl_alive", field->ptcl_alive } },
            { { "ptcl_density", field->ptcl_density },
              { "ptcl_vel_x", field->ptcl_vel_x },
              { "ptcl_vel_y", field->ptcl_vel_y },
              { "ptcl_vel_z", field->ptcl_vel_z },
              { "ptcl_color_r", field->ptcl_rast_r },
              { "ptcl_color_y", field->ptcl_rast_y },
              { "ptcl_color_b", field->ptcl_rast_b } },
            ptcl_cb,
            max_ptcl);

        // Merge particle grids into main grids
        Ruzino::SimConstants mc2 = {};
        mc2.res = field->grid_res;
        mc2.cell_size = cell_sz;
        mc2.paper_size = field->grid_paper;
        mc2.ink_amount = ink_amount;
        mc2.oil_density_base = oil_density_in;
        mc2.window_origin_x = field->win_origin_x;
        mc2.window_origin_y = field->win_origin_y;
        mc2.window_origin_z = 0;
        mc2.window_size_x = WIN_XY;
        mc2.window_size_y = WIN_XY;
        mc2.window_size_z = WIN_Z;
        nvrhi::BufferHandle merge_cb;
        Ruzino::brush_upload_cb(
            rc, device, &mc2, sizeof(mc2), "wb_ptcl_merge_cb", merge_cb);

        Ruzino::brush_dispatch(
            rc,
            field->bristle_merge_program,
            { { "bristle_density", field->ptcl_density },
              { "bristle_vel_x", field->ptcl_vel_x },
              { "bristle_vel_y", field->ptcl_vel_y },
              { "bristle_vel_z", field->ptcl_vel_z },
              { "bristle_color_r", field->ptcl_rast_r },
              { "bristle_color_y", field->ptcl_rast_y },
              { "bristle_color_b", field->ptcl_rast_b } },
            { { "density", field->density },
              { "color_r", field->color_r },
              { "color_y", field->color_y },
              { "color_b", field->color_b },
              { "vel_x", field->vel_x },
              { "vel_y", field->vel_y },
              { "vel_z", field->vel_z },
              { "wetness", field->wetness },
              { "oil_density", field->oil_density } },
            merge_cb,
            win_n3d);
        rc.destroy(merge_cb);
        rc.destroy(ptcl_cb);
    }

    // ======================================================================
    // FLUID SOLVE (brush_paint_sim ~1971-2468). One or more substeps based on
    // the frame dt (capped at 16). Each substep: velocity diffuse -> project
    // -> advect velocity -> re-project -> advect scalars -> diffuse scalars
    // -> damp/dry -> FLIP velocity update.
    // ======================================================================
    float dt = payload.delta_time > 0.0f ? payload.delta_time : (1.0f / 60.0f);
    float sim_dt = std::min(dt, 0.05f);
    int wox = field->win_origin_x;
    int woy = field->win_origin_y;

    if (sim_dt > 1e-6f) {
        float max_sub_dt = 2.0f / static_cast<float>(field->grid_res);
        int substeps =
            std::max(1, static_cast<int>(std::ceil(sim_dt / max_sub_dt)));
        substeps = std::min(substeps, 16);
        float sub_dt = sim_dt / static_cast<float>(substeps);

        for (int s = 0; s < substeps; s++) {
            Ruzino::SimConstants fluid_cb = {};
            fluid_cb.res = field->grid_res;
            fluid_cb.res_z = WIN_Z;
            fluid_cb.height_extent = field->grid_height;
            fluid_cb.grid_center_z = field->grid_center_z;
            fluid_cb.cell_size = cell_sz;
            fluid_cb.paper_size = field->grid_paper;
            fluid_cb.dt = sub_dt;
            fluid_cb.viscosity = viscosity;
            fluid_cb.diffusion = diffusion;
            fluid_cb.drying_rate = drying_rate;
            fluid_cb.oil_density_base = oil_density_in;
            fluid_cb.window_origin_x = wox;
            fluid_cb.window_origin_y = woy;
            fluid_cb.window_origin_z = 0;
            fluid_cb.window_size_x = WIN_XY;
            fluid_cb.window_size_y = WIN_XY;
            fluid_cb.window_size_z = WIN_Z;

            nvrhi::BufferHandle cb_buf;
            Ruzino::brush_upload_cb(
                rc, device, &fluid_cb, sizeof(fluid_cb), "wb_fluid_cb", cb_buf);

            // Snapshot velocity for FLIP
            {
                auto snap_cmd = rc.create(CommandListDesc{});
                snap_cmd->open();
                snap_cmd->copyBuffer(
                    field->vel_x_old,
                    0,
                    field->vel_x,
                    0,
                    win_n3d * sizeof(float));
                snap_cmd->copyBuffer(
                    field->vel_y_old,
                    0,
                    field->vel_y,
                    0,
                    win_n3d * sizeof(float));
                snap_cmd->copyBuffer(
                    field->vel_z_old,
                    0,
                    field->vel_z,
                    0,
                    win_n3d * sizeof(float));
                snap_cmd->close();
                device->executeCommandList(snap_cmd);
                device->waitForIdle();
                rc.destroy(snap_cmd);
            }

            // Velocity diffuse (Jacobi, mode 0)
            fluid_cb.jacobi_mode = 0;
            fluid_cb.jacobi_alpha =
                sub_dt * viscosity *
                static_cast<float>(field->grid_res * field->grid_res);
            {
                nvrhi::BufferHandle jcb;
                Ruzino::brush_upload_cb(
                    rc,
                    device,
                    &fluid_cb,
                    sizeof(fluid_cb),
                    "wb_jacobi_cb",
                    jcb);
                // NOTE: do NOT use &field->vel_x here. RefCountPtr overloads
                // operator&() to return IBuffer** (resource.h:307), so the
                // unary-& yields a pointer to the ptr_ MEMBER, not to the
                // RefCountPtr object — a downstream std::swap then swaps raw
                // IBuffer* values, bypassing refcount accounting and
                // corrupting the handles. This was the cause of field->vel_x
                // becoming NULL mid-solve (crash in requireBufferState).
                // std::addressof bypasses the overloaded operator& and returns
                // the true BufferHandle*, so *addr is a correct BufferHandle&
                // alias that swaps through the RefCountPtr move operators.
                nvrhi::BufferHandle* vel_pairs[3][2] = {
                    { std::addressof(field->vel_x),
                      std::addressof(field->vel_x_tmp) },
                    { std::addressof(field->vel_y),
                      std::addressof(field->vel_y_tmp) },
                    { std::addressof(field->vel_z),
                      std::addressof(field->vel_z_tmp) },
                };
                for (auto& pp : vel_pairs) {
                    nvrhi::BufferHandle& in = *pp[0];
                    nvrhi::BufferHandle& out = *pp[1];
                    Ruzino::brush_dispatch(
                        rc,
                        field->jacobi_program,
                        { { "field_in", in },
                          { "rhs", in },
                          { "wetness", field->wetness },
                          { "density", field->density } },
                        { { "field_out", out } },
                        jcb,
                        window_total);
                    std::swap(in, out);
                }
                rc.destroy(jcb);
            }

            // Project (fixed-point, 3 iterations, 2 Jacobi each)
            auto project = [&]() {
                for (int fp = 0; fp < 3; fp++) {
                    Ruzino::brush_dispatch(
                        rc,
                        field->divergence_program,
                        { { "vel_x", field->vel_x },
                          { "vel_y", field->vel_y },
                          { "vel_z", field->vel_z },
                          { "wetness", field->wetness },
                          { "density", field->density } },
                        { { "div_out", field->divergence_buf } },
                        cb_buf,
                        window_total);

                    fluid_cb.jacobi_mode = 1;
                    nvrhi::BufferHandle pcb;
                    Ruzino::brush_upload_cb(
                        rc,
                        device,
                        &fluid_cb,
                        sizeof(fluid_cb),
                        "wb_press_cb",
                        pcb);
                    for (int ji = 0; ji < 2; ji++) {
                        Ruzino::brush_dispatch(
                            rc,
                            field->jacobi_program,
                            { { "field_in", field->pressure_a },
                              { "rhs", field->divergence_buf },
                              { "wetness", field->wetness },
                              { "density", field->density } },
                            { { "field_out", field->pressure_b } },
                            pcb,
                            window_total);
                        std::swap(field->pressure_a, field->pressure_b);
                    }
                    rc.destroy(pcb);

                    Ruzino::brush_dispatch(
                        rc,
                        field->gradient_program,
                        { { "pressure", field->pressure_a },
                          { "wetness", field->wetness },
                          { "density", field->density } },
                        { { "vel_x", field->vel_x },
                          { "vel_y", field->vel_y },
                          { "vel_z", field->vel_z } },
                        cb_buf,
                        window_total);
                }
            };
            project();

            // Advect velocity. See diffuse note above: std::addressof is
            // required because RefCountPtr::operator&() returns IBuffer**.
            nvrhi::BufferHandle* advect_pairs[3][2] = {
                { std::addressof(field->vel_x),
                  std::addressof(field->vel_x_tmp) },
                { std::addressof(field->vel_y),
                  std::addressof(field->vel_y_tmp) },
                { std::addressof(field->vel_z),
                  std::addressof(field->vel_z_tmp) },
            };
            for (auto& pp : advect_pairs) {
                nvrhi::BufferHandle& in = *pp[0];
                nvrhi::BufferHandle& out = *pp[1];
                Ruzino::brush_dispatch(
                    rc,
                    field->advect_program,
                    { { "field_in", in },
                      { "vel_x", field->vel_x },
                      { "vel_y", field->vel_y },
                      { "vel_z", field->vel_z } },
                    { { "field_out", out } },
                    cb_buf,
                    window_total);
                std::swap(in, out);
            }

            // Re-project
            project();

            // Advect scalars (density, color, wetness, oil_density)
            auto advect_scalar = [&](nvrhi::BufferHandle& f,
                                     nvrhi::BufferHandle& tmp) {
                Ruzino::brush_dispatch(
                    rc,
                    field->advect_program,
                    { { "field_in", f },
                      { "vel_x", field->vel_x },
                      { "vel_y", field->vel_y },
                      { "vel_z", field->vel_z } },
                    { { "field_out", tmp } },
                    cb_buf,
                    window_total);
                std::swap(f, tmp);
            };
            advect_scalar(field->density, field->density_tmp);
            advect_scalar(field->color_r, field->color_r_tmp);
            advect_scalar(field->color_y, field->color_y_tmp);
            advect_scalar(field->color_b, field->color_b_tmp);
            advect_scalar(field->wetness, field->wetness_tmp);
            advect_scalar(field->oil_density, field->oil_density_tmp);

            // Scalar diffusion of the COLOR + OIL channels (paper §4.2: "we
            // diffuse the density and color fields"). Diffusing the pigment
            // smooths the per-cell RYB splat noise that showed as speckle on
            // freshly painted strokes.
            //
            // DENSITY is deliberately NOT diffused (see above): it is the
            // paint MASS injected only where bristles touch the canvas;
            // diffusing it pushes the high-density front into zero-pigment
            // cells, which then render white and advance the stroke width
            // past the brush footprint.
            //
            // DENSITY-GATED color diffusion (diffuse_density_gate > 0): a
            // neighbor only joins the 6-cell average if IT holds paint
            // (density > gate). Without this gate, averaging a real pigment
            // against an empty (zero-pigment) neighbor dilutes it toward RYB
            // (0,0,0) = white paper in the Gossett&Chen RYB->RGB map. Blue is
            // the worst case: its RGB corner (0.163,0.373,0.6) is dim, so a
            // half-diluted RYB (0,0,0.5) renders as light gray, not light
            // blue — the "blue stroke renders white" bug. The gate keeps the
            // diffusion inside the painted region so pigment stays saturated
            // while still smoothing the speckle the diffusion was added for.
            // The velocity-viscosity diffuse pass leaves the gate at 0.
            //
            // alpha = dt·diffusion·N² (≈1.7 at res 4096 for the default
            // diffusion 1e-4). The window Neumann BC (fluid_jacobi.slang)
            // makes this a closed domain, so the diffused mass is conserved
            // within the active window.
            fluid_cb.jacobi_mode = 0;
            fluid_cb.jacobi_alpha =
                sub_dt * diffusion *
                static_cast<float>(field->grid_res * field->grid_res);
            fluid_cb.diffuse_density_gate =
                1e-3f;  // only between painted cells
            {
                nvrhi::BufferHandle dcb;
                Ruzino::brush_upload_cb(
                    rc, device, &fluid_cb, sizeof(fluid_cb), "wb_diff_cb", dcb);
                auto diffuse_scalar = [&](nvrhi::BufferHandle& f,
                                          nvrhi::BufferHandle& tmp) {
                    Ruzino::brush_dispatch(
                        rc,
                        field->jacobi_program,
                        { { "field_in", f },
                          { "rhs", f },
                          { "wetness", field->wetness },
                          { "density", field->density } },
                        { { "field_out", tmp } },
                        dcb,
                        window_total);
                    std::swap(f, tmp);
                };
                diffuse_scalar(field->color_r, field->color_r_tmp);
                diffuse_scalar(field->color_y, field->color_y_tmp);
                diffuse_scalar(field->color_b, field->color_b_tmp);
                diffuse_scalar(field->oil_density, field->oil_density_tmp);
                rc.destroy(dcb);
            }
            fluid_cb.diffuse_density_gate = 0.0f;  // restore for velocity pass

            // Damp + dry. Dispatched over the FULL grid (not just the active
            // window) so that paint left behind by a moving brush still dries
            // (§4.2: "increase the dryness of every grid cell"). Velocity damp
            // on empty cells is a no-op (their velocity is already zero).
            Ruzino::brush_dispatch(
                rc,
                field->damp_dry_program,
                { { "oil_density", field->oil_density } },
                { { "vel_x", field->vel_x },
                  { "vel_y", field->vel_y },
                  { "vel_z", field->vel_z },
                  { "wetness", field->wetness } },
                cb_buf,
                global_n3d);

            // FLIP/PIC velocity update for particles
            if (field->particles_initialized) {
                Ruzino::ParticleConstants pc = {};
                pc.max_particles = max_ptcl;
                pc.dt = sub_dt;
                pc.D0 = brush_radius * 3.0f;
                pc.flip_gamma = 0.8f;
                pc.grid_res = field->grid_res;
                pc.grid_res_z = WIN_Z;
                pc.height_extent = field->grid_height;
                pc.grid_center_z = field->grid_center_z;
                pc.cell_size = cell_sz;
                pc.paper_size = field->grid_paper;
                pc.grid_center_x = field->grid_center.x;
                pc.grid_center_y = field->grid_center.y;
                pc.window_origin_x = field->win_origin_x;
                pc.window_origin_y = field->win_origin_y;
                pc.window_origin_z = 0;
                pc.window_size_x = WIN_XY;
                pc.window_size_z = WIN_Z;
                pc.brush_pos_x = brush_pos_3d.x;
                pc.brush_pos_y = brush_pos_3d.y;
                pc.brush_pos_z = brush_pos_3d.z;
                pc.brush_radius = brush_radius;

                nvrhi::BufferHandle flip_cb;
                Ruzino::brush_upload_cb(
                    rc, device, &pc, sizeof(pc), "wb_flip_cb", flip_cb);
                Ruzino::brush_dispatch(
                    rc,
                    field->ptcl_flip_pic_program,
                    { { "ptcl_pos", field->ptcl_pos },
                      { "ptcl_alive", field->ptcl_alive },
                      { "vel_x_old", field->vel_x_old },
                      { "vel_y_old", field->vel_y_old },
                      { "vel_z_old", field->vel_z_old },
                      { "vel_x_new", field->vel_x },
                      { "vel_y_new", field->vel_y },
                      { "vel_z_new", field->vel_z } },
                    { { "ptcl_vel", field->ptcl_vel } },
                    flip_cb,
                    max_ptcl);
                rc.destroy(flip_cb);
            }

            rc.destroy(cb_buf);
        }
    }

    // ======================================================================
    // POST-FLUID particle maintenance (brush_paint_sim ~2470-2566)
    // ======================================================================
    if (field->particles_initialized) {
        Ruzino::ParticleConstants pc = {};
        pc.max_particles = max_ptcl;
        pc.dt = 0.016f;
        pc.D0 = brush_radius * 3.0f;
        pc.grid_res = field->grid_res;
        pc.grid_res_z = WIN_Z;
        pc.height_extent = field->grid_height;
        pc.grid_center_z = field->grid_center_z;
        pc.cell_size = cell_sz;
        pc.paper_size = field->grid_paper;
        pc.grid_center_x = field->grid_center.x;
        pc.grid_center_y = field->grid_center.y;
        pc.window_origin_x = field->win_origin_x;
        pc.window_origin_y = field->win_origin_y;
        pc.window_origin_z = 0;
        pc.window_size_x = WIN_XY;
        pc.window_size_z = WIN_Z;
        pc.brush_pos_x = brush_pos_3d.x;
        pc.brush_pos_y = brush_pos_3d.y;
        pc.brush_pos_z = brush_pos_3d.z;
        pc.brush_radius = brush_radius;

        nvrhi::BufferHandle maint_cb;
        Ruzino::brush_upload_cb(
            rc, device, &pc, sizeof(pc), "wb_maint_cb", maint_cb);

        // Particle to grid (absorb distant slow particles)
        Ruzino::brush_dispatch(
            rc,
            field->ptcl_to_grid_program,
            { { "ptcl_pos", field->ptcl_pos },
              { "ptcl_vel", field->ptcl_vel },
              { "ptcl_color", field->ptcl_color },
              { "ptcl_alive", field->ptcl_alive } },
            { { "density", field->density },
              { "color_r", field->color_r },
              { "color_y", field->color_y },
              { "color_b", field->color_b },
              { "ptcl_alive_out", field->ptcl_alive_b } },
            maint_cb,
            max_ptcl);
        std::swap(field->ptcl_alive, field->ptcl_alive_b);

        // Grid to particle (emit near brush, Eq.15 density subtraction).
        // No counter reset: append past the survivors (compact left them at
        // [0, counter), so InterlockedAdd writes into freed dead slots).
        //
        // Paper §5.2: "c can be any cell near new particles and it does not
        // have to emit any particle." Each emitted particle subtracts its mass
        // (weighted by W) from its 3×3×3 neighborhood, NOT just its emitting
        // cell. The shader writes density_out via RWByteAddressBuffer atomic
        // subtract, so we must seed density_out with the current density
        // (atomic subtract accumulates on top of the seed).
        {
            auto seed_cmd = rc.create(CommandListDesc{});
            seed_cmd->open();
            seed_cmd->copyBuffer(
                field->density_tmp,
                0,
                field->density,
                0,
                static_cast<size_t>(field->grid_res) * field->grid_res_z *
                    field->grid_res * sizeof(float));
            seed_cmd->close();
            device->executeCommandList(seed_cmd);
            device->waitForIdle();
            rc.destroy(seed_cmd);
        }
        Ruzino::brush_dispatch(
            rc,
            field->grid_to_ptcl_program,
            { { "density", field->density },
              { "color_r", field->color_r },
              { "color_y", field->color_y },
              { "color_b", field->color_b },
              { "vel_x", field->vel_x },
              { "vel_y", field->vel_y },
              { "vel_z", field->vel_z } },
            { { "ptcl_counter", field->ptcl_counter },
              { "ptcl_pos", field->ptcl_pos },
              { "ptcl_vel", field->ptcl_vel },
              { "ptcl_color", field->ptcl_color },
              { "ptcl_alive", field->ptcl_alive },
              { "density_out", field->density_tmp } },
            maint_cb,
            win_n3d);
        std::swap(field->density, field->density_tmp);

        // Particle compaction
        Ruzino::brush_reset_counter(rc, device, field->ptcl_counter);
        Ruzino::brush_dispatch(
            rc,
            field->ptcl_compact_program,
            { { "ptcl_alive", field->ptcl_alive },
              { "ptcl_pos", field->ptcl_pos },
              { "ptcl_vel", field->ptcl_vel },
              { "ptcl_color", field->ptcl_color } },
            { { "ptcl_counter", field->ptcl_counter },
              { "ptcl_pos_out", field->ptcl_pos_b },
              { "ptcl_vel_out", field->ptcl_vel_b },
              { "ptcl_color_out", field->ptcl_color_b },
              { "ptcl_alive_out", field->ptcl_alive_b } },
            maint_cb,
            max_ptcl);
        std::swap(field->ptcl_pos, field->ptcl_pos_b);
        std::swap(field->ptcl_vel, field->ptcl_vel_b);
        std::swap(field->ptcl_color, field->ptcl_color_b);
        std::swap(field->ptcl_alive, field->ptcl_alive_b);

        rc.destroy(maint_cb);
    }

    params.set_output("State", zs);
    return true;
}

NODE_DECLARATION_UI(brush_wb_fluid);

NODE_DECLARATION_ALWAYS_DIRTY(brush_wb_fluid);

NODE_DEF_CLOSE_SCOPE
