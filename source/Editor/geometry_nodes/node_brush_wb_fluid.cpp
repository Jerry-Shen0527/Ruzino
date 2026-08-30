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
#include <vector>

#include "GCore/GOP.h"
#include "GCore/geom_payload.hpp"
#include "brush_sim_common.hpp"  // WetbrushSimState, WetbrushZoneState, brush_* helpers
#include "geom_node_base.h"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(brush_wb_fluid)
{
    // Per-frame pen-dynamics sample, forwarded from deposit (so this node
    // knows when the pen is up — pen-up frames still relax the fluid but
    // skip emission).
    b.add_input<Ruzino::StrokeSample>("Stroke Sample");
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

    Ruzino::StrokeSample bp =
        params.get_input<Ruzino::StrokeSample>("Stroke Sample");
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

    // [wb-xfer] stage probes (WB_LEDGER_PROBE=1): full-global density sum at
    // four pipeline stations, to localize which stage destroys mass. Full
    // 268MB readbacks — tail frames only (plus one mid-stroke reference).
    static int xfer_frame = 0;
    const bool ledger_probe =
        [] { const char* e = std::getenv("WB_LEDGER_PROBE"); return e && e[0] == '1'; }();
    const int xf = xfer_frame++;
    auto probe_grid_sum = [&](const char* tag) {
        if (!ledger_probe) return;
        if (!(xf >= 108 || xf == 30)) return;
        std::vector<float> data(global_n3d);
        auto rb = rc.create(
            nvrhi::BufferDesc{}
                .setByteSize(static_cast<size_t>(global_n3d) * sizeof(float))
                .setCpuAccess(nvrhi::CpuAccessMode::Read)
                .setDebugName("wb_xfer_rb"));
        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        cmd->copyBuffer(
            rb, 0, field->density, 0,
            static_cast<size_t>(global_n3d) * sizeof(float));
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        void* mapped = device->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
        memcpy(data.data(), mapped, static_cast<size_t>(global_n3d) * sizeof(float));
        device->unmapBuffer(rb);
        rc.destroy(rb);
        rc.destroy(cmd);
        double sum = 0.0;
        int negs = 0;
        for (int i = 0; i < global_n3d; ++i) {
            sum += data[i];
            if (data[i] < -1e-4f) ++negs;
        }
        spdlog::info("[wb-xfer] f={} {} sum={:.1f} neg={}", xf, tag, sum, negs);
    };
    probe_grid_sum("A_in");

    // Brush pose (grid-local) — needed for the particle CBs.
    glm::vec3 brush_pos_3d = bp.pos;
    brush_pos_3d.x -= field->grid_center.x;
    brush_pos_3d.y -= field->grid_center.y;
    glm::vec3 brush_accel_3d(0.0f);
    glm::vec3 brush_angular_accel(0.0f);

    // Grid↔particle conversion + adhesion ranges (paper §5.2 Table 1): the
    // paper fixes D0 = 1 cm and D1 = 0.3 cm in SI. Its brushes are ~0.55 cm
    // in radius (the §1 demo videos / Fig 1 scale), so in units of the brush
    // radius: D0 ≈ 1.8 R and D1 ≈ 0.55 R (D1/D0 = 0.3 exactly as printed).
    // These are the values used here — the earlier D0 = 1.0 R / D1 = 0.5 R
    // were §17/§20 calibrations against a 60-frame test stroke (ride-time
    // shortening), not paper ratios; per the paper-fidelity directive they
    // are restored. Note the ride time does grow back (~D0 shell width /
    // relative speed): the paper's head-to-trail mass ratio converges over
    // LONG strokes, which a 60-frame test cannot show.
    // R_j consistency: the §5.1 emission radius R_j = cbrt(3·M_max/(4π·ρ₀))
    // ≈ 0.36 R (ρ₀ = 2e4, M_max = WB_M_MAX) stays below D1 = 0.55 R, so
    // newborn particles still ride the brush through Eq.10's adhesion blend
    // max(1 − d_B/D1, 0) ≈ 0.35–1 inside the emission shell.
    const float D0 = brush_radius * 1.8f;
    const float D1 = brush_radius * 0.55f;
    // §5.2 "moves slowly": the paper gives no number. Scaled from real
    // units — a real trail's liquid behind the brush settles below ~2 cm/s,
    // and paper_size = 1 unit ≈ 27 cm (brush radius 0.55 cm : 0.02) →
    // ≈ 0.075 units/s, rounded to 0.1.
    const float slow_deposit_speed = 0.1f;

    // Gravity (world units/s²), physically scaled: 1 unit ≈ 27.5 cm →
    // g = 981 cm/s² / 27.5 ≈ 35.7 units/s². Applied to particles (§4.3 a_k
    // "including the gravity and the friction") and to fluid cells (§4.2
    // standard Eulerian external force — the grid liquid previously had NO
    // force and hovered where deposited). Direction-adjustable for
    // experiments via WB_GRAVITY_X/Y/Z.
    const glm::vec3 gravity = [] {
        auto envf = [](const char* k, float d) {
            const char* v = std::getenv(k);
            return v ? std::atof(v) : d;
        };
        return glm::vec3(
            envf("WB_GRAVITY_X", 0.0f),
            envf("WB_GRAVITY_Y", 0.0f),
            envf("WB_GRAVITY_Z", -35.7f));
    }();

    // Lazily compile the fluid + particle shaders.
    auto ensure_prog = [&](ProgramHandle& slot, const char* fn) {
        if (!slot)
            slot = Ruzino::brush_compile_shader(rc, fn);
    };
    ensure_prog(field->advect_program, "fluid_advect.slang");
    ensure_prog(
        field->advect_scalar_program,
        "fluid_advect_upwind.slang");
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
    ensure_prog(field->field_copy_window_program, "field_copy_window.slang");

    // ======================================================================
    // PARTICLE EMIT + UPDATE (brush_paint_sim ~1614-1833)
    // Only when there is active deposit this frame. The monolith gates this on
    // new_count > 0; in streaming each active frame is "new".
    // ======================================================================
    if (field->particles_initialized && bp.active) {
        Ruzino::ParticleConstants pc = {};
        pc.max_particles = max_ptcl;
        pc.dt = 0.016f;
        pc.D0 = D0;
        pc.pen_down = 1;  // this whole section is gated on bp.active
        pc.friction_delta = 5.0f / D0;
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
        pc.D1 = D1;
        // Brush linear velocity for the Eq.9/10 two-step (the closest
        // sample's frame translates with the brush). prev_brush_vel is
        // updated by the deposit node at the END of each frame, so here it
        // holds the LAST frame's velocity — correct as v_frame for the
        // frame-constant strokes the tests drive.
        pc.brush_vel_x = field->prev_brush_vel.x;
        pc.brush_vel_y = field->prev_brush_vel.y;
        pc.brush_vel_z = field->prev_brush_vel.z;
        pc.num_bristles = Nb;
        pc.samples_per_bristle = S;
        pc.gravity_x = gravity.x;
        pc.gravity_y = gravity.y;
        pc.gravity_z = gravity.z;
        pc.brush_accel_x = brush_accel_3d.x;
        pc.brush_accel_y = brush_accel_3d.y;
        pc.brush_accel_z = brush_accel_3d.z;
        pc.brush_angular_accel_x = brush_angular_accel.x;
        pc.brush_angular_accel_y = brush_angular_accel.y;
        pc.brush_angular_accel_z = brush_angular_accel.z;

        nvrhi::BufferHandle ptcl_cb;
        Ruzino::brush_upload_cb(
            rc, device, &pc, sizeof(pc), "wb_ptcl_cb", ptcl_cb);

        // Paint-particle emission is handled ENTIRELY by the bristle node's
        // §5.1 EMIT pass (bristle_liquid_emit.slang): bristle sample liquid
        // overloads (m_j > (1+ε)M_j) and releases particles carrying its
        // pigment c_j and the excess mass. That is the paper's only paint ->
        // particle path (§5.1).
        //
        // The previous emit-mode-0 / emit-mode-1 dispatches here
        // (particle_emit.slang) are DISABLED. They were decoupled from the
        // §5.1 capacity model: mode 0 fired once per bristle sample every
        // frame based on the global ink_amount (not m_j/M_j overload), and
        // mode 1 minted zero-pigment particles from grid density. Together
        // they flooded the pool and bypassed the sample-liquid conservation,
        // so paint mass grew without bound. particle_emit.slang is kept on
        // disk (not dispatched) for reference.

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
        // Swarm→grid momentum coupling scale (bristle_merge relaxation gate;
        // see the quadratic-momentum note there). WB_VEL_INJECT tunes it.
        static const float vel_inject_scale = [] {
            const char* env = std::getenv("WB_VEL_INJECT");
            return env ? std::max(std::atof(env), 0.0) : 1.0;
        }();
        mc2.velocity_inject_scale = vel_inject_scale;
        nvrhi::BufferHandle merge_cb;
        Ruzino::brush_upload_cb(
            rc, device, &mc2, sizeof(mc2), "wb_ptcl_merge_cb", merge_cb);

        // WB_DISABLE_MERGE=1: diagnostic bisect — skip the swarm→grid
        // momentum merge entirely (velocity family keeps whatever the fluid
        // solve alone produces). Used to attribute the press-phase grid
        // velocity buildup (blob-test explosion hunt). NOT a physics knob.
        static const bool disable_merge = [] {
            const char* env = std::getenv("WB_DISABLE_MERGE");
            return env && std::atoi(env) == 1;
        }();
        if (!disable_merge) {
            Ruzino::brush_dispatch(
                rc,
                field->bristle_merge_program,
                { { "bristle_density", field->ptcl_density },
                  { "bristle_vel_x", field->ptcl_vel_x },
                  { "bristle_vel_y", field->ptcl_vel_y },
                  { "bristle_vel_z", field->ptcl_vel_z } },
                { { "vel_x", field->vel_x },
                  { "vel_y", field->vel_y },
                  { "vel_z", field->vel_z } },
                merge_cb,
                win_n3d);
        }
        rc.destroy(merge_cb);
        rc.destroy(ptcl_cb);
    }
    else if (field->particles_initialized) {
        // Brush up / no deposit this frame: the raster grids still hold the
        // last active frame's swarm splat. pack_float4 composites them into
        // the render field, so a stale raster would ghost paint that the
        // maintenance pass is simultaneously depositing into the canvas —
        // double-visible mass. Clear the 4 pack-relevant raster grids (the
        // vel rasters have no reader outside the merge above).
        //
        // ALSO clear the BRISTLE raster (density + velocities): the bristle
        // node skips pen-up frames, so its rasterized boundary otherwise
        // persists as GHOST NO-FLUX WALLS at the brush's last position —
        // the pressure projection kept diverging around walls that no
        // longer exist, one suspected driver of the blob test's pen-up
        // velocity churn and mass advection.
        nvrhi::BufferHandle* rast_bufs[] = {
            std::addressof(field->ptcl_density),
            std::addressof(field->ptcl_rast_r),
            std::addressof(field->ptcl_rast_y),
            std::addressof(field->ptcl_rast_b),
            std::addressof(field->bristle_density),
            std::addressof(field->bristle_vel_x),
            std::addressof(field->bristle_vel_y),
            std::addressof(field->bristle_vel_z),
        };
        for (nvrhi::BufferHandle* buf : rast_bufs) {
            Ruzino::brush_dispatch(
                rc,
                field->field_clear_program,
                {},
                { { "field", *buf } },
                nullptr,
                win_n3d);
        }
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

        // Velocity decay control (fluid_damp_dry applies it once per
        // substep, so convert: damp_sub = frame_damp^(1/substeps)).
        // PAPER-FAITHFUL DEFAULT = OFF (1.0): the paper eliminates trailing
        // velocity via (a) strong §4.2 viscosity diffusion — the implicit
        // Jacobi at a = dt·visc·N² ≈ 500 homogenizes the window velocity each
        // substep, diluting localized momentum to ~the window mean, and
        // (b) the dryness threshold zeroing velocity, plus the bounded §4.3
        // particle-velocity merge. This global multiplier was a temporary
        // stand-in from the era when the merge accumulated momentum
        // quadratically (see bristle_merge.slang); it stays env-tunable
        // (<1.0) as an escape hatch for stroke-speed extremes.
        static const float vel_damp_frame = [] {
            const char* env = std::getenv("WB_VEL_DAMP");
            float v = env ? std::atof(env) : 1.0f;
            return std::min(std::max(v, 0.0f), 1.0f);
        }();
        const float vel_damp_sub =
            vel_damp_frame > 0.0f
                ? std::pow(vel_damp_frame, 1.0f / static_cast<float>(substeps))
                : 1.0f;

        // DIAGNOSTIC (WB_STAGE_DUMP=1): per-stage |vel| maxima for the f12
        // eruption hunt. The velocity field cannot grow new maxima through
        // convex stages (diffuse = neighbor average, semi-Lagrangian advect =
        // convex sample), so the first stage whose max jumps is the injector.
        // Reads back the WINDOW-SIZED vel buffers + div/pressure after each
        // project. Substep 0 only. NOT a physics knob.
        static const bool stage_dump = [] {
            const char* env = std::getenv("WB_STAGE_DUMP");
            return env && std::atoi(env) == 1;
        }();
        static int stage_frame = 0;
        ++stage_frame;
        auto buf_absmax = [&](const nvrhi::BufferHandle& buf) -> float {
            std::vector<float> data(win_n3d);
            auto rb = rc.create(
                nvrhi::BufferDesc{}
                    .setByteSize(static_cast<size_t>(win_n3d) * sizeof(float))
                    .setCpuAccess(nvrhi::CpuAccessMode::Read)
                    .setDebugName("wb_stage_rb"));
            auto cmd = rc.create(CommandListDesc{});
            cmd->open();
            cmd->copyBuffer(
                rb, 0, buf, 0, static_cast<size_t>(win_n3d) * sizeof(float));
            cmd->close();
            device->executeCommandList(cmd);
            device->waitForIdle();
            void* mapped = device->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
            memcpy(
                data.data(),
                mapped,
                static_cast<size_t>(win_n3d) * sizeof(float));
            device->unmapBuffer(rb);
            rc.destroy(rb);
            rc.destroy(cmd);
            float mx = 0.0f;
            for (float v : data)
                mx = std::max(mx, std::fabs(v));
            return mx;
        };
        // Same readback but also reports the argmax window-local coordinates
        // (lx, ly, lz) of the |max| cell — needed to tell WHICH face of WHICH
        // cell a projection-stage spike lives at.
        auto buf_absmax_loc = [&](const nvrhi::BufferHandle& buf,
                                  int& lx,
                                  int& ly,
                                  int& lz) -> float {
            std::vector<float> data(win_n3d);
            auto rb = rc.create(
                nvrhi::BufferDesc{}
                    .setByteSize(static_cast<size_t>(win_n3d) * sizeof(float))
                    .setCpuAccess(nvrhi::CpuAccessMode::Read)
                    .setDebugName("wb_stage_rb"));
            auto cmd = rc.create(CommandListDesc{});
            cmd->open();
            cmd->copyBuffer(
                rb, 0, buf, 0, static_cast<size_t>(win_n3d) * sizeof(float));
            cmd->close();
            device->executeCommandList(cmd);
            device->waitForIdle();
            void* mapped = device->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
            memcpy(
                data.data(),
                mapped,
                static_cast<size_t>(win_n3d) * sizeof(float));
            device->unmapBuffer(rb);
            rc.destroy(rb);
            rc.destroy(cmd);
            float mx = 0.0f;
            int best = 0;
            for (int i = 0; i < win_n3d; ++i) {
                float a = std::fabs(data[i]);
                if (a > mx) {
                    mx = a;
                    best = i;
                }
            }
            const int wxy = WIN_XY * WIN_XY;
            lz = best / wxy;
            int rem = best - lz * wxy;
            ly = rem / WIN_XY;
            lx = rem - ly * WIN_XY;
            return data[best];
        };
        auto vel_stage = [&](const char* stage) {
            if (!stage_dump)
                return;
            int lx = -1, ly = -1, lz = -1;
            float mv = 0.0f;
            int comp = -1;
            float sv = buf_absmax_loc(field->vel_x, lx, ly, lz);
            if (std::fabs(sv) >= std::fabs(mv)) {
                mv = sv;
                comp = 0;
            }
            int x2, y2, z2;
            sv = buf_absmax_loc(field->vel_y, x2, y2, z2);
            if (std::fabs(sv) > std::fabs(mv)) {
                mv = sv;
                comp = 1;
                lx = x2;
                ly = y2;
                lz = z2;
            }
            sv = buf_absmax_loc(field->vel_z, x2, y2, z2);
            if (std::fabs(sv) > std::fabs(mv)) {
                mv = sv;
                comp = 2;
                lx = x2;
                ly = y2;
                lz = z2;
            }
            spdlog::info(
                "[wb-stage] f={} {} vabsmax={:.5f} at (lx{},ly{},lz{},c{})",
                stage_frame,
                stage,
                mv,
                lx,
                ly,
                lz,
                comp);
        };
        auto solve_stage = [&](const char* stage) {
            if (!stage_dump)
                return;
            int lx, ly, lz;
            float dv = buf_absmax_loc(field->divergence_buf, lx, ly, lz);
            spdlog::info(
                "[wb-stage] f={} {} div={} at (lx{},ly{},lz{}) pmax={:.5f}",
                stage_frame,
                stage,
                dv,
                lx,
                ly,
                lz,
                buf_absmax(field->pressure_a));
        };

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
            // Brush-interior boundary for pressure projection (paper §4.2):
            // bristle-occupied cells act as no-flux walls. The gate is small
            // so only cells genuinely under bristles block the flow; empty
            // cells and thin paint do not. bristle_density is the §4.1
            // rasterized field (window-sized), bound into divergence/jacobi/
            // gradient. See those shaders' is_brush_g.
            //
            // Diagnostic bisect switches for the blob-test f12 eruption hunt
            // (NOT physics knobs): WB_NO_PROJECT=1 skips both pressure
            // projections; WB_NO_BRUSH_WALL=1 disables the §4.2 bristle
            // no-flux/moving-wall BC (gate huge → is_brush_g always false).
            // The gate MUST be decided BEFORE cb_buf is uploaded below — the
            // divergence/gradient dispatches inside project() reuse cb_buf,
            // so patching fluid_cb afterwards silently did nothing and the
            // WB_NO_BRUSH_WALL bisect was invalid.
            static const bool no_project = [] {
                const char* env = std::getenv("WB_NO_PROJECT");
                return env && std::atoi(env) == 1;
            }();
            static const float wall_gate = [] {
                const char* env = std::getenv("WB_NO_BRUSH_WALL");
                if (env && std::atoi(env) == 1)
                    return 1e9f;
                // Tunable via WB_WALL_GATE (default 0.01: only cells
                // genuinely under bristles count as brush interior). A 0.001
                // gate ("any splat") was tested for the blob lift trail and
                // made no difference — the trail cells carry no bristle
                // splat at all (sample mass is depleted by lift time).
                const char* gate_env = std::getenv("WB_WALL_GATE");
                return gate_env ? static_cast<float>(
                                      std::max(std::atof(gate_env), 0.0))
                                : 0.01f;
            }();
            fluid_cb.brush_boundary_gate = wall_gate;
            fluid_cb.velocity_damp = vel_damp_sub;
            fluid_cb.copy_mode =
                0;  // window→window (both buffers window-sized)
            fluid_cb.advect_field_window_local = 0;
            fluid_cb.gravity_x = gravity.x;
            fluid_cb.gravity_y = gravity.y;
            fluid_cb.gravity_z = gravity.z;

            nvrhi::BufferHandle cb_buf;
            Ruzino::brush_upload_cb(
                rc, device, &fluid_cb, sizeof(fluid_cb), "wb_fluid_cb", cb_buf);

            // Snapshot velocity for FLIP (Eq.11 needs pre-projection vel).
            // The active window is a NON-CONTIGUOUS block inside the global
            // buffer (each row of WIN_XY cells is separated by res−WIN_XY
            // stride cells), so a plain copyBuffer(src, dst, win_n3d) would
            // copy the CORNER block at offset 0 — wrong once the window moves
            // off the corner. The previous code did exactly that, so FLIP read
            // stale corner velocities at particle positions (vel_old was only
            // ever valid at the grid corner). field_copy_window maps each
            // window cell to its global index and copies that exact cell.
            Ruzino::brush_dispatch(
                rc,
                field->field_copy_window_program,
                { { "src_field", field->vel_x } },
                { { "dst_field", field->vel_x_old } },
                cb_buf,
                win_n3d);
            Ruzino::brush_dispatch(
                rc,
                field->field_copy_window_program,
                { { "src_field", field->vel_y } },
                { { "dst_field", field->vel_y_old } },
                cb_buf,
                win_n3d);
            Ruzino::brush_dispatch(
                rc,
                field->field_copy_window_program,
                { { "src_field", field->vel_z } },
                { { "dst_field", field->vel_z_old } },
                cb_buf,
                win_n3d);
            // brush_dispatch submits internally; ensure the snapshot lands
            // before the solve dispatches read vel_*_old.
            device->waitForIdle();
            if (s == 0)
                vel_stage("pre_diffuse");

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
                    // Paper-style solver iteration (§4.2/Algorithm 1 uses
                    // three fixed-point iterations): several Jacobi sweeps
                    // per substep actually resolve the implicit high-
                    // viscosity diffusion. ONE sweep left local spikes
                    // (moving-wall injections, venturi through the brush
                    // gap) alive at ~10x brush speed, which advected the
                    // trail into torn bands; 3 sweeps spread the momentum
                    // over the √α≈22-cell diffusion radius at a lower peak.
                    for (int vs = 0; vs < 3; vs++) {
                        Ruzino::brush_dispatch(
                            rc,
                            field->jacobi_program,
                            { { "field_in", in },
                              { "rhs", in },
                              { "wetness", field->wetness },
                              { "density", field->density },
                              { "bristle_density", field->bristle_density },
                              // Mode 0 (diffuse) never reads the swarm
                              // raster, but the shader DECLARES the slot —
                              // an unbound declared slot fails binding-set
                              // creation and the whole dispatch is skipped.
                              { "ptcl_density", field->ptcl_density } },
                            { { "field_out", out } },
                            jcb,
                            window_total);
                        std::swap(in, out);
                    }
                }
                rc.destroy(jcb);
            }
            if (s == 0)
                vel_stage("post_diffuse");

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
                          { "density", field->density },
                          { "bristle_density", field->bristle_density },
                          { "bristle_vel_x", field->bristle_vel_x },
                          { "bristle_vel_y", field->bristle_vel_y },
                          { "bristle_vel_z", field->bristle_vel_z },
                          { "ptcl_density", field->ptcl_density } },
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
                              { "density", field->density },
                              { "bristle_density", field->bristle_density },
                              { "ptcl_density", field->ptcl_density } },
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
                          { "density", field->density },
                          { "bristle_density", field->bristle_density },
                          { "bristle_vel_x", field->bristle_vel_x },
                          { "bristle_vel_y", field->bristle_vel_y },
                          { "bristle_vel_z", field->bristle_vel_z },
                          { "ptcl_density", field->ptcl_density } },
                        { { "vel_x", field->vel_x },
                          { "vel_y", field->vel_y },
                          { "vel_z", field->vel_z } },
                        cb_buf,
                        window_total);
                }
            };

            // Diagnostic bisect switches (definitions live with the CB
            // upload above, where they actually take effect): WB_NO_PROJECT
            // skips both pressure projections; WB_NO_BRUSH_WALL sets the
            // projection wall gate to 1e9 via the uploaded CB.
            if (!no_project)
                project();
            if (s == 0) {
                vel_stage("post_project1");
                solve_stage("after_project1");
            }

            // Advect velocity. See diffuse note above: std::addressof is
            // required because RefCountPtr::operator&() returns IBuffer**.
            // field_in is the window-local velocity itself → the flag CB.
            fluid_cb.advect_field_window_local = 1;
            nvrhi::BufferHandle advect_vel_cb;
            Ruzino::brush_upload_cb(
                rc,
                device,
                &fluid_cb,
                sizeof(fluid_cb),
                "wb_adv_vel_cb",
                advect_vel_cb);
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
                    advect_vel_cb,
                    window_total);
                std::swap(in, out);
            }
            rc.destroy(advect_vel_cb);
            if (s == 0)
                vel_stage("post_advect");

            // Re-project
            if (!no_project)
                project();
            if (s == 0) {
                vel_stage("post_project2");
                solve_stage("after_project2");
            }

            // Advect scalars (density, color, wetness, oil_density). These are
            // GLOBAL canvas fields: read globally (flag 0, the main cb_buf),
            // advected into the WINDOW-SIZED tmp, then copied back over the
            // window region (field_copy_window mode 2). No buffer swap — the
            // tmp and the field have different sizes.
            fluid_cb.copy_mode = 2;
            nvrhi::BufferHandle advect_scalar_cb;
            Ruzino::brush_upload_cb(
                rc,
                device,
                &fluid_cb,
                sizeof(fluid_cb),
                "wb_adv_scalar_cb",
                advect_scalar_cb);
            // CONSERVATIVE scalar advection: upwind flux-form (see
            // fluid_advect_upwind.slang's header — the value-based
            // semi-Lagrangian read created/destroyed canvas mass at the
            // floor/window boundaries). Window-local advect + window→global
            // copy back, same plumbing as before; only the transport scheme
            // changed. Velocity keeps the paper's semi-Lagrangian above.
            auto advect_scalar = [&](nvrhi::BufferHandle& f,
                                     nvrhi::BufferHandle& tmp) {
                Ruzino::brush_dispatch(
                    rc,
                    field->advect_scalar_program,
                    { { "field_in", f },
                      { "vel_x", field->vel_x },
                      { "vel_y", field->vel_y },
                      { "vel_z", field->vel_z } },
                    { { "field_out", tmp } },
                    advect_scalar_cb,
                    window_total);
                Ruzino::brush_dispatch(
                    rc,
                    field->field_copy_window_program,
                    { { "src_field", tmp } },
                    { { "dst_field", f } },
                    advect_scalar_cb,
                    win_n3d);
            };
            advect_scalar(field->density, field->density_tmp);
            advect_scalar(field->color_r, field->color_r_tmp);
            advect_scalar(field->color_y, field->color_y_tmp);
            advect_scalar(field->color_b, field->color_b_tmp);
            advect_scalar(field->wetness, field->wetness_tmp);
            advect_scalar(field->oil_density, field->oil_density_tmp);
            rc.destroy(advect_scalar_cb);

            // NOTE: no scalar diffusion step here. Paper §4.2/§5.1 line 209
            // applies viscosity to the VELOCITY field only (done in
            // fluid_damp_dry.slang as per-cell drag), NOT to density/color/
            // wetness scalars. A previous Jacobi-mode-0 diffusion of the
            // scalars (alpha = dt*diffusion*N² ≈ 26 at res 512) was eroding
            // stroke edges below the render threshold every frame, which
            // looked like the finished stroke contracting/shrinking over
            // time. Removing it matches the paper and eliminates the shrink.

            // Damp + dry. Dispatched over the FULL grid (not just the active
            // window) so that paint left behind by a moving brush still dries
            // (§4.2: "increase the dryness of every grid cell"). Velocity damp
            // on empty cells is a no-op (their velocity is already zero).
            // Damp + dry, split along the buffer residency (see
            // fluid_damp_dry.slang): a GLOBAL wetness-only pass so paint left
            // behind by a moving brush still dries (§4.2: "increase the
            // dryness of every grid cell"), and a WINDOW velocity pass (the
            // velocity family only exists inside the window).
            fluid_cb.damp_mode = 0;
            nvrhi::BufferHandle damp_global_cb;
            Ruzino::brush_upload_cb(
                rc,
                device,
                &fluid_cb,
                sizeof(fluid_cb),
                "wb_damp_g_cb",
                damp_global_cb);
            Ruzino::brush_dispatch(
                rc,
                field->damp_dry_program,
                { { "density", field->density } },
                { { "wetness", field->wetness } },
                damp_global_cb,
                global_n3d);
            rc.destroy(damp_global_cb);

            fluid_cb.damp_mode = 1;
            nvrhi::BufferHandle damp_window_cb;
            Ruzino::brush_upload_cb(
                rc,
                device,
                &fluid_cb,
                sizeof(fluid_cb),
                "wb_damp_w_cb",
                damp_window_cb);
            Ruzino::brush_dispatch(
                rc,
                field->damp_dry_program,
                { { "density", field->density } },
                { { "vel_x", field->vel_x },
                  { "vel_y", field->vel_y },
                  { "vel_z", field->vel_z },
                  { "wetness", field->wetness } },
                damp_window_cb,
                window_total);
            rc.destroy(damp_window_cb);

            // FLIP/PIC velocity update for particles
            if (field->particles_initialized) {
                Ruzino::ParticleConstants pc = {};
                pc.max_particles = max_ptcl;
                pc.dt = sub_dt;
                pc.D0 = D0;  // match particle/maintenance D0
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
                pc.brush_vel_x = field->prev_brush_vel.x;
                pc.brush_vel_y = field->prev_brush_vel.y;
                pc.brush_vel_z = field->prev_brush_vel.z;

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
        pc.D0 = D0;  // match particle-section D0
        // D1 (adhesion range, Eq.10) — grid_to_particle drains within D0 per
        // paper §5.2; D1 is carried for reference but unused by the drain.
        pc.D1 = D1;
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
        pc.brush_vel_x = field->prev_brush_vel.x;
        pc.brush_vel_y = field->prev_brush_vel.y;
        pc.brush_vel_z = field->prev_brush_vel.z;
        // num_bristles / samples_per_bristle: REQUIRED by particle_to_grid's
        // d_{B,k} nearest-sample query (§5.2). Without these, num_samples = 0,
        // the scan finds no bristle, d_B defaults to sqrt(1e30)≈3e15, and
        // EVERY particle reads as "far from bristles" → instant deposit,
        // regardless of D0. This was the hidden reason widening D0/D1 had no
        // effect: the d_B was always astronomically larger than any D0.
        pc.num_bristles = Nb;
        pc.samples_per_bristle = S;
        pc.slow_deposit_speed = slow_deposit_speed;
        pc.gravity_x = gravity.x;
        pc.gravity_y = gravity.y;
        pc.gravity_z = gravity.z;
        // Pen state for the §5.2 conversions below: pen-up deposits the
        // carried swarm (d_{B,k} vs stalled samples means nothing) and
        // suspends grid→particle conversion.
        pc.pen_down = bp.active ? 1 : 0;

        nvrhi::BufferHandle maint_cb;
        Ruzino::brush_upload_cb(
            rc, device, &pc, sizeof(pc), "wb_maint_cb", maint_cb);

        probe_grid_sum("B_solve");

        // Particle to grid (deposit distant slow particles, §5.2 Eq.16).
        // Binds sample_pos so the shader can compute d_{B,k} (distance to the
        // nearest bristle sample) instead of the brush-center distance.
        Ruzino::brush_dispatch(
            rc,
            field->ptcl_to_grid_program,
            { { "ptcl_pos", field->ptcl_pos },
              { "ptcl_vel", field->ptcl_vel },
              { "ptcl_color", field->ptcl_color },
              { "ptcl_alive", field->ptcl_alive },
              { "sample_pos", field->sample_pos } },
            { { "density", field->density },
              { "color_r", field->color_r },
              { "color_y", field->color_y },
              { "color_b", field->color_b },
              { "ptcl_alive_out", field->ptcl_alive_b },
              { "wetness", field->wetness } },
            maint_cb,
            max_ptcl);
        std::swap(field->ptcl_alive, field->ptcl_alive_b);
        probe_grid_sum("C_p2g");

        // Grid to particle (emit near brush, Eq.15 density subtraction).
        // No counter reset: append past the survivors (compact left them at
        // [0, counter), so InterlockedAdd writes into freed dead slots).
        //
        // Paper §5.2: "c can be any cell near new particles and it does not
        // have to emit any particle." Each emitted particle subtracts its mass
        // (weighted by W) from its 3×3×3 neighborhood, NOT just its emitting
        // cell. The *_out tmps are WINDOW-SIZED: seed them from the global
        // canvas fields over the window (field_copy_window mode 1), let the
        // shader subtract, then copy the results back (mode 2). No buffer
        // swap — the tmp and the field have different sizes.
        {
            Ruzino::SimConstants seed_cb = {};
            seed_cb.res = field->grid_res;
            seed_cb.res_z = field->grid_res_z;
            seed_cb.window_origin_x = field->win_origin_x;
            seed_cb.window_origin_y = field->win_origin_y;
            seed_cb.window_origin_z = 0;
            seed_cb.window_size_x = WIN_XY;
            seed_cb.window_size_y = WIN_XY;
            seed_cb.window_size_z = WIN_Z;
            seed_cb.copy_mode = 1;  // global → window
            nvrhi::BufferHandle g2p_copy_cb;
            Ruzino::brush_upload_cb(
                rc,
                device,
                &seed_cb,
                sizeof(seed_cb),
                "wb_g2p_copy_cb",
                g2p_copy_cb);
            auto win_copy = [&](nvrhi::BufferHandle& src,
                                nvrhi::BufferHandle& dst) {
                Ruzino::brush_dispatch(
                    rc,
                    field->field_copy_window_program,
                    { { "src_field", src } },
                    { { "dst_field", dst } },
                    g2p_copy_cb,
                    win_n3d);
            };
            win_copy(field->density, field->density_tmp);
            win_copy(field->color_r, field->color_r_tmp);
            win_copy(field->color_y, field->color_y_tmp);
            win_copy(field->color_b, field->color_b_tmp);
            rc.destroy(g2p_copy_cb);
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
              { "density_out", field->density_tmp },
              { "color_r_out", field->color_r_tmp },
              { "color_y_out", field->color_y_tmp },
              { "color_b_out", field->color_b_tmp } },
            maint_cb,
            win_n3d);
        // Copy the subtracted window regions back into the global fields.
        {
            Ruzino::SimConstants writeback_cb = {};
            writeback_cb.res = field->grid_res;
            writeback_cb.res_z = field->grid_res_z;
            writeback_cb.window_origin_x = field->win_origin_x;
            writeback_cb.window_origin_y = field->win_origin_y;
            writeback_cb.window_origin_z = 0;
            writeback_cb.window_size_x = WIN_XY;
            writeback_cb.window_size_y = WIN_XY;
            writeback_cb.window_size_z = WIN_Z;
            writeback_cb.copy_mode = 2;  // window → global
            nvrhi::BufferHandle g2p_wb_cb;
            Ruzino::brush_upload_cb(
                rc,
                device,
                &writeback_cb,
                sizeof(writeback_cb),
                "wb_g2p_wb_cb",
                g2p_wb_cb);
            auto copy_back = [&](nvrhi::BufferHandle& src,
                                 nvrhi::BufferHandle& dst) {
                Ruzino::brush_dispatch(
                    rc,
                    field->field_copy_window_program,
                    { { "src_field", src } },
                    { { "dst_field", dst } },
                    g2p_wb_cb,
                    win_n3d);
            };
            copy_back(field->density_tmp, field->density);
            copy_back(field->color_r_tmp, field->color_r);
            copy_back(field->color_y_tmp, field->color_y);
            copy_back(field->color_b_tmp, field->color_b);
            rc.destroy(g2p_wb_cb);
        }
        probe_grid_sum("D_g2p");

        // Particle compaction. Zero the OUTPUT alive buffer first: compact
        // only writes packed survivors' flags, so slots above the live count
        // must be cleared by us or they keep the ping-pong buffer's stale
        // alive=1 flags (deposited particles resurrected at their pre-deposit
        // positions every frame, re-depositing their mass — see
        // particle_compact.slang).
        Ruzino::brush_dispatch(
            rc,
            field->field_clear_program,
            {},
            { { "field", field->ptcl_alive_b } },
            nullptr,
            max_ptcl);
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
