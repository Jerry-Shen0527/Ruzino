// node_brush_wb_deposit — Wetbrush DEPOSIT sub-step (streaming zone chain).
//
// Topology (the simulation zone now carries ONE typed boundary slot:
// WetbrushZoneState = shared_ptr<WetbrushSimState>, i.e. just the paint field.
// The per-frame BrushPoint and the input stroke Geometry stay OFF the
// boundary):
//
//   mock_stroke --Stroke Curves--> [ simulation_in ]   (static input slot)
//   [ simulation_in ] --WetbrushZoneState--> brush_wb_deposit   (fed-back
//   field) mock_point_emitter --BrushPoint--> brush_wb_deposit   (interior,
//   fresh/帧) brush_wb_deposit --WetbrushZoneState--> brush_wb_bristle --...-->
//   commit brush_wb_commit --WetbrushZoneState--> [ simulation_out ]   (fed
//   back)
//
// This node owns the FIRST physics stage, lifted 1:1 from brush_paint_sim:
//   1. Lazily allocate the FULL WetbrushSimState buffer set (3D window fields
//      + 2D canvas + bristle + particle) — the same buffers the monolith keeps
//      in Node::storage, here carried in the shared field. Allocation mirrors
//      brush_paint_sim lines ~463-818.
//   2. Lazily compile every Wetbrush shader (they persist in the field too).
//   3. Derive the brush pose (pos / vel / accel / omega / omega_dot) from THIS
//      BrushPoint + the field's prev_* via frame finite-differencing
//      (brush_paint_sim ~938-998). On stroke_start, reset prev_*.
//   4. Sub-step loop (brush_paint_sim ~1383-1478): subdivide the frame
//      displacement into <= one brush-diameter steps, and at each sub-step:
//        - position_window(sub_pos): commit+clear the OLD window if it moved
//          (brush_paint_sim ~1017-1106);
//        - deposit_at(...): bristle_simulate -> density_constraint ->
//          resample -> rasterize -> merge into the live window
//          (brush_paint_sim ~1115-1373).
//   5. Stamp this frame's brush kinematics into prev_* for the next frame.
//
// The field (with the deposited paint) is forwarded on the "State" output for
// bristle's liquid-transfer stage. This node is NOT responsible for the fluid
// solve, particles, or canvas readback — those live in the downstream nodes.

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "GCore/GOP.h"
#include "GCore/geom_payload.hpp"
#include "GPUContext/compute_context.hpp"  // CommandListDesc
#include "RHI/ResourceManager/resource_allocator.hpp"
#include "brush_sim_common.hpp"  // BrushPoint, WetbrushSimState, WetbrushZoneState, brush_* helpers
#include "geom_node_base.h"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(brush_wb_deposit)
{
    // Per-frame brush sample from the emitter (interior edge, fresh each
    // frame).
    b.add_input<Ruzino::BrushPoint>("Brush Point");
    // The fed-back paint field. On the init frame there is NO feedback yet
    // (simulation_out has not run), so this slot is empty then -- optional so
    // the zone doesn't skip simulation_in for a missing required input. The
    // exec allocates a fresh field when it's absent.
    b.add_input<Ruzino::WetbrushZoneState>("State").optional(true);
    // Grid / canvas domain params.
    b.add_input<int>("Resolution").default_val(512).min(64).max(4096);
    b.add_input<int>("Resolution Z").default_val(32).min(4).max(128);
    b.add_input<float>("Paper Size").default_val(1.0f).min(0.1f).max(10.0f);
    b.add_input<float>("Canvas Center X").default_val(0.0f);
    b.add_input<float>("Canvas Center Y").default_val(0.0f);
    b.add_input<float>("Canvas Z").default_val(0.0f);
    b.add_input<float>("Canvas Height").default_val(0.0f).min(0.0f).max(2.0f);
    b.add_input<float>("Brush Radius").default_val(0.02f).min(0.001f).max(0.5f);
    b.add_input<float>("Brush Pressure").default_val(1.0f).min(0.0f).max(4.0f);
    b.add_input<float>("Ink Amount").default_val(0.8f).min(0.0f).max(2.0f);
    b.add_input<float>("Oil Density").default_val(0.5f).min(0.0f).max(1.0f);
    // RYB ink color. Optional because vec3 sockets can't carry a default_val
    // through serialization; the exec falls back to red when unwired.
    b.add_input<glm::vec3>("Ink Color").optional(true);

    // Outgoing paint field (allocated/updated) for the rest of the chain +
    // next-frame feedback.
    b.add_output<Ruzino::WetbrushZoneState>("State");
    // Forward the per-frame BrushPoint so downstream nodes (fluid) know pen
    // up/down without re-reading the emitter.
    b.add_output<Ruzino::BrushPoint>("Brush Point");
}

NODE_EXECUTION_FUNCTION(brush_wb_deposit)
{
    using Ruzino::WetbrushSimState;
    using Ruzino::WetbrushZoneState;

    Ruzino::BrushPoint bp = params.get_input<Ruzino::BrushPoint>("Brush Point");
    // "State" is optional: absent on the init frame (no feedback yet). Default
    // to a fresh WetbrushZoneState with a null field; need_alloc below handles
    // it. Never call get_input on an unwired optional — the executor sets its
    // input pointer to nullptr and get_input would deref it.
    WetbrushZoneState zs;
    if (params.has_input("State"))
        zs = params.get_input<Ruzino::WetbrushZoneState>("State");
    auto& field = zs.state;  // shared_ptr<WetbrushSimState>
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
    // Ink color: prefer the BrushPoint's trajectory color (enables multi-color
    // strokes where each point carries its own RYB); fall back to the static
    // "Ink Color" socket when the emitter didn't supply one (single-color).
    glm::vec3 ink_color = params.has_input("Ink Color")
                              ? params.get_input<glm::vec3>("Ink Color")
                              : glm::vec3(1.0f, 0.0f, 0.0f);
    if (bp.active) {
        ink_color = bp.color;
    }

    auto& rc = get_resource_allocator();
    auto device = RHI::get_device();
    auto payload = params.get_global_payload<GeomPayload>();
    float dt = payload.delta_time > 0.0f ? payload.delta_time : (1.0f / 60.0f);

    // ======================================================================
    // LAZY FIELD ALLOCATION (mirrors brush_paint_sim ~463-818)
    // Done once, when the grid dimensions are first known / change. Allocates
    // the FULL buffer set so velocity / bristle / particle persist across
    // frames via the zone feedback.
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

        // Global grid size: all 3D buffers are now global (res × res × res_z),
        // not window-sized. Paper §4.2: a large 3D grid is the persistent
        // paint store; the window is only a dispatch range.
        int alloc_win_n3d = resolution * resolution * rz;

        // Bristle/particle accumulation grids (Group B/C) are allocated at the
        // active-window size, not the full global grid. Paper §5/§5.1: bristle
        // samples and particles only exist within the brush-local compute
        // window (WIN_ALLOC_XY × WIN_ALLOC_XY × res_z), so a full-grid alloc
        // wastes (res/WIN)²× the memory for buffers that are cleared and
        // rebuilt each sub-step anyway. At res=4096 this is the difference
        // between 14 buffers × 1B cells (out of memory) and 14 × 1M cells.
        // Shaders index these with window-local coords (global cell minus
        // window_origin); see bristle_rasterize / particle_rasterize /
        // bristle_merge / bristle_liquid_transfer.
        int win_alloc_n3d = WetbrushSimState::WIN_ALLOC_XY *
                            WetbrushSimState::WIN_ALLOC_XY * rz;

        auto safe_destroy = [&](nvrhi::BufferHandle& h) {
            if (h) {
                rc.destroy(h);
                h = nullptr;
            }
        };
        // Helper: destroy a fixed set of buffers by reference. Using a variadic
        // instead of a braced-init-list of addresses, because MSVC's
        // initializer_list deduction chokes on RefCountPtr<IBuffer>* element
        // types (its implicit conversion to IBuffer* makes the list element
        // type ambiguous, surfacing as C2440 "IBuffer** -> BufferHandle*").
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

        field->density = make_buf("wb_density");
        field->density_tmp = make_buf("wb_density_tmp");
        field->color_r = make_buf("wb_color_r");
        field->color_y = make_buf("wb_color_y");
        field->color_b = make_buf("wb_color_b");
        field->color_r_tmp = make_buf("wb_color_r_tmp");
        field->color_y_tmp = make_buf("wb_color_y_tmp");
        field->color_b_tmp = make_buf("wb_color_b_tmp");
        field->vel_x = make_buf("wb_vel_x");
        field->vel_x_tmp = make_buf("wb_vel_x_tmp");
        field->vel_y = make_buf("wb_vel_y");
        field->vel_y_tmp = make_buf("wb_vel_y_tmp");
        field->vel_z = make_buf("wb_vel_z");
        field->vel_z_tmp = make_buf("wb_vel_z_tmp");
        field->wetness = make_buf("wb_wetness");
        field->wetness_tmp = make_buf("wb_wetness_tmp");
        field->oil_density = make_buf("wb_oil_density");
        field->oil_density_tmp = make_buf("wb_oil_density_tmp");
        field->height_field = make_buf("wb_height");
        field->pressure_a = make_buf("wb_pressure_a");
        field->pressure_b = make_buf("wb_pressure_b");
        field->divergence_buf = make_buf("wb_divergence");
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
        field->vel_x_old = make_buf("wb_vel_x_old");
        field->vel_y_old = make_buf("wb_vel_y_old");
        field->vel_z_old = make_buf("wb_vel_z_old");
        // Float4 packed paint field (density,r,g,b) — global grid sized, for
        // the shared GPU buffer registry (zero-copy sim→render). Needs
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
        write_3d(
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
            field->vel_x_old,
            field->vel_y_old,
            field->vel_z_old);
        write_win(
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
            "brush_wb_deposit: allocated global 3D grid {}x{}x{}, "
            "paper={:.3f}, height={:.3f}",
            resolution,
            resolution,
            rz,
            paper_size,
            height);
    }

    const int WIN_XY =
        std::min(WetbrushSimState::WIN_ALLOC_XY, field->grid_res);
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
        // sample_liquid starts DRY (m_j = 0). Paper §5.1: a brush carries
        // paint due to hydrophilicity, but loading happens via ABSORB from the
        // supply reservoir (the "dip"), NOT by pre-saturating every sample at
        // allocation. Pre-saturating to M_max caused a cold-start burst: frame
        // 1 ABSORB pushes every canvas-touching sample past (1+ε)M_j at once
        // (supply is refilled at stroke_start), they all EMIT simultaneously,
        // and the brush's static velocity field can't advect the deposit away
        // → a thick pile at the touchdown point ("落笔处一大坨").
        //
        // Starting dry, ABSORB frame 1 fills each sample up to M_j (saturated,
        // not yet overloaded — m_j ≤ (1+ε)M_j, so EMIT is a no-op). Only from
        // frame 2 do samples begin to overload and emit, by which time the
        // brush has started moving, so the first deposit lands along the
        // stroke rather than piling up at the touchdown point. The pigment c_j
        // is still seeded to the ink color so ABSORB's color_mix has a base.
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
    // the BrushPoint's color can change mid-simulation (multi-color strokes),
    // so the init-block write above (which only runs once) would leave a stale
    // color. This write is cheap (4 floats) and runs only when the pen is down.
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
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        rc.destroy(cmd);

        field->particles_initialized = true;
    }

    // ======================================================================
    // LAZY SHADER COMPILATION (brush_paint_sim ~818-884). Persists in field.
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

    // Init frame or pen-up: nothing to deposit. Forward the (allocated) field.
    if (!payload.is_simulating || !bp.active) {
        params.set_output("State", zs);
        params.set_output("Brush Point", bp);
        return true;
    }

    // ======================================================================
    // BRUSH POSE — frame finite-differencing (brush_paint_sim ~938-998)
    // The monolith differences along the curve's last 2-3 vertices; here we
    // difference THIS BrushPoint against the field's prev_* (one point/frame).
    // ======================================================================
    glm::vec3 brush_pos_3d = bp.pos;
    brush_pos_3d.x -= field->grid_center.x;
    brush_pos_3d.y -= field->grid_center.y;

    glm::vec3 brush_vel_3d(0.0f);
    glm::vec3 brush_accel_3d(0.0f);
    glm::vec3 brush_angular_vel(0.0f);
    glm::vec3 brush_angular_accel(0.0f);
    float brush_rotation = 0.0f;

    if (bp.stroke_start) {
        // Fresh pen-down: no inherited motion.
        field->has_prev_brush_pos = false;
        field->prev_brush_vel = glm::vec3(0.0f);
        field->prev_angular_vel = glm::vec3(0.0f);
    }
    else if (field->has_prev_brush_pos) {
        glm::vec3 new_vel = brush_pos_3d - field->prev_brush_pos;
        if (dt > 1e-6f)
            brush_accel_3d = (new_vel - field->prev_brush_vel) / dt;
        brush_vel_3d = new_vel;
        brush_rotation = atan2(brush_vel_3d.y, brush_vel_3d.x);

        // Angular velocity about canvas normal (Z), wrapped to [-pi, pi].
        if (field->has_prev_brush_pos) {
            // Recover prev heading from prev velocity.
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

    // ======================================================================
    // position_window — center the active window's dispatch range on a brush
    // XY. Paper §4.2: the 3D grid is global and persistent; the window is only
    // the per-frame compute region. Moving the window no longer commits/clears
    // anything — the old cells keep their values in the global grid.
    // ======================================================================
    auto position_window = [&](float bx, float by) {
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
                          float dt_sub) {
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

        // Step 2: Density constraint (PBF), 3 iterations, each mode 0 then 1.
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

        // Step 3: Resample bristle chains -> samples (with user paint color)
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

        // Step 5: Rasterize samples -> accumulation grids.
        // Paper §4.2: dried cells are solid in PRESSURE PROJECTION (fluid
        // divergence/Jacobi/gradient), not here. The rasterize shader splats
        // each sample at its actual position; the fluid solve deflects new
        // paint around/above dried cells. (Earlier "deposit climbs above
        // solid" SRVs removed — that hack broke strokes at higher grid res.)
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

        // Step 6 (bristle -> main-grid merge) is intentionally OMITTED.
        //
        // Paper §5 (line 218): "brush bristles are not in direct contact with
        // grid-based liquid." The bristle density/color/velocity rasterized
        // above are NOT paint mass to inject into the grid — per paper §4.1
        // (line 118) and §4.2 (line 124) they serve only as (a) the sample
        // capacity field ψ for §5.1 Eq.12 (read by bristle_liquid_transfer as
        // bristle_psi), and (b) boundary conditions for pressure projection
        // (§4.2, applied in fluid_divergence/jacobi/gradient).
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
        // itself is retained — the fluid node still uses it for the particle
        // rasterize → grid transfer.

        rc.destroy(bristle_cb);
    };

    // ======================================================================
    // SUB-STEP LOOP (brush_paint_sim ~1383-1478). Subdivide the frame
    // displacement into <= one brush-diameter steps; deposit at each.
    // ======================================================================
    {
        // Paint supply reservoir — paper §5.1: a dipped brush carries a FINITE
        // ink charge in its bristles, depleted as paint is emitted. The
        // previous code refilled EVERY sample to ink_amount EACH frame, an
        // unbounded source that minted mass continuously (supply → ABSORB →
        // sample overload → emit particles → particle_to_grid deposit), so
        // grid density grew without limit and bloomed into a black blob.
        //
        // Now the reservoir is refilled ONCE at stroke_start ("re-dip"),
        // sized for a full stroke. ABSORB drains it sample-by-sample; once it
        // is exhausted the brush runs dry (as a real brush does), capping the
        // total paint mass that can enter the grid. The per-sample amount is
        // ink_amount scaled by a nominal stroke length so a stroke lays down
        // visible paint before drying out.
        if (bp.stroke_start) {
            // dip_charge scales with M_max (MUST match node_brush_wb_bristle's
            // blc.M_max = 0.03): ABSORB saturates a sample to M_j per frame, so
            // a charge of M_max*N sustains ~N frames of emission. ink_amount
            // modulates how heavily loaded the dip is. With M_max=0.03 and
            // ink_amount=0.8 → 0.72 per sample, ~24 frames of active paint.
            const float dip_charge = 0.03f * ink_amount * 30.0f;
            std::vector<float> supply(Nb * S, dip_charge);
            auto cmd = rc.create(CommandListDesc{});
            cmd->open();
            cmd->writeBuffer(
                field->sample_supply,
                supply.data(),
                supply.size() * sizeof(float));
            cmd->close();
            device->executeCommandList(cmd);
            device->waitForIdle();
            rc.destroy(cmd);
        }

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

        for (int s = 0; s < n_sub; s++) {
            float t =
                (static_cast<float>(s) + 0.5f) / static_cast<float>(n_sub);
            glm::vec3 sub_pos =
                field->has_prev_brush_pos
                    ? glm::mix(field->prev_brush_pos, brush_pos_3d, t)
                    : brush_pos_3d;
            // Vel/omega are instantaneous rates (NOT divided by n_sub); only
            // the integration time dt_sub shrinks per sub-step.
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
                dt_sub);
        }

        // Record this frame's brush center + velocity for the next frame.
        field->prev_brush_pos = brush_pos_3d;
        field->has_prev_brush_pos = true;
        field->prev_brush_vel = brush_vel_3d;
        field->prev_angular_vel = brush_angular_vel;
    }

    params.set_output("State", zs);
    params.set_output("Brush Point", bp);
    return true;
}

NODE_DECLARATION_UI(brush_wb_deposit);

NODE_DECLARATION_ALWAYS_DIRTY(brush_wb_deposit);

NODE_DEF_CLOSE_SCOPE
