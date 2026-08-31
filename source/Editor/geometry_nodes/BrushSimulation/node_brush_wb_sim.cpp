// node_brush_wb_sim — the whole Wetbrush per-frame simulation as ONE node.
//
// Previously three chained nodes (brush_wb_deposit -> brush_wb_bristle ->
// brush_wb_fluid). The split was a graph-authoring choice, not a paper one:
// the paper's algorithm is a single per-step loop, and the three stages share
// one WetbrushZoneState anyway — the node boundaries only forwarded the same
// shared_ptr while locking the §5.1 exchange to frame granularity. This node
// restores the single-loop structure; the phases run in the paper's order and
// chain ONE WetbrushZoneState by reference (the merged execute reads the
// optional sim_in feedback once — absent on the init frame, where PHASE 1's
// allocation fills it, and later phases must see that allocation):
//
//   PHASE 1 (§4.1/§4.2, ex-brush_wb_deposit)     lazy alloc, active-window
//       follow/scroll, dip, per-substep bristle dynamics -> psi/BC raster
//   PHASE 2 (§5.1, ex-brush_wb_bristle)          ABSORB + EMIT (sample <-> FLIP
//       particles liquid exchange)
//   PHASE 3 (§4.3/§4.2/§5.2, ex-brush_wb_fluid)  particle cycle + swarm
//       raster/merge, grid solve (diffuse/project/advect/damp-dry),
//       Eq.15/Eq.16 transfers, pool compaction
//
// Sockets are the union of the three old nodes (defaults unchanged); the
// "Stroke Sample" pass-through output and the never-consumed "Bristle
// Samples" readback output are gone. brush_wb_commit stays a separate node —
// it is the render-facing packer, not simulation.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "GCore/GOP.h"
#include "GCore/geom_payload.hpp"
#include "GPUContext/compute_context.hpp"  // CommandListDesc
#include "RHI/ResourceManager/resource_allocator.hpp"
#include "brush_sim_common.hpp"  // StrokeSample, WetbrushSimState, WetbrushZoneState, brush_* helpers
#include "geom_node_base.h"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(brush_wb_sim)
{
    // Per-frame pen-dynamics sample from the emitter (interior edge, fresh
    // each frame): position + orientation + analytic derivatives when the
    // source knows them (has_dynamics).
    b.add_input<Ruzino::StrokeSample>("Stroke Sample");
    // The fed-back paint field. Optional: absent on the init frame (the
    // PHASE-1 allocation fills it). Never call get_input on an unwired
    // optional — the executor sets its input pointer to nullptr and
    // get_input would deref it.
    b.add_input<Ruzino::WetbrushZoneState>("State").optional(true);
    // Grid / canvas domain params. WORLD SCALE (doc §33): 1 unit = 1 cm —
    // canvas 10×10 cm, brush head 1 cm wide (radius 0.5). Every length
    // parameter below is cm.
    b.add_input<int>("Resolution").default_val(512).min(64).max(4096);
    b.add_input<int>("Resolution Z").default_val(32).min(4).max(128);
    b.add_input<float>("Paper Size").default_val(10.0f).min(0.1f).max(50.0f);
    b.add_input<float>("Canvas Center X").default_val(0.0f);
    b.add_input<float>("Canvas Center Y").default_val(0.0f);
    b.add_input<float>("Canvas Z").default_val(0.0f);
    b.add_input<float>("Canvas Height").default_val(0.0f).min(0.0f).max(20.0f);
    // Brush / stroke params (PHASE 1 + 3; PHASE 2 reads Brush Radius only).
    b.add_input<float>("Brush Radius").default_val(0.5f).min(0.01f).max(5.0f);
    b.add_input<float>("Brush Pressure").default_val(1.0f).min(0.0f).max(4.0f);
    b.add_input<float>("Ink Amount").default_val(0.8f).min(0.0f).max(2.0f);
    b.add_input<float>("Oil Density").default_val(0.5f).min(0.0f).max(1.0f);
    // RYB ink color. Optional because vec3 sockets can't carry a default_val
    // through serialization; the exec falls back to red when unwired.
    b.add_input<glm::vec3>("Ink Color").optional(true);
    // Fluid solve params (PHASE 3).
    // Kinematic viscosity in cm²/s (water ≈ 0.01, glycerin ≈ 1, thick
    // acrylic ≈ 2–20). The Jacobi coefficient consumes it world-absolutely
    // (a = dt·ν/h²); see the solve loop. Default tints toward acrylic so the
    // wake freezes within a frame at the 1u=1cm calibration.
    b.add_input<float>("Viscosity").default_val(2.0f).min(0.0f).max(50.0f);
    b.add_input<float>("Diffusion Rate")
        .default_val(0.0001f)
        .min(0.0f)
        .max(0.01f);
    // Default 2.0/s: a deposit (wetness 1.0) reaches the dry-solid threshold
    // (0.01) in ~0.5 s — the paper's §4.2 trail-freezing ("once the dryness
    // reaches a threshold, we ignore its velocity and treat it as solid").
    // The old 0.1/s left paint mobile for ~600 frames — a whole session — so
    // wake+sag smeared the trail and BANKED it against the active-window's
    // no-flux edges (the "mystery liquid at the window edge, far from the
    // brush" artifact of 2026-08-31). Paper gives no rate; this is the style
    // knob. Validated at 2-3/s (doc §32).
    // Linear dryness rate (wetness/s). 12/s ≈ touch-dry (wetness 0.01) in
    // 0.083 s ≈ one brush-width of travel at 2.5 cm/s: the §5.2 drain disc
    // then only churns the wet head under the brush instead of the whole
    // trail (doc §34). The paper gives no number ("increase the dryness ...
    // by a small amount", §4.2).
    b.add_input<float>("Drying Rate").default_val(12.0f).min(0.0f).max(50.0f);

    // Outgoing paint field (allocated/updated) for brush_wb_commit + the
    // next-frame feedback.
    b.add_output<Ruzino::WetbrushZoneState>("State");
}

NODE_EXECUTION_FUNCTION(brush_wb_sim)
{
    using Ruzino::WetbrushSimState;
    using Ruzino::WetbrushZoneState;

    // zs arrives from the optional sim_in feedback: absent on the init
    // frame — the allocation below fills it, and all three phases chain
    // that ONE state object.
    WetbrushZoneState zs;
    if (params.has_input("State"))
        zs = params.get_input<Ruzino::WetbrushZoneState>("State");
    auto& field = zs.state;  // shared_ptr<WetbrushSimState>

    Ruzino::StrokeSample bp =
        params.get_input<Ruzino::StrokeSample>("Stroke Sample");
    int resolution = params.get_input<int>("Resolution");
    int resolution_z = params.get_input<int>("Resolution Z");
    float paper_size = params.get_input<float>("Paper Size");
    glm::vec2 canvas_center_xy(
        params.get_input<float>("Canvas Center X"),
        params.get_input<float>("Canvas Center Y"));
    float canvas_z = params.get_input<float>("Canvas Z");
    float canvas_height_in = params.get_input<float>("Canvas Height");
    float brush_radius = params.get_input<float>("Brush Radius");
    float brush_pressure = params.get_input<float>("Brush Pressure");
    float ink_amount = params.get_input<float>("Ink Amount");
    float oil_density_in = params.get_input<float>("Oil Density");
    // Ink color: prefer the StrokeSample's trajectory color (enables
    // multi-color strokes where each point carries its own RYB); fall back
    // to the static "Ink Color" socket when the emitter didn't supply one
    // (single-color).
    glm::vec3 ink_color = params.has_input("Ink Color")
                              ? params.get_input<glm::vec3>("Ink Color")
                              : glm::vec3(1.0f, 0.0f, 0.0f);
    if (bp.active) {
        ink_color = bp.color;
    }

    // Pen state for downstream nodes (§5.1 absorb/emit gating + §5.2
    // conversion gating): a pen-up brush is not painting — no sample
    // uptake, no emission, and no grid↔particle conversion zone around it.
    // Recorded every frame BEFORE any early return so it never goes stale.
    if (field) {
        field->pen_down = bp.active;
        field->dip_frame = bp.stroke_start;
    }

    auto& rc = get_resource_allocator();
    auto device = RHI::get_device();
    auto payload = params.get_global_payload<GeomPayload>();
    float dt = payload.delta_time > 0.0f ? payload.delta_time : (1.0f / 60.0f);

    // ======================================================================
    // LAZY FIELD ALLOCATION (mirrors brush_paint_sim ~463-818)
    // Done once, when the grid dimensions are first known / change.
    // Allocates the FULL buffer set so velocity / bristle / particle
    // persist across frames via the zone feedback.
    // ======================================================================
    bool need_alloc = !field || !field->center_initialized ||
                      field->grid_alloc_res != resolution ||
                      field->grid_alloc_res_z != resolution_z;

    if (need_alloc) {
        if (!field)
            field = std::make_shared<WetbrushSimState>();
        int rz = resolution_z > 0 ? resolution_z : 32;
        float height = canvas_height_in > 1e-6f
                           ? canvas_height_in
                           : paper_size * static_cast<float>(rz) /
                                 static_cast<float>(resolution);
        field->grid_center = canvas_center_xy;
        field->grid_height = height;
        field->grid_center_z = canvas_z + height * 0.5f;
        field->grid_paper = paper_size;
        field->grid_res = resolution;
        field->grid_res_z = rz;
        field->center_initialized = true;
        field->grid_alloc_res = resolution;
        field->grid_alloc_res_z = rz;
        field->win_alloc_z = rz;
        field->win_origin_set = false;
        field->deposited_count = 0;
        field->last_sim_time = -1.0f;
        field->has_prev_brush_pos = false;
        field->prev_brush_vel = glm::vec3(0.0f);
        field->prev_angular_vel = glm::vec3(0.0f);
        field->bristles_initialized = false;
        field->particles_initialized = false;

        // Global grid size: all 3D buffers are now global (res × res ×
        // res_z), not window-sized. Paper §4.2: a large 3D grid is the
        // persistent paint store; the window is only a dispatch range.
        int alloc_win_n3d = resolution * resolution * rz;

        // Bristle/particle accumulation grids (Group B/C) are allocated at
        // the active-window size, not the full global grid. Paper §5/§5.1:
        // bristle samples and particles only exist within the brush-local
        // compute window (WIN_ALLOC_XY × WIN_ALLOC_XY × res_z), so a
        // full-grid alloc wastes (res/WIN)²× the memory for buffers that
        // are cleared and rebuilt each sub-step anyway. At res=4096 this is
        // the difference between 14 buffers × 1B cells (out of memory) and
        // 14 × 1M cells. Shaders index these with window-local coords
        // (global cell minus window_origin); see bristle_rasterize /
        // particle_rasterize / bristle_merge / bristle_liquid_transfer.
        int win_alloc_n3d = WetbrushSimState::win_alloc_xy() *
                            WetbrushSimState::win_alloc_xy() * rz;

        auto safe_destroy = [&](nvrhi::BufferHandle& h) {
            if (h) {
                rc.destroy(h);
                h = nullptr;
            }
        };
        // Helper: destroy a fixed set of buffers by reference. Using a
        // variadic instead of a braced-init-list of addresses, because
        // MSVC's initializer_list deduction chokes on RefCountPtr<IBuffer>*
        // element types (its implicit conversion to IBuffer* makes the list
        // element type ambiguous, surfacing as C2440 "IBuffer** ->
        // BufferHandle*").
        auto destroy_buffers = [&](auto&... bufs) {
            (safe_destroy(bufs), ...);
        };
        // Release every buffer (full set — a grid change invalidates all).
        destroy_buffers(
            field->density,
            field->density_tmp,
            field->color_r,
            field->color_y,
            field->color_b,
            field->color_r_tmp,
            field->color_y_tmp,
            field->color_b_tmp,
            field->vel_x,
            field->vel_x_tmp,
            field->vel_y,
            field->vel_y_tmp,
            field->vel_z,
            field->vel_z_tmp,
            field->wetness,
            field->wetness_tmp,
            field->oil_density,
            field->oil_density_tmp,
            field->height_field,
            field->pressure_a,
            field->pressure_b,
            field->divergence_buf,
            field->bristle_density,
            field->bristle_vel_x,
            field->bristle_vel_y,
            field->bristle_vel_z,
            field->bristle_color_r,
            field->bristle_color_y,
            field->bristle_color_b,
            field->ptcl_density,
            field->ptcl_vel_x,
            field->ptcl_vel_y,
            field->ptcl_vel_z,
            field->ptcl_rast_r,
            field->ptcl_rast_y,
            field->ptcl_rast_b,
            field->vel_x_old,
            field->vel_y_old,
            field->vel_z_old,
            field->packed_paint);

        auto make_buf = [&](const char* name) {
            return Ruzino::brush_create_field_buffer(rc, alloc_win_n3d, name);
        };
        // Window-sized factory for Group B/C (bristle/particle accumulation
        // grids). See win_alloc_n3d comment above.
        auto make_win_buf = [&](const char* name) {
            return Ruzino::brush_create_field_buffer(rc, win_alloc_n3d, name);
        };

        // Canvas state (persistent paint store) — GLOBAL-sized. Paper §4.2:
        // "a large 3D grid stores all cells". These are the only fields
        // that genuinely need global extent.
        field->density = make_buf("wb_density");
        field->color_r = make_buf("wb_color_r");
        field->color_y = make_buf("wb_color_y");
        field->color_b = make_buf("wb_color_b");
        field->wetness = make_buf("wb_wetness");
        field->oil_density = make_buf("wb_oil_density");
        // height_field is NOT allocated: its only writer was the legacy
        // brush_deposit.slang path (never dispatched in the wb pipeline)
        // and no reader exists. Keeping it global-sized cost a full grid
        // buffer. TRANSIENT SOLVE FIELDS — window-sized (paper §4.2: "we
        // can restrict grid-based simulation to a small active window
        // around the brush", 128×128×32). Velocity/pressure/divergence live
        // only in the window; the scalar tmps are window scratch for
        // advection and the §5.2 Eq.15 subtraction (host copies between the
        // two index spaces via field_copy_window). 18 global buffers → ~4MB
        // each: saves ~4.8GB at res 1024.
        field->density_tmp = make_win_buf("wb_density_tmp");
        field->color_r_tmp = make_win_buf("wb_color_r_tmp");
        field->color_y_tmp = make_win_buf("wb_color_y_tmp");
        field->color_b_tmp = make_win_buf("wb_color_b_tmp");
        field->vel_x = make_win_buf("wb_vel_x");
        field->vel_x_tmp = make_win_buf("wb_vel_x_tmp");
        field->vel_y = make_win_buf("wb_vel_y");
        field->vel_y_tmp = make_win_buf("wb_vel_y_tmp");
        field->vel_z = make_win_buf("wb_vel_z");
        field->vel_z_tmp = make_win_buf("wb_vel_z_tmp");
        field->wetness_tmp = make_win_buf("wb_wetness_tmp");
        field->oil_density_tmp = make_win_buf("wb_oil_density_tmp");
        field->pressure_a = make_win_buf("wb_pressure_a");
        field->pressure_b = make_win_buf("wb_pressure_b");
        field->divergence_buf = make_win_buf("wb_divergence");
        // Group B (bristle accumulation grids) — window-sized.
        field->bristle_density = make_win_buf("wb_bristle_density");
        field->bristle_vel_x = make_win_buf("wb_bristle_vel_x");
        field->bristle_vel_y = make_win_buf("wb_bristle_vel_y");
        field->bristle_vel_z = make_win_buf("wb_bristle_vel_z");
        field->bristle_color_r = make_win_buf("wb_bristle_color_r");
        field->bristle_color_y = make_win_buf("wb_bristle_color_y");
        field->bristle_color_b = make_win_buf("wb_bristle_color_b");
        // Group C (particle rasterize grids) — window-sized.
        field->ptcl_density = make_win_buf("wb_ptcl_density");
        field->ptcl_vel_x = make_win_buf("wb_ptcl_vel_x");
        field->ptcl_vel_y = make_win_buf("wb_ptcl_vel_y");
        field->ptcl_vel_z = make_win_buf("wb_ptcl_vel_z");
        field->ptcl_rast_r = make_win_buf("wb_ptcl_rast_r");
        field->ptcl_rast_y = make_win_buf("wb_ptcl_rast_y");
        field->ptcl_rast_b = make_win_buf("wb_ptcl_rast_b");
        field->vel_x_old = make_win_buf("wb_vel_x_old");
        field->vel_y_old = make_win_buf("wb_vel_y_old");
        field->vel_z_old = make_win_buf("wb_vel_z_old");

        // Zero the 4 pack-relevant particle raster grids at allocation: GPU
        // heap allocations are NOT zero-initialized, and pack_float4.slang
        // composites these into the render field from the very first cook —
        // before the fluid node's per-frame clear+raster has ever run (and
        // on frames before the brush first deposits). Garbage there would
        // render as a noise cloud on frame 1.
        if (!field->field_clear_program)
            field->field_clear_program =
                Ruzino::brush_compile_shader(rc, "field_clear.slang");
        nvrhi::BufferHandle* rast_bufs[] = {
            std::addressof(field->ptcl_density),
            std::addressof(field->ptcl_rast_r),
            std::addressof(field->ptcl_rast_y),
            std::addressof(field->ptcl_rast_b),
        };
        for (nvrhi::BufferHandle* buf : rast_bufs) {
            Ruzino::brush_dispatch(
                rc,
                field->field_clear_program,
                {},
                { { "field", *buf } },
                nullptr,
                win_alloc_n3d);
        }
        // Float4 packed paint field (density,r,g,b) — global grid sized,
        // for the shared GPU buffer registry (zero-copy sim→render). Needs
        // CanHaveRawViews because the render rprim binds it as a
        // RawBuffer_SRV (ByteAddressBuffer); brush_create_typed_buffer only
        // sets TypedViews, which produces a view-mismatch (shader reads
        // zeroes). Built inline rather than via the factory to keep the
        // factory's flag set unchanged for the many other callers.
        field->packed_paint = rc.create(
            nvrhi::BufferDesc{}
                .setByteSize(
                    static_cast<size_t>(alloc_win_n3d) * sizeof(float) * 4)
                .setStructStride(sizeof(float) * 4)
                .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                .setKeepInitialState(true)
                .setCanHaveUAVs(true)
                .setCanHaveTypedViews(true)
                .setCanHaveRawViews(true)
                .setDebugName("wb_packed_paint"));

        // Zero-init everything. Variadic write (same MSVC init-list reason
        // as destroy_buffers above). Two groups: full-grid (alloc_win_n3d)
        // and window-sized (win_alloc_n3d) for Group B/C.
        std::vector<float> zeros3d(alloc_win_n3d, 0.0f);
        std::vector<float> zeros_win(win_alloc_n3d, 0.0f);
        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        auto write_3d = [&](auto&... bufs) {
            (cmd->writeBuffer(
                 bufs, zeros3d.data(), alloc_win_n3d * sizeof(float)),
             ...);
        };
        auto write_win = [&](auto&... bufs) {
            (cmd->writeBuffer(
                 bufs, zeros_win.data(), win_alloc_n3d * sizeof(float)),
             ...);
        };
        // Canvas state is global-sized; every transient solve field is
        // window-sized (see the allocation comments above).
        write_3d(
            field->density,
            field->color_r,
            field->color_y,
            field->color_b,
            field->wetness,
            field->oil_density);
        write_win(
            field->density_tmp,
            field->color_r_tmp,
            field->color_y_tmp,
            field->color_b_tmp,
            field->vel_x,
            field->vel_x_tmp,
            field->vel_y,
            field->vel_y_tmp,
            field->vel_z,
            field->vel_z_tmp,
            field->wetness_tmp,
            field->oil_density_tmp,
            field->pressure_a,
            field->pressure_b,
            field->divergence_buf,
            field->vel_x_old,
            field->vel_y_old,
            field->vel_z_old,
            field->bristle_density,
            field->bristle_vel_x,
            field->bristle_vel_y,
            field->bristle_vel_z,
            field->bristle_color_r,
            field->bristle_color_y,
            field->bristle_color_b,
            field->ptcl_density,
            field->ptcl_vel_x,
            field->ptcl_vel_y,
            field->ptcl_vel_z,
            field->ptcl_rast_r,
            field->ptcl_rast_y,
            field->ptcl_rast_b);
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        rc.destroy(cmd);

        spdlog::info(
            "brush_wb_sim: allocated global 3D grid {}x{}x{}, "
            "paper={:.3f}, height={:.3f}",
            resolution,
            resolution,
            rz,
            paper_size,
            height);
    }

    const int WIN_XY =
        std::min(WetbrushSimState::win_alloc_xy(), field->grid_res);
    const int WIN_Z = field->grid_res_z;
    const int win_n3d = WIN_XY * WIN_XY * WIN_Z;
    const float cell_sz =
        field->grid_paper / static_cast<float>(field->grid_res);

    // ======================================================================
    // LAZY BRISTLE / PARTICLE BUFFER ALLOCATION (brush_paint_sim ~630-812)
    // ======================================================================
    const int Nb = WetbrushSimState::NUM_BRISTLES;
    const int M = WetbrushSimState::VERTS_PER_BRISTLE;
    const int S = WetbrushSimState::SAMPLES_PER_BRISTLE;

    if (!field->bristles_initialized) {
        auto safe_destroy = [&](nvrhi::BufferHandle& h) {
            if (h) {
                rc.destroy(h);
                h = nullptr;
            }
        };
        auto destroy_buffers = [&](auto&... bufs) {
            (safe_destroy(bufs), ...);
        };
        destroy_buffers(
            field->bristle_data,
            field->lambda_buf,
            field->sample_pos,
            field->sample_vel,
            field->sample_color,
            field->sample_frame,
            field->sample_liquid,
            field->sample_liquid_b,
            field->sample_supply,
            field->bristle_input_color_buf);

        field->bristle_data = Ruzino::brush_create_typed_buffer(
            rc,
            Nb * M,
            WetbrushSimState::BRISTLE_VERTEX_STRIDE,
            "wb_bristle_data");
        field->lambda_buf = Ruzino::brush_create_typed_buffer(
            rc, Nb * M, sizeof(float), "wb_lambda");
        field->sample_pos = Ruzino::brush_create_typed_buffer(
            rc, Nb * S, sizeof(float) * 4, "wb_sample_pos");
        field->sample_vel = Ruzino::brush_create_typed_buffer(
            rc, Nb * S, sizeof(float) * 4, "wb_sample_vel");
        field->sample_color = Ruzino::brush_create_typed_buffer(
            rc, Nb * S, sizeof(float) * 4, "wb_sample_color");
        field->sample_frame = Ruzino::brush_create_typed_buffer(
            rc, Nb * S, sizeof(float) * 4 * 3, "wb_sample_frame");
        field->sample_liquid = Ruzino::brush_create_typed_buffer(
            rc, Nb * S, sizeof(float) * 4, "wb_sample_liquid");
        field->sample_liquid_b = Ruzino::brush_create_typed_buffer(
            rc, Nb * S, sizeof(float) * 4, "wb_sample_liquid_b");
        field->sample_supply = Ruzino::brush_create_typed_buffer(
            rc, Nb * S, sizeof(float), "wb_sample_supply");
        field->bristle_input_color_buf = Ruzino::brush_create_typed_buffer(
            rc, 1, sizeof(float) * 4, "wb_bristle_input_color");

        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        std::vector<float> z_bristle(
            Nb * M * (WetbrushSimState::BRISTLE_VERTEX_STRIDE / sizeof(float)),
            0.0f);
        cmd->writeBuffer(
            field->bristle_data,
            z_bristle.data(),
            z_bristle.size() * sizeof(float));
        std::vector<float> z_sample(Nb * S * 4, 0.0f);
        auto write_sample4 = [&](auto&... bufs) {
            (cmd->writeBuffer(
                 bufs, z_sample.data(), Nb * S * sizeof(float) * 4),
             ...);
        };
        write_sample4(
            field->sample_pos,
            field->sample_vel,
            field->sample_color,
            field->sample_liquid_b);
        // sample_liquid allocation stays DRY (m_j = 0). The dip (saturated
        // m_j = WB_M_MAX + ink pigment) is written at every stroke_start by
        // the SUB-STEP LOOP below — allocation can precede the first stroke
        // point, and re-dips must pick up the current stroke's ink color.
        // The pigment c_j is seeded to the ink color so color_mix has a
        // base.
        std::vector<float> liquid_init(Nb * S * 4, 0.0f);
        for (int i = 0; i < Nb * S; ++i) {
            liquid_init[i * 4 + 0] = 0.0f;  // m_j = 0 (dry)
            liquid_init[i * 4 + 1] = ink_color.r;
            liquid_init[i * 4 + 2] = ink_color.g;
            liquid_init[i * 4 + 3] = ink_color.b;
        }
        cmd->writeBuffer(
            field->sample_liquid,
            liquid_init.data(),
            Nb * S * sizeof(float) * 4);
        std::vector<float> z_frame(Nb * S * 4 * 3, 0.0f);
        cmd->writeBuffer(
            field->sample_frame,
            z_frame.data(),
            Nb * S * sizeof(float) * 4 * 3);
        float input_color[4] = {
            ink_color.r, ink_color.g, ink_color.b, ink_amount
        };
        cmd->writeBuffer(
            field->bristle_input_color_buf, input_color, sizeof(float) * 4);
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        rc.destroy(cmd);

        field->bristles_initialized = true;
    }

    // Refresh the bristle ink color EVERY active frame (not just on alloc):
    // the StrokeSample's color can change mid-simulation (multi-color
    // strokes), so the init-block write above (which only runs once) would
    // leave a stale color. This write is cheap (4 floats) and runs only
    // when the pen is down.
    if (field->bristles_initialized && bp.active) {
        float input_color[4] = {
            ink_color.r, ink_color.g, ink_color.b, ink_amount
        };
        auto color_cmd = rc.create(CommandListDesc{});
        color_cmd->open();
        color_cmd->writeBuffer(
            field->bristle_input_color_buf, input_color, sizeof(float) * 4);
        color_cmd->close();
        device->executeCommandList(color_cmd);
        device->waitForIdle();
        rc.destroy(color_cmd);
    }

    if (!field->particles_initialized) {
        int max_ptcl = WetbrushSimState::MAX_PARTICLES;
        auto safe_destroy = [&](nvrhi::BufferHandle& h) {
            if (h) {
                rc.destroy(h);
                h = nullptr;
            }
        };
        auto destroy_buffers = [&](auto&... bufs) {
            (safe_destroy(bufs), ...);
        };
        destroy_buffers(
            field->ptcl_pos,
            field->ptcl_vel,
            field->ptcl_color,
            field->ptcl_alive,
            field->ptcl_counter,
            field->emit_budget,
            field->ptcl_pos_b,
            field->ptcl_vel_b,
            field->ptcl_color_b,
            field->ptcl_alive_b);

        field->ptcl_pos = Ruzino::brush_create_typed_buffer(
            rc, max_ptcl, sizeof(float) * 4, "wb_ptcl_pos");
        field->ptcl_vel = Ruzino::brush_create_typed_buffer(
            rc, max_ptcl, sizeof(float) * 4, "wb_ptcl_vel");
        field->ptcl_color = Ruzino::brush_create_typed_buffer(
            rc, max_ptcl, sizeof(float) * 4, "wb_ptcl_color");
        field->ptcl_alive = Ruzino::brush_create_typed_buffer(
            rc, max_ptcl, sizeof(uint32_t), "wb_ptcl_alive");
        field->ptcl_counter = Ruzino::brush_create_byte_buffer(
            rc, sizeof(uint32_t), "wb_ptcl_counter");
        field->emit_budget = Ruzino::brush_create_byte_buffer(
            rc, sizeof(uint32_t), "wb_emit_budget");
        field->ptcl_pos_b = Ruzino::brush_create_typed_buffer(
            rc, max_ptcl, sizeof(float) * 4, "wb_ptcl_pos_b");
        field->ptcl_vel_b = Ruzino::brush_create_typed_buffer(
            rc, max_ptcl, sizeof(float) * 4, "wb_ptcl_vel_b");
        field->ptcl_color_b = Ruzino::brush_create_typed_buffer(
            rc, max_ptcl, sizeof(float) * 4, "wb_ptcl_color_b");
        field->ptcl_alive_b = Ruzino::brush_create_typed_buffer(
            rc, max_ptcl, sizeof(uint32_t), "wb_ptcl_alive_b");

        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        std::vector<float> z_ptcl(max_ptcl * 4, 0.0f);
        auto write_ptcl4 = [&](auto&... bufs) {
            (cmd->writeBuffer(
                 bufs, z_ptcl.data(), max_ptcl * sizeof(float) * 4),
             ...);
        };
        write_ptcl4(
            field->ptcl_pos,
            field->ptcl_vel,
            field->ptcl_color,
            field->ptcl_color_b);
        auto write_ptcl3 = [&](auto&... bufs) {
            (cmd->writeBuffer(
                 bufs, z_ptcl.data(), max_ptcl * sizeof(float) * 3),
             ...);
        };
        write_ptcl3(field->ptcl_pos_b, field->ptcl_vel_b);
        std::vector<uint32_t> z_u(max_ptcl, 0);
        auto write_u = [&](auto&... bufs) {
            (cmd->writeBuffer(bufs, z_u.data(), max_ptcl * sizeof(uint32_t)),
             ...);
        };
        write_u(field->ptcl_alive, field->ptcl_alive_b);
        uint32_t zero_c = 0;
        cmd->writeBuffer(field->ptcl_counter, &zero_c, sizeof(uint32_t));
        cmd->writeBuffer(field->emit_budget, &zero_c, sizeof(uint32_t));
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        rc.destroy(cmd);

        field->particles_initialized = true;
    }

    // ======================================================================
    // LAZY SHADER COMPILATION (brush_paint_sim ~818-884). Persists in
    // field.
    // ======================================================================
    auto ensure_prog = [&](ProgramHandle& slot, const char* fn) {
        if (!slot)
            slot = Ruzino::brush_compile_shader(rc, fn);
    };
    ensure_prog(field->field_clear_program, "field_clear.slang");
    ensure_prog(field->pack_program, "pack_float4.slang");
    ensure_prog(field->bristle_sim_program, "bristle_simulate.slang");
    ensure_prog(
        field->bristle_density_constraint_program,
        "bristle_density_constraint.slang");
    ensure_prog(field->bristle_resample_program, "bristle_resample.slang");
    ensure_prog(field->bristle_raster_program, "bristle_rasterize.slang");
    ensure_prog(field->bristle_merge_program, "bristle_merge.slang");

    // Init frame: no simulation yet — forward the (allocated) field.
    // Pen-UP frames do NOT return here anymore: the brush still exists in
    // the air, so the bristle chains must keep following it (paper §4.1
    // dynamics are contact-independent). The pen-up relax path lives at the
    // sub-step loop below; skipping the brush entirely left bristle_data
    // zero-initialized during hover/descend/lift and the debug render
    // showed a single dot at the world origin.
    if (!payload.is_simulating) {
        // The ex-phase gates each returned true and the merged execute then
        // still wrote the output — replicate that: the commit node and the
        // zone feedback need the State output on the init frame too.
        params.set_output("State", zs);
        return true;
    }

    // ======================================================================
    // BRUSH POSE — prefer the sample's analytic dynamics, fall back to host
    // finite-differencing (brush_paint_sim ~938-998).
    // StrokeSample (the pen-dynamics contract) carries vel / angular_vel /
    // orientation when the trajectory source knows them analytically
    // (has_dynamics): use them directly and difference only ONCE for
    // accel / omega_dot (first-order on exact derivatives). Position-only
    // sources (real captured input) keep the legacy path: difference THIS
    // sample against the field's prev_* (one sample/frame).
    // ======================================================================
    glm::vec3 brush_pos_3d = bp.pos;
    brush_pos_3d.x -= field->grid_center.x;
    brush_pos_3d.y -= field->grid_center.y;

    glm::vec3 brush_vel_3d(0.0f);
    glm::vec3 brush_accel_3d(0.0f);
    glm::vec3 brush_angular_vel(0.0f);
    glm::vec3 brush_angular_accel(0.0f);
    // Heading angle (XY). LEGACY: no shader consumes brush_rotation — the
    // pen's orientation now travels as the brush_R* matrix rows (below).
    // Kept only so the CB field stays populated; do not build on it.
    float brush_rotation = 0.0f;

    if (bp.stroke_start) {
        // Fresh pen-down. With analytic dynamics the pen is ALREADY moving
        // at pen-down: adopt the sample's derivatives this very frame — the
        // substep-loop tail stamps prev_brush_vel from brush_vel_3d, so
        // this also seeds the next frame's accel difference correctly (a
        // from-rest assumption would manufacture a vel/dt acceleration
        // spike on frame 2). Position-only sources keep "no inherited
        // motion" (unknown).
        field->has_prev_brush_pos = false;
        if (bp.has_dynamics) {
            brush_vel_3d = bp.vel;
            brush_angular_vel = bp.angular_vel;
        }
        field->prev_brush_vel = glm::vec3(0.0f);
        field->prev_angular_vel = glm::vec3(0.0f);
    }
    else if (field->has_prev_brush_pos) {
        if (bp.has_dynamics) {
            // Analytic path: trust the sample's derivatives. accel /
            // omega_dot stay differenced (one subtraction on exact values —
            // no double amplification of position quantization noise).
            brush_vel_3d = bp.vel;
            if (dt > 1e-6f)
                brush_accel_3d = (bp.vel - field->prev_brush_vel) / dt;
            brush_angular_vel = bp.angular_vel;
            if (dt > 1e-6f)
                brush_angular_accel =
                    (bp.angular_vel - field->prev_angular_vel) / dt;
            brush_rotation = brush_vel_3d.x != 0.0f || brush_vel_3d.y != 0.0f
                                 ? atan2(brush_vel_3d.y, brush_vel_3d.x)
                                 : 0.0f;
        }
        else {
            // Legacy finite-difference path (captured input without
            // derivatives). new_vel is the frame displacement; divide by dt
            // to get TRUE velocity (units/s). The bristle shader uses
            // brush_vel as a velocity (v_B = vel - brush_vel, vel = v_B +
            // brush_vel) and the paper's Eq.2 Coriolis term is 2ω×v_B —
            // feeding a per-frame displacement here made v_B 60× too small,
            // so the bristles barely felt the brush's motion. With velocity
            // units, brush_accel_3d below is a real acceleration (units/s²)
            // and the rectilinear term a_B in Eq.2 is correct.
            glm::vec3 new_vel = (brush_pos_3d - field->prev_brush_pos) / dt;
            if (dt > 1e-6f)
                brush_accel_3d = (new_vel - field->prev_brush_vel) / dt;
            brush_vel_3d = new_vel;
            brush_rotation = atan2(brush_vel_3d.y, brush_vel_3d.x);

            // Angular velocity about canvas normal (Z), wrapped [-pi, pi].
            float prev_rot =
                atan2(field->prev_brush_vel.y, field->prev_brush_vel.x);
            float dtheta = brush_rotation - prev_rot;
            dtheta = atan2(sin(dtheta), cos(dtheta));
            if (dt > 1e-6f) {
                glm::vec3 new_omega(0.0f, 0.0f, dtheta / dt);
                brush_angular_accel =
                    (new_omega - field->prev_angular_vel) / dt;
                brush_angular_vel = new_omega;
            }
        }
    }

    // Pen orientation → rotation-matrix rows for BristleConstants::brush_R*
    // (columns of mat3_cast = world images of the local axes; the shader
    // combines rot(v) = v.x*R0 + v.y*R1 + v.z*R2, see bristle_simulate).
    glm::quat brush_orientation = bp.orientation;
    glm::mat3 brush_rot_m = glm::mat3_cast(brush_orientation);

    // [wb-pose] gate-A diagnostic (see docs §24): the pose that the whole
    // downstream chain actually receives, per frame.
    if (std::getenv("WB_DEBUG_DUMP_PTCL")) {
        static int pose_frame = 0;
        // Tilt = angle between the pen axis (local -Z in world, i.e.
        // -mat3_cast(q)·e_z) and the world-down direction: cos = R_col2.z.
        float tilt_deg = glm::degrees(
            acosf(std::min(1.0f, std::max(-1.0f, brush_rot_m[2].z))));
        spdlog::info(
            "[wb-pose] f={} t={:.3f} pos=({:.3f},{:.3f},{:.3f}) "
            "|v|={:.3f} |a|={:.3f} |w|={:.4f} tilt={:.1f}deg dyn={}",
            pose_frame++,
            bp.time,
            brush_pos_3d.x,
            brush_pos_3d.y,
            brush_pos_3d.z,
            glm::length(brush_vel_3d),
            glm::length(brush_accel_3d),
            glm::length(brush_angular_vel),
            tilt_deg,
            bp.has_dynamics ? 1 : 0);
    }

    // ======================================================================
    // position_window — center the active window's dispatch range on a
    // brush XY. Paper §4.2: "We update the window location at the beginning
    // of each time step." Canvas cells are global/persistent and untouched
    // by the move; the PERSISTENT window-sized fields (velocity family +
    // the pressure warm-start) must be SCROLLED: their window-local layout
    // is relative to the origin, so the overlap region is re-based and the
    // cells left behind the window are discarded (no liquid motion outside
    // it).
    // ======================================================================
    auto position_window = [&](float bx, float by) {
        int old_ox = field->win_origin_x;
        int old_oy = field->win_origin_y;
        bool first = !field->win_origin_set;

        float half_p = field->grid_paper * 0.5f;
        float bgx = (bx - field->grid_center.x + half_p) / cell_sz;
        float bgy = (by - field->grid_center.y + half_p) / cell_sz;
        int new_wox = static_cast<int>(bgx) - WIN_XY / 2;
        int new_woy = static_cast<int>(bgy) - WIN_XY / 2;
        new_wox = std::max(0, std::min(new_wox, field->grid_res - WIN_XY));
        new_woy = std::max(0, std::min(new_woy, field->grid_res - WIN_XY));

        field->win_origin_x = new_wox;
        field->win_origin_y = new_woy;
        field->win_origin_z = 0;
        field->win_origin_set = true;

        if (first || (new_wox == old_ox && new_woy == old_oy))
            return;  // no move (or no prior window to scroll from)

        // Scroll the persistent window fields: vel_x/y/z + pressure_a.
        // window_scroll writes the re-based contents into a scratch buffer
        // (same size), then we swap handles so the field keeps the data.
        if (!field->window_scroll_program)
            field->window_scroll_program =
                Ruzino::brush_compile_shader(rc, "window_scroll.slang");
        struct ScrollCB {
            int old_ox, old_oy, old_oz;
            int new_ox, new_oy, new_oz;
            int wsx, wsz;
        };
        ScrollCB scb{ old_ox, old_oy, 0, new_wox, new_woy, 0, WIN_XY, WIN_Z };
        nvrhi::BufferHandle scroll_cb;
        Ruzino::brush_upload_cb(
            rc, device, &scb, sizeof(scb), "wb_scroll_cb", scroll_cb);
        auto scroll = [&](nvrhi::BufferHandle& field_buf,
                          nvrhi::BufferHandle& scratch) {
            Ruzino::brush_dispatch(
                rc,
                field->window_scroll_program,
                { { "src_field", field_buf } },
                { { "dst_field", scratch } },
                scroll_cb,
                win_n3d);
            std::swap(field_buf, scratch);
        };
        scroll(field->vel_x, field->vel_x_tmp);
        scroll(field->vel_y, field->vel_y_tmp);
        scroll(field->vel_z, field->vel_z_tmp);
        scroll(field->pressure_a, field->pressure_b);
        rc.destroy(scroll_cb);
    };

    // ======================================================================
    // deposit_at — the full Wetbrush §4.1 bristle deposit pipeline at one
    // sub-step brush pose (brush_paint_sim ~1115-1373).
    // ======================================================================
    auto deposit_at = [&](const glm::vec3& sub_pos,
                          const glm::vec3& sub_vel,
                          const glm::vec3& sub_accel,
                          float sub_rot,
                          const glm::vec3& sub_omega,
                          const glm::vec3& sub_omega_dot,
                          float dt_sub,
                          bool deposit) {
        Ruzino::BristleConstants bc = {};
        bc.num_bristles = Nb;
        bc.verts_per_bristle = M;
        bc.samples_per_bristle = S;
        bc.beta_B = 0.05f;
        bc.dt = dt_sub;
        bc.brush_pos_x = sub_pos.x;
        bc.brush_pos_y = sub_pos.y;
        bc.brush_pos_z = sub_pos.z;
        bc.brush_vel_x = sub_vel.x;
        bc.brush_vel_y = sub_vel.y;
        bc.brush_vel_z = sub_vel.z;
        bc.brush_angular_vel_x = sub_omega.x;
        bc.brush_angular_vel_y = sub_omega.y;
        bc.brush_angular_vel_z = sub_omega.z;
        bc.brush_rotation = sub_rot;
        bc.brush_accel_x = sub_accel.x;
        bc.brush_accel_y = sub_accel.y;
        bc.brush_accel_z = sub_accel.z;
        bc.brush_angular_accel_x = sub_omega_dot.x;
        bc.brush_angular_accel_y = sub_omega_dot.y;
        bc.brush_angular_accel_z = sub_omega_dot.z;
        bc.brush_pressure = brush_pressure;
        bc.canvas_z = field->grid_center_z - field->grid_height * 0.5f;
        bc.brush_radius = brush_radius;
        bc.spring_k = 50.0f;
        bc.damping = 5.0f;
        bc.grid_res = field->grid_res;
        bc.grid_res_z = WIN_Z;
        bc.height_extent = field->grid_height;
        bc.grid_center_z = field->grid_center_z;
        bc.cell_size = cell_sz;
        bc.paper_size = field->grid_paper;
        bc.grid_center_x = field->grid_center.x;
        bc.grid_center_y = field->grid_center.y;
        bc.window_origin_x = field->win_origin_x;
        bc.window_origin_y = field->win_origin_y;
        bc.window_origin_z = 0;
        bc.window_size_x = WIN_XY;
        bc.window_size_z = WIN_Z;
        bc.prev_brush_pos_x = 0.0f;
        bc.prev_brush_pos_y = 0.0f;
        bc.prev_brush_pos_z = 0.0f;
        bc.has_prev_brush_pos = 0;
        bc.sweep_steps = 1;
        bc._sweep_pad0 = 0.0f;
        bc._sweep_pad1 = 0.0f;
        // Pen orientation rows (StrokeSample.orientation → mat3 columns).
        // Identity = upright brush; bristle_simulate builds the footprint
        // disk and the bristle growth axis from these.
        bc.brush_R0 = glm::vec4(brush_rot_m[0], 0.0f);
        bc.brush_R1 = glm::vec4(brush_rot_m[1], 0.0f);
        bc.brush_R2 = glm::vec4(brush_rot_m[2], 0.0f);

        nvrhi::BufferHandle bristle_cb;
        Ruzino::brush_upload_cb(
            rc, device, &bc, sizeof(bc), "wb_bristle_cb", bristle_cb);

        // Step 1: Bristle spring dynamics. Pass the grid velocity field so
        // bristles feel the grid-liquid drag (paper §4.1 Eq.2: a_i includes
        // "drag force due to the grid-based liquid flow").
        Ruzino::brush_dispatch(
            rc,
            field->bristle_sim_program,
            { { "grid_vel_x", field->vel_x },
              { "grid_vel_y", field->vel_y },
              { "grid_vel_z", field->vel_z } },
            { { "bristle_data", field->bristle_data } },
            bristle_cb,
            Nb);

        // Step 2: Density constraint (PBF), 3 iterations, each mode 0
        // then 1.
        int total_verts = Nb * M;
        for (int dc_iter = 0; dc_iter < 3; dc_iter++) {
            for (int mode : { 0, 1 }) {
                Ruzino::ConstraintModeCB mcb = { mode, { 0, 0, 0 } };
                nvrhi::BufferHandle mode_cb;
                Ruzino::brush_upload_cb(
                    rc, device, &mcb, sizeof(mcb), "wb_dc_mode_cb", mode_cb);
                ProgramVars v(rc, field->bristle_density_constraint_program);
                v["cb"] = bristle_cb.Get();
                v["bristle_data"] = field->bristle_data.Get();
                v["lambda_buf"] = field->lambda_buf.Get();
                v["mode_cb"] = mode_cb.Get();
                v.finish_setting_vars();
                ComputeContext c(rc, v);
                c.finish_setting_pso();
                c.begin();
                c.dispatch({}, v, total_verts, 256);
                c.finish();
                rc.destroy(mode_cb);
            }
        }

        // Step 3: Resample bristle chains -> samples (with user paint
        // color)
        Ruzino::brush_dispatch(
            rc,
            field->bristle_resample_program,
            { { "bristle_data", field->bristle_data },
              { "bristle_input_color", field->bristle_input_color_buf } },
            { { "sample_pos", field->sample_pos },
              { "sample_vel", field->sample_vel },
              { "sample_color", field->sample_color },
              { "sample_frame", field->sample_frame } },
            bristle_cb,
            Nb);

        // Step 4: Clear bristle accumulation grids
        auto clear_bristle_grid = [&](auto& buf) {
            Ruzino::brush_dispatch(
                rc,
                field->field_clear_program,
                {},
                { { "field", buf } },
                nullptr,
                win_n3d);
        };
        clear_bristle_grid(field->bristle_density);
        clear_bristle_grid(field->bristle_vel_x);
        clear_bristle_grid(field->bristle_vel_y);
        clear_bristle_grid(field->bristle_vel_z);
        clear_bristle_grid(field->bristle_color_r);
        clear_bristle_grid(field->bristle_color_y);
        clear_bristle_grid(field->bristle_color_b);

        // Step 5: Rasterize samples -> accumulation grids. SKIPPED on
        // pen-up relax steps (deposit=false): a hovering brush must not
        // stamp ψ or velocity BCs — the Step-4 clear then leaves "no brush
        // in the fluid", which is exactly what the projection should see.
        // Paper §4.2: dried cells are solid in PRESSURE PROJECTION (fluid
        // divergence/Jacobi/gradient), not here. The rasterize shader
        // splats each sample at its actual position; the fluid solve
        // deflects new paint around/above dried cells. (Earlier "deposit
        // climbs above solid" SRVs removed — that hack broke strokes at
        // higher grid res.)
        if (deposit) {
            Ruzino::brush_dispatch(
                rc,
                field->bristle_raster_program,
                { { "sample_pos", field->sample_pos },
                  { "sample_color", field->sample_color },
                  { "sample_vel", field->sample_vel } },
                { { "bristle_density", field->bristle_density },
                  { "bristle_vel_x", field->bristle_vel_x },
                  { "bristle_vel_y", field->bristle_vel_y },
                  { "bristle_vel_z", field->bristle_vel_z },
                  { "bristle_color_r", field->bristle_color_r },
                  { "bristle_color_y", field->bristle_color_y },
                  { "bristle_color_b", field->bristle_color_b } },
                bristle_cb,
                Nb * S);
        }

        // Step 6 (bristle -> main-grid merge) is intentionally OMITTED.
        //
        // Paper §5 (line 218): "brush bristles are not in direct contact
        // with grid-based liquid." The bristle density/color/velocity
        // rasterized above are NOT paint mass to inject into the grid — per
        // paper §4.1 (line 118) and §4.2 (line 124) they serve only as (a)
        // the sample capacity field ψ for §5.1 Eq.12 (read by
        // bristle_liquid_transfer as bristle_psi), and (b) boundary
        // conditions for pressure projection (§4.2, applied in
        // fluid_divergence/jacobi/gradient).
        //
        // Paint enters the grid by exactly one path: bristle sample liquid
        // overload (m_j > (1+ε)M_j, §5.1) → emit particles → particle
        // rasterize → bristle_merge (the fluid-node dispatch that merges
        // ptcl_* into the main grid, a paper-faithful §5.2 transfer).
        //
        // The previous Step 6 dispatched bristle_merge here to add
        // bristle_density/color/vel/wetness/oil straight into the main grid
        // every sub-step. That was a non-conservative direct injection
        // (density[gidx] += bd, mass created each frame) and the root cause
        // of unbounded paint growth ("white bloat"). It also duplicated the
        // particle path, so paint was injected twice. bristle_merge.slang
        // itself is retained — the fluid node still uses it for the
        // particle rasterize → grid transfer.

        rc.destroy(bristle_cb);
    };

    // ======================================================================
    // SUB-STEP LOOP (brush_paint_sim ~1383-1478). Subdivide the frame
    // displacement into <= one brush-diameter steps; deposit at each.
    // ======================================================================
    {
        int n_sub = 1;
        if (field->has_prev_brush_pos) {
            glm::vec3 delta = brush_pos_3d - field->prev_brush_pos;
            float frame_disp = glm::length(delta);
            float diam = std::max(brush_radius * 2.0f, cell_sz);
            n_sub = std::max(1, static_cast<int>(std::ceil(frame_disp / diam)));
            const int N_SUB_CAP = 128;
            if (n_sub > N_SUB_CAP)
                n_sub = N_SUB_CAP;
        }

        if (bp.active) {
            for (int s = 0; s < n_sub; s++) {
                float t =
                    (static_cast<float>(s) + 0.5f) / static_cast<float>(n_sub);
                glm::vec3 sub_pos =
                    field->has_prev_brush_pos
                        ? glm::mix(field->prev_brush_pos, brush_pos_3d, t)
                        : brush_pos_3d;
                // Vel/omega are instantaneous rates (NOT divided by n_sub);
                // only the integration time dt_sub shrinks per sub-step.
                glm::vec3 sub_vel = brush_vel_3d;
                glm::vec3 sub_accel = brush_accel_3d;
                float sub_rot = brush_rotation;
                glm::vec3 sub_omega = brush_angular_vel;
                glm::vec3 sub_omega_dot = brush_angular_accel;
                float dt_sub = dt / static_cast<float>(n_sub);

                position_window(sub_pos.x, sub_pos.y);
                deposit_at(
                    sub_pos,
                    sub_vel,
                    sub_accel,
                    sub_rot,
                    sub_omega,
                    sub_omega_dot,
                    dt_sub,
                    /*deposit=*/true);
            }
        }
        else {
            // Pen UP — relax step. Paper §4.1 simulates the bristle
            // dynamics independent of canvas contact, so the chains keep
            // following the hovering pen (descend/lift phases included).
            // ONE step at the frame dt, no substeps (nothing is being
            // deposited), and deposit=false: no ψ/BC raster (the Step-4
            // clear zeroes the bristle fields — the fluid sees no brush
            // while hovering).
            position_window(brush_pos_3d.x, brush_pos_3d.y);
            deposit_at(
                brush_pos_3d,
                brush_vel_3d,
                brush_accel_3d,
                brush_rotation,
                brush_angular_vel,
                brush_angular_accel,
                dt,
                /*deposit=*/false);
        }

        // Record this frame's brush center + velocity for the next frame.
        field->prev_brush_pos = brush_pos_3d;
        field->has_prev_brush_pos = true;
        field->prev_brush_vel = brush_vel_3d;
        field->prev_angular_vel = brush_angular_vel;
    }

    // ======================================================================
    // PHASE 2 — §5.1 bristle <-> particle liquid exchange (ex-node
    // brush_wb_bristle): ABSORB (grid paint -> sample, Eq.12/13 capacity)
    // then EMIT (over-capacity sample -> FLIP particles).
    //
    // The ex-node's guards (!field / !is_simulating /
    // !particles_initialized) are invariants here: phase 1 allocated the
    // field and initialized the particle buffers, and the !is_simulating
    // exit above has already returned. All locals (field, rc, device,
    // WIN_*, Nb, S, brush_radius) carry over from phase 1.
    // ======================================================================
    const int max_ptcl = WetbrushSimState::MAX_PARTICLES;

    // ======================================================================
    // §5.1 Bristle-particle liquid transfer (brush_paint_sim ~1531-1612).
    // ABSORB (paint supply -> sample using Eq.12/13 capacity) then EMIT
    // (sample -> particles). Ping-pong on sample_liquid.
    //
    // PEN-UP GATING: a pen-up brush is not painting — skip both passes. The
    // ungated version kept absorbing the dip supply and emitting at the
    // (stalled) sample positions while the fluid node skipped particle
    // maintenance on pen-up frames, so the pool filled with particles that
    // could never deposit (+emit_budget/frame, wrapping MAX_PARTICLES on a
    // long pen-up tail).
    // ======================================================================
    if (field->pen_down) {
        Ruzino::BristleLiquidConstants blc = {};
        blc.num_bristles = Nb;
        blc.samples_per_bristle = S;
        blc.mu = 0.5f;
        // M_max bounds the emission radius R_j = cbrt(3*M_max/(4π*ρ₀)). R_j
        // must stay well inside D1 so newborns ride the adhesion bulb; with
        // ρ₀=6.0, M_max=0.15 → R_j≈0.18 cm ≈ 0.36×brush_radius (paper ratio
        // ~0.35). M_max and ρ₀ move TOGETHER (M_max = dip size — how long a
        // stroke one dip paints; ρ₀ keeps R_j constant): the pair (0.03, 2e4)
        // drained a dip in ~15 frames at the old 1u=27.5cm scale; (0.15, 1e5)
        // carried 5× the paint at the same R_j. In the current 1u=1cm scale the
        // pair is (0.15, 6.0) — same R_j ≈ 0.18 cm. Larger M_max without
        // raising ρ₀ scatters emitted particles past the brush (wide diffuse
        // blob instead of a stroke). Also the dip load (m_j = M_max at
        // stroke_start) — shared constant.
        blc.M_max = WetbrushSimState::WB_M_MAX;
        blc.M_min = 0.005f;
        blc.rho_0 = 12.3f;
        blc.eps_emit = 0.1f;
        // §5.1 emission smoothing: "To further smoothen the liquid transfer
        // process, we also set a limit on the maximum number of particles that
        // can be absorbed or emitted by a sample per time step." 10/step let a
        // saturated dip dump HALF the ink charge in the first ~5 frames at the
        // touchdown point (174K particles / 5762 mass airborne at frame 5), and
        // the stroke then ran dry. 1/step paces the same drawable mass
        // (M_max − (1+ε)M_j ≈ 0.1/sample) over ~50 frames — a full stroke —
        // at paper particle density (§7: 210K–2M).
        blc.max_emit_per_step = 1;
        blc.grid_res = field->grid_res;
        blc.grid_res_z = WIN_Z;
        blc.height_extent = field->grid_height;
        blc.grid_center_z = field->grid_center_z;
        blc.cell_size = cell_sz;
        blc.paper_size = field->grid_paper;
        blc.grid_center_x = field->grid_center.x;
        blc.grid_center_y = field->grid_center.y;
        blc.D0 = brush_radius * 3.0f;
        blc.max_particles = max_ptcl;
        // Global per-step particle-creation cap. PAPER-FAITHFUL DEFAULT = OFF
        // (0): the paper bounds emission only per SAMPLE ("a limit on the
        // maximum number of particles that can be absorbed or emitted by a
        // sample per time step") and its typical run holds 210K–2M particles.
        // With the dip model (mass = the finite stroke_start saturation), the
        // total emitted mass is bounded by the dip itself — no per-frame
        // minting to throttle. The env var stays as a pool-overflow escape
        // hatch for extreme test configurations.
        static const int emit_budget = [] {
            const char* env = std::getenv("WB_EMIT_BUDGET");
            return env ? std::max(std::atoi(env), 0) : 0;
        }();
        blc.emit_budget = emit_budget;
        // Per-frame entropy for the EMIT budget's probabilistic pre-gate (see
        // bristle_liquid_transfer.slang) — the moving brush position
        // decorrelates the gate draw across frames. The deposit node (which
        // runs before this one in the zone chain) stores THIS frame's
        // grid-local position in prev_brush_pos at its end, so it is current
        // here.
        blc.brush_pos_x = field->prev_brush_pos.x;
        blc.brush_pos_y = field->prev_brush_pos.y;
        blc.brush_pos_z = field->prev_brush_pos.z;
        blc.window_origin_x = field->win_origin_x;
        blc.window_origin_y = field->win_origin_y;
        blc.window_origin_z = 0;
        blc.window_size_x = WIN_XY;
        blc.window_size_z = WIN_Z;
        // Dip request from the deposit node (stroke_start): the ABSORB pass
        // initializes m_j = M'_j(ψ) in-shader this frame.
        blc.dip_frame = field->dip_frame ? 1 : 0;
        field->dip_frame = false;

        nvrhi::BufferHandle liquid_cb;
        Ruzino::brush_upload_cb(
            rc, device, &blc, sizeof(blc), "wb_liquid_cb", liquid_cb);

        // Lazily compile the liquid shaders (they live in the field).
        if (!field->bri_liquid_transfer_program)
            field->bri_liquid_transfer_program =
                Ruzino::brush_compile_shader(rc, "bristle_liquid_transfer.slang");
        if (!field->bri_liquid_emit_program)
            field->bri_liquid_emit_program =
                Ruzino::brush_compile_shader(rc, "bristle_liquid_emit.slang");

        // Pass 0: ABSORB (color bleeding; sample_liquid SRV -> sample_liquid_b
        // UAV)
        Ruzino::brush_dispatch(
            rc,
            field->bri_liquid_transfer_program,
            { { "sample_pos", field->sample_pos },
              { "sample_color", field->sample_color },
              { "sample_liquid_in", field->sample_liquid },
              { "bristle_psi", field->bristle_density },
              { "grid_density", field->density },
              { "grid_color_r", field->color_r },
              { "grid_color_y", field->color_y },
              { "grid_color_b", field->color_b } },
            { { "sample_liquid_out", field->sample_liquid_b } },
            liquid_cb,
            Nb * S);
        std::swap(field->sample_liquid, field->sample_liquid_b);

        // Pass 1: EMIT (sample -> particles, hemisphere pattern).
        //
        // Do NOT reset the counter here. Paper §4.3: "Our system needs 200K to
        // 1M particles" that survive across frames. The counter is owned by the
        // fluid node's compact step, which resets it to 0 and rewrites only the
        // alive particles into [0, count). EMIT appends past that count via
        // InterlockedAdd. The previous reset here zeroed the counter every
        // frame, destroying the survivors the compact step had preserved and
        // breaking particle persistence — new particles overwrote slot 0+ and
        // the §5.1-emitted paint never accumulated.
        //
        // The EMIT budget counter IS reset here: it bounds births per step, so
        // it must start from zero every frame.
        Ruzino::brush_reset_counter(rc, device, field->emit_budget);
        Ruzino::brush_dispatch(
            rc,
            field->bri_liquid_emit_program,
            { { "sample_pos", field->sample_pos },
              { "sample_color", field->sample_color },
              { "sample_vel", field->sample_vel },
              { "sample_liquid_in", field->sample_liquid },
              { "bristle_psi", field->bristle_density },
              { "grid_density", field->density },
              { "grid_color_r", field->color_r },
              { "grid_color_y", field->color_y },
              { "grid_color_b", field->color_b } },
            { { "sample_liquid_out", field->sample_liquid_b },
              { "ptcl_counter", field->ptcl_counter },
              { "emit_budget", field->emit_budget },
              { "ptcl_pos_out", field->ptcl_pos },
              { "ptcl_vel_out", field->ptcl_vel },
              { "ptcl_color_out", field->ptcl_color },
              { "ptcl_alive_out", field->ptcl_alive } },
            liquid_cb,
            Nb * S);
        std::swap(field->sample_liquid, field->sample_liquid_b);

        rc.destroy(liquid_cb);
    }

    // ======================================================================
    // PHASE 3 — particle cycle + grid fluid solve + §5.2 transfers (ex-node
    // brush_wb_fluid): particle update + swarm raster + momentum merge,
    // substepped grid solve, Eq.16 deposit / Eq.15 drain, pool compaction.
    //
    // Pen-up frames still run this phase (damp/dry + particle maintenance)
    // exactly like the monolith's pen-up handling — paint keeps settling.
    // The ex-node's guards (!field / !is_simulating) are invariants here;
    // the fluid-solve sockets below are the only inputs phase 1 didn't
    // read.
    // ======================================================================
    float viscosity = params.get_input<float>("Viscosity");
    float diffusion = params.get_input<float>("Diffusion Rate");
    float drying_rate = params.get_input<float>("Drying Rate");

    const int window_total = win_n3d;
    // Full-grid cell count for global drying (§4.2: "increase the dryness
    // of EVERY grid cell"). Drying must be global so paint that the active
    // window has moved past still dries — otherwise previously painted
    // strokes never harden and can't act as solid cells that deflect later
    // strokes.
    const int global_n3d = field->grid_res * field->grid_res * WIN_Z;

    // [wb-xfer] stage probes (WB_LEDGER_PROBE=1): full-global density sum
    // at four pipeline stations, to localize which stage destroys mass.
    // Full 268MB readbacks — tail frames only (plus one mid-stroke
    // reference).
    static int xfer_frame = 0;
    const bool ledger_probe = [] {
        const char* e = std::getenv("WB_LEDGER_PROBE");
        return e && e[0] == '1';
    }();
    const int xf = xfer_frame++;
    auto probe_grid_sum = [&](const char* tag) {
        if (!ledger_probe)
            return;
        if (!(xf >= 108 || xf == 30))
            return;
        std::vector<float> data(global_n3d);
        auto rb = rc.create(
            nvrhi::BufferDesc{}
                .setByteSize(static_cast<size_t>(global_n3d) * sizeof(float))
                .setCpuAccess(nvrhi::CpuAccessMode::Read)
                .setDebugName("wb_xfer_rb"));
        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        cmd->copyBuffer(
            rb,
            0,
            field->density,
            0,
            static_cast<size_t>(global_n3d) * sizeof(float));
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        void* mapped = device->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
        memcpy(
            data.data(),
            mapped,
            static_cast<size_t>(global_n3d) * sizeof(float));
        device->unmapBuffer(rb);
        rc.destroy(rb);
        rc.destroy(cmd);
        double sum = 0.0;
        int negs = 0;
        for (int i = 0; i < global_n3d; ++i) {
            sum += data[i];
            if (data[i] < -1e-4f)
                ++negs;
        }
        spdlog::info("[wb-xfer] f={} {} sum={:.1f} neg={}", xf, tag, sum, negs);
    };
    probe_grid_sum("A_in");

    // Brush pose (grid-local) — brush_pos_3d carries over from phase 1
    // (same bp.pos minus grid_center). The ex-fluid node never computed
    // pose derivatives and fed ZERO accel / omega_dot to the particle CBs;
    // keep that validated behavior (phase 1's real accel would change the
    // particle-update dynamics — its own A/B, not this refactor).
    brush_accel_3d = glm::vec3(0.0f);
    brush_angular_accel = glm::vec3(0.0f);

    // Grid↔particle conversion + adhesion ranges (paper §5.2 Table 1).
    // WORLD SCALE 1 unit = 1 cm (doc §33), so the paper's SI values apply
    // VERBATIM: D0 = 1 cm, D1 = 0.3 cm — and our brush (radius 0.5 cm ≈ the
    // paper's ~0.55 cm brush) makes the paper's own absolute numbers the
    // right calibration, replacing the earlier brush-relative inference (D0
    // = 1.8 R, D1 = 0.55 R). Note the ride time grows (~D0 shell width /
    // relative speed): the paper's head-to-trail mass ratio converges over
    // LONG strokes, which a short test cannot show.
    // R_j consistency: the §5.1 emission radius R_j = cbrt(3·M_max/(4π·ρ₀))
    // with M_max = WB_M_MAX and ρ₀ = 6.0 (below, phase 2) gives
    // R_j ≈ 0.18 cm = 0.36 R < D1 = 0.3 cm, so newborn particles still ride
    // the brush through Eq.10's adhesion blend max(1 − d_B/D1, 0) ≈ 0.4–1
    // inside the emission shell.
    const float D0 = 1.0f;
    const float D1 = 0.3f;
    // §5.2 "moves slowly": the paper gives no number. Real units now — a
    // trail's liquid behind the brush settles below ~2 cm/s.
    const float slow_deposit_speed = 2.0f;

    // Gravity (world units/s²): 1 unit = 1 cm → g = 981 cm/s² = 981 u/s².
    // Applied to particles (§4.3 a_k "including the gravity and the
    // friction") and to fluid cells (§4.2 standard Eulerian external force
    // — the grid liquid previously had NO force and hovered where
    // deposited). Direction-adjustable for experiments via
    // WB_GRAVITY_X/Y/Z.
    const glm::vec3 gravity = [] {
        auto envf = [](const char* k, float d) {
            const char* v = std::getenv(k);
            return v ? std::atof(v) : d;
        };
        return glm::vec3(
            envf("WB_GRAVITY_X", 0.0f),
            envf("WB_GRAVITY_Y", 0.0f),
            envf("WB_GRAVITY_Z", -981.0f));
    }();

    // Lazily compile the fluid + particle shaders (ensure_prog from the
    // phase-1 preamble).
    ensure_prog(field->advect_program, "fluid_advect.slang");
    // Scalar advection scheme. PAPER-FAITHFUL DEFAULT = semi-Lagrangian
    // (§4.2: "we advect all of the fields using the semi-Lagrangian method")
    // — the same shader as velocity. The conservative upwind flux-form
    // variant (f4ba3b43 mass-ledger fix, validated at −0.03% over a
    // 135-frame stroke) stays selectable via WB_SCALAR_ADVECT=upwind:
    // semi-Lagrangian interpolation of the EXTENSIVE canvas fields is not
    // mass-conserving, so the escape hatch exists for ledger hunts and
    // extreme configurations.
    static const char* scalar_advect_shader = [] {
        const char* env = std::getenv("WB_SCALAR_ADVECT");
        return env && std::strcmp(env, "upwind") == 0
                   ? "fluid_advect_upwind.slang"
                   : "fluid_advect.slang";
    }();
    ensure_prog(field->advect_scalar_program, scalar_advect_shader);
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
    // Only when there is active deposit this frame. The monolith gates this
    // on new_count > 0; in streaming each active frame is "new".
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
        // pigment c_j and the excess mass. That is the paper's only paint
        // -> particle path (§5.1).
        //
        // The previous emit-mode-0 / emit-mode-1 dispatches here
        // (particle_emit.slang) are DISABLED. They were decoupled from the
        // §5.1 capacity model: mode 0 fired once per bristle sample every
        // frame based on the global ink_amount (not m_j/M_j overload), and
        // mode 1 minted zero-pigment particles from grid density. Together
        // they flooded the pool and bypassed the sample-liquid
        // conservation, so paint mass grew without bound.
        // particle_emit.slang is kept on disk (not dispatched) for
        // reference.

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
        // Swarm→grid momentum coupling scale (bristle_merge relaxation
        // gate; see the quadratic-momentum note there). WB_VEL_INJECT tunes
        // it.
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
    // FLUID SOLVE (brush_paint_sim ~1971-2468). One or more substeps based
    // on the frame dt (capped at 16). Each substep: velocity diffuse ->
    // project
    // -> advect velocity -> re-project -> advect scalars -> diffuse scalars
    // -> damp/dry -> FLIP velocity update.
    // ======================================================================
    float sim_dt = std::min(dt, 0.05f);  // dt from the top-of-frame preamble
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
        // Jacobi at a = dt·visc·N² ≈ 500 homogenizes the window velocity
        // each substep, diluting localized momentum to ~the window mean,
        // and (b) the dryness threshold zeroing velocity, plus the bounded
        // §4.3 particle-velocity merge. This global multiplier was a
        // temporary stand-in from the era when the merge accumulated
        // momentum quadratically (see bristle_merge.slang); it stays
        // env-tunable
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
        // convex stages (diffuse = neighbor average, semi-Lagrangian advect
        // = convex sample), so the first stage whose max jumps is the
        // injector. Reads back the WINDOW-SIZED vel buffers + div/pressure
        // after each project. Substep 0 only. NOT a physics knob.
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
        // Same readback but also reports the argmax window-local
        // coordinates (lx, ly, lz) of the |max| cell — needed to tell WHICH
        // face of WHICH cell a projection-stage spike lives at.
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
            // bristle-occupied cells act as no-flux walls. The gate is
            // small so only cells genuinely under bristles block the flow;
            // empty cells and thin paint do not. bristle_density is the
            // §4.1 rasterized field (window-sized), bound into
            // divergence/jacobi/ gradient. See those shaders' is_brush_g.
            //
            // Diagnostic bisect switches for the blob-test f12 eruption
            // hunt (NOT physics knobs): WB_NO_PROJECT=1 skips both pressure
            // projections; WB_NO_BRUSH_WALL=1 disables the §4.2 bristle
            // no-flux/moving-wall BC (gate huge → is_brush_g always false).
            // The gate MUST be decided BEFORE cb_buf is uploaded below —
            // the divergence/gradient dispatches inside project() reuse
            // cb_buf, so patching fluid_cb afterwards silently did nothing
            // and the WB_NO_BRUSH_WALL bisect was invalid.
            static const bool no_project = [] {
                const char* env = std::getenv("WB_NO_PROJECT");
                return env && std::atoi(env) == 1;
            }();
            static const float wall_gate = [] {
                const char* env = std::getenv("WB_NO_BRUSH_WALL");
                if (env && std::atoi(env) == 1)
                    return 1e9f;
                // Tunable via WB_WALL_GATE (default 0.01: only cells
                // genuinely under bristles count as brush interior). A
                // 0.001 gate ("any splat") was tested for the blob lift
                // trail and made no difference — the trail cells carry no
                // bristle splat at all (sample mass is depleted by lift
                // time).
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
            // copy the CORNER block at offset 0 — wrong once the window
            // moves off the corner. The previous code did exactly that, so
            // FLIP read stale corner velocities at particle positions
            // (vel_old was only ever valid at the grid corner).
            // field_copy_window maps each window cell to its global index
            // and copies that exact cell.
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
            // Stam implicit-diffusion number a = dt·ν/h² with h the CELL
            // SIZE in world units (h = paper_size/res). The transcription's
            // dt·ν·N² silently assumed a unit-size domain (h = 1/N) and
            // held only while paper_size was 1. The 1u=1cm rescale
            // (paper_size=10) overstated ν by paper² = 100×: a ≈ 5.5e3 made
            // each sweep an almost-exact window mean, dragging the wake at
            // brush speed — deposits flickered (Eq.15 drained the trail
            // into particles that inherited the homogenized velocity and
            // never satisfied the slow-deposit gate) and the per-frame
            // window scroll left sawtooth scales. Viscosity is now a
            // kinematic viscosity in cm²/s.
            fluid_cb.jacobi_alpha = sub_dt * viscosity / (cell_sz * cell_sz);
            {
                nvrhi::BufferHandle jcb;
                Ruzino::brush_upload_cb(
                    rc,
                    device,
                    &fluid_cb,
                    sizeof(fluid_cb),
                    "wb_jacobi_cb",
                    jcb);
                // NOTE: do NOT use &field->vel_x here. RefCountPtr
                // overloads operator&() to return IBuffer**
                // (resource.h:307), so the unary-& yields a pointer to the
                // ptr_ MEMBER, not to the RefCountPtr object — a downstream
                // std::swap then swaps raw IBuffer* values, bypassing
                // refcount accounting and corrupting the handles. This was
                // the cause of field->vel_x becoming NULL mid-solve (crash
                // in requireBufferState). std::addressof bypasses the
                // overloaded operator& and returns the true BufferHandle*,
                // so *addr is a correct BufferHandle& alias that swaps
                // through the RefCountPtr move operators.
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

            // Advect scalars (density, color, wetness, oil_density). These
            // are GLOBAL canvas fields: read globally (flag 0, the main
            // cb_buf), advected into the WINDOW-SIZED tmp, then copied back
            // over the window region (field_copy_window mode 2). No buffer
            // swap — the tmp and the field have different sizes.
            fluid_cb.copy_mode = 2;
            // field_in is the GLOBAL canvas field: the semi-Lagrangian
            // shader reads it globally only under flag 0, and the velocity
            // section above left advect_field_window_local at 1
            // (window-local). Reset it here. The upwind shader (the
            // WB_SCALAR_ADVECT=upwind escape hatch) ignores the flag — it
            // reads field_in globally by construction.
            fluid_cb.advect_field_window_local = 0;
            nvrhi::BufferHandle advect_scalar_cb;
            Ruzino::brush_upload_cb(
                rc,
                device,
                &fluid_cb,
                sizeof(fluid_cb),
                "wb_adv_scalar_cb",
                advect_scalar_cb);
            // Paper §4.2: "we advect all of the fields using the
            // semi-Lagrangian method" — the scalars ride the same scheme as
            // the velocity (shader chosen at the ensure_prog above; default
            // semi-Lagrangian, WB_SCALAR_ADVECT=upwind restores the
            // conservative flux-form). Window-tmp out + window→global copy
            // back, same plumbing as the velocity family. Known trade-off,
            // kept deliberately: value-based semi-Lagrangian interpolation
            // does not conserve an extensive field — see the ledger notes in
            // fluid_advect_upwind.slang's header.
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
            // time. Removing it matches the paper and eliminates the
            // shrink.

            // Damp + dry. Dispatched over the FULL grid (not just the
            // active window) so that paint left behind by a moving brush
            // still dries (§4.2: "increase the dryness of every grid
            // cell"). Velocity damp on empty cells is a no-op (their
            // velocity is already zero). Damp + dry, split along the buffer
            // residency (see fluid_damp_dry.slang): a GLOBAL wetness-only
            // pass so paint left behind by a moving brush still dries
            // (§4.2: "increase the dryness of every grid cell"), and a
            // WINDOW velocity pass (the velocity family only exists inside
            // the window).
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
        // D1 (adhesion range, Eq.10) — grid_to_particle drains within D0
        // per paper §5.2; D1 is carried for reference but unused by the
        // drain.
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
        // num_bristles / samples_per_bristle: REQUIRED by
        // particle_to_grid's d_{B,k} nearest-sample query (§5.2). Without
        // these, num_samples = 0, the scan finds no bristle, d_B defaults
        // to sqrt(1e30)≈3e15, and EVERY particle reads as "far from
        // bristles" → instant deposit, regardless of D0. This was the
        // hidden reason widening D0/D1 had no effect: the d_B was always
        // astronomically larger than any D0.
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
        // Binds sample_pos so the shader can compute d_{B,k} (distance to
        // the nearest bristle sample) instead of the brush-center distance.
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
        // have to emit any particle." Each emitted particle subtracts its
        // mass (weighted by W) from its 3×3×3 neighborhood, NOT just its
        // emitting cell. The *_out tmps are WINDOW-SIZED: seed them from
        // the global canvas fields over the window (field_copy_window mode
        // 1), let the shader subtract, then copy the results back (mode 2).
        // No buffer swap — the tmp and the field have different sizes.
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
              { "vel_z", field->vel_z },
              // §4.2 dryness gate: the drain must not resurrect dried-solid
              // paint (doc §34 trail-flicker fix).
              { "wetness", field->wetness } },
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
        // only writes packed survivors' flags, so slots above the live
        // count must be cleared by us or they keep the ping-pong buffer's
        // stale alive=1 flags (deposited particles resurrected at their
        // pre-deposit positions every frame, re-depositing their mass — see
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

NODE_DECLARATION_UI(brush_wb_sim);

NODE_DECLARATION_ALWAYS_DIRTY(brush_wb_sim);

NODE_DEF_CLOSE_SCOPE
