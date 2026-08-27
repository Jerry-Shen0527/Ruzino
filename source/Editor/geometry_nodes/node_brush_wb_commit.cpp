// node_brush_wb_commit — Wetbrush OUTPUT sub-step.
//
// Receives the final field from brush_wb_fluid and:
//   - reads back fidelity statistics into the debug output ports;
//   - emits the Paint Field 3D geometry (one point per painted 3D voxel of the
//     active window, in world space) so downstream consumers see the paint.
//
// Paper §4.2: the 3D grid is global and persistent — no commit step. The 2D
// canvas layer has been removed (footnote 1 rejects height-field/2D). The
// "Paint Particles" port is kept empty for socket compatibility.
//
// The field is forwarded on the "State" output so the zone feeds it back
// simulation_out -> simulation_in for the next frame.

#include <fstream>
#include <memory>

#include "GCore/Components/PointsComponent.h"
#include "GCore/GOP.h"
#include "GCore/geom_payload.hpp"
#include "GPUContext/compute_context.hpp"  // CommandListDesc
#include "RHI/ResourceManager/resource_allocator.hpp"
#include "RHI/shared_buffer_registry.hpp"
#include "brush_sim_common.hpp"  // WetbrushSimState, WetbrushZoneState, brush_* helpers
#include "geom_node_base.h"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(brush_wb_commit)
{
    b.add_input<Ruzino::WetbrushZoneState>("State");
    // The stroke Geometry, forwarded unchanged. OPTIONAL: pen-motion graphs
    // (mock_pen_motion) carry no curve through the zone, so this slot is only
    // wired in curve-replay graphs (mock_strokes -> sim_in), where the zone's
    // group sync mirrors the boundary slot onto sim_out and commit is the
    // interior forwarder. Unwired → forwarded as empty geometry.
    b.add_input<Geometry>("Stroke Curves").optional(true);

    // Outputs mirror brush_paint_sim so the existing fidelity-test harness
    // (read 8 debug ports) works unchanged.
    b.add_output<Geometry>("Paint Particles");
    // The global 3D density grid as a point cloud: one point per painted 3D
    // voxel of the global grid (field->density, grid_res³), in world space.
    // This is the paper §6 render target — the 3D density grid. Widths carry
    // the instantaneous density (bounded, conserved). render_wetbrush.py
    // accumulates these across frames into one large 3D field for the volume
    // renderer.
    b.add_output<Geometry>("Paint Field 3D");
    b.add_output<float>("Max Divergence");
    b.add_output<float>("Mean Divergence");
    b.add_output<float>("Total Density");
    b.add_output<float>("Total Color R");
    b.add_output<float>("Total Color Y");
    b.add_output<float>("Total Color B");
    b.add_output<int>("Particle Count");
    b.add_output<float>("Total Particle Mass");
    // Field forwarded for zone feedback (carries the persistent canvas).
    b.add_output<Ruzino::WetbrushZoneState>("State");
    // Stroke forwarded so simulation_out's mirrored [Stroke Curves] slot is
    // filled (the zone group sync requires all boundary slots to be present).
    b.add_output<Geometry>("Stroke Curves");
}

NODE_EXECUTION_FUNCTION(brush_wb_commit)
{
    using Ruzino::WetbrushSimState;
    using Ruzino::WetbrushZoneState;

    WetbrushZoneState zs = params.get_input<WetbrushZoneState>("State");
    auto& field = zs.state;
    // Optional socket (pen-motion graphs leave it unwired): has_input guard —
    // get_input on an unconnected socket dereferences an empty meta_any.
    Geometry stroke = params.has_input("Stroke Curves")
                          ? params.get_input<Geometry>("Stroke Curves")
                          : Geometry{};

    auto& rc = get_resource_allocator();
    auto device = RHI::get_device();

    auto make_particles = [&]() -> std::pair<Geometry, PointsComponent*> {
        Geometry geom;
        auto pts = std::make_shared<PointsComponent>(&geom);
        geom.attach_component(pts);
        return { std::move(geom), pts.get() };
    };
    auto emit_empty = [&]() {
        auto [geom, pts] = make_particles();
        params.set_output("Paint Particles", std::move(geom));
        auto [geom3d, pts3d] = make_particles();
        params.set_output("Paint Field 3D", std::move(geom3d));
        params.set_output("Max Divergence", 0.0f);
        params.set_output("Mean Divergence", 0.0f);
        params.set_output("Total Density", 0.0f);
        params.set_output("Total Color R", 0.0f);
        params.set_output("Total Color Y", 0.0f);
        params.set_output("Total Color B", 0.0f);
        params.set_output("Particle Count", 0);
        params.set_output("Total Particle Mass", 0.0f);
    };

    if (!field) {
        spdlog::warn("brush_wb_commit: no field in state (graph mis-wired)");
        emit_empty();
        params.set_output("State", zs);
        params.set_output("Stroke Curves", stroke);
        return true;
    }

    // Global grid total (the buffers are now global-sized, not window-sized).
    const int grid_n3d = field->grid_res * field->grid_res * field->grid_res_z;
    const float cell_sz =
        field->grid_paper / static_cast<float>(field->grid_res);
    const int max_ptcl = WetbrushSimState::MAX_PARTICLES;

    // ======================================================================
    // PACK + REGISTER: pack density/color into a Float4 buffer and register it
    // in the shared GPU buffer registry. The render rprim looks this up by key
    // to consume the paint field with zero copy (no CPU readback / USD primvar
    // round-trip). Falls back to primvar if the rprim can't find the key.
    // ======================================================================
    {
        // Ensure the pack shader is compiled (deposit usually does this first).
        if (!field->pack_program)
            field->pack_program =
                Ruzino::brush_compile_shader(rc, "pack_float4.slang");

        Ruzino::SimConstants pack_cb = {};
        pack_cb.res = field->grid_res;
        pack_cb.res_z = field->grid_res_z;
        // Window mapping for the swarm-raster composite (pack_float4.slang):
        // window cells add the live particle mass, everything else is pure
        // canvas grid. Origin follows the sim's current window.
        pack_cb.window_origin_x = field->win_origin_x;
        pack_cb.window_origin_y = field->win_origin_y;
        pack_cb.window_origin_z = 0;
        pack_cb.window_size_x =
            std::min(WetbrushSimState::WIN_ALLOC_XY, field->grid_res);
        pack_cb.window_size_y = pack_cb.window_size_x;
        pack_cb.window_size_z = field->grid_res_z;
        nvrhi::BufferHandle pack_cb_buf;
        Ruzino::brush_upload_cb(
            rc, device, &pack_cb, sizeof(pack_cb), "wb_pack_cb", pack_cb_buf);
        Ruzino::brush_dispatch(
            rc,
            field->pack_program,
            { { "density", field->density },
              { "color_r", field->color_r },
              { "color_y", field->color_y },
              { "color_b", field->color_b },
              { "ptcl_density", field->ptcl_density },
              { "ptcl_rast_r", field->ptcl_rast_r },
              { "ptcl_rast_y", field->ptcl_rast_y },
              { "ptcl_rast_b", field->ptcl_rast_b } },
            { { "packed_out", field->packed_paint } },
            pack_cb_buf,
            grid_n3d);
        rc.destroy(pack_cb_buf);

        // Flush the pack dispatch before registering the buffer. brush_dispatch
        // only enqueues the compute work; without this flush the registry would
        // hand the renderer a buffer whose contents are still being written.
        //
        // We deliberately do NOT call setPermanentBufferState(ShaderResource)
        // here. That call is IRREVERSIBLE: it flips packed_paint's permanent
        // state to ShaderResource, but the very next frame's pack dispatch
        // needs it back in UnorderedAccess. nvrhi then logs "doesn't have the
        // right state bits. Required: 0x80 (UAV), present: 0x60 (SRV)" and
        // SKIPS the pack write — so packed_paint keeps stale (frame-1) data
        // and the renderer shows nothing. Instead the buffer keeps its
        // allocation-time permanent state (UnorderedAccess, keepInitialState),
        // and nvrhi's automatic barrier tracking transitions it UAV→SRV when
        // the render rprim binds it as a RawBuffer_SRV, and back to UAV on the
        // next pack. waitForIdle gives the cross-command-list synchronization
        // the interleaved sim-tick → render-frame loop needs.
        {
            auto flush_cmd = rc.create(CommandListDesc{});
            flush_cmd->open();
            flush_cmd->close();
            device->executeCommandList(flush_cmd);
            device->waitForIdle();
            rc.destroy(flush_cmd);
        }

        // Register the packed buffer for zero-copy render consumption.
        // The metadata blob carries the grid geometry so the render rprim can
        // build its AABB without reading USD primvars (both sides agree on this
        // POD layout out-of-band — the registry treats it as opaque bytes).
        struct PaintFieldMeta {
            uint32_t resX, resY, resZ;
            float cellSize;
            float gridMinX, gridMinY, gridMinZ;
        };
        PaintFieldMeta meta;
        meta.resX = static_cast<uint32_t>(field->grid_res);
        meta.resY = static_cast<uint32_t>(field->grid_res);
        meta.resZ = static_cast<uint32_t>(field->grid_res_z);
        meta.cellSize = cell_sz;
        meta.gridMinX = -field->grid_paper * 0.5f + field->grid_center.x;
        meta.gridMinY = -field->grid_paper * 0.5f + field->grid_center.y;
        meta.gridMinZ = field->grid_center_z - field->grid_height * 0.5f;

        Ruzino::SharedGPUBufferRegistry::get().register_buffer(
            "wetbrush_paint_field",
            field->packed_paint,
            static_cast<size_t>(grid_n3d) * sizeof(float) * 4,
            &meta,
            sizeof(meta));
    }

    // ======================================================================
    // READBACK: the full global 3D grid. The grid is now global and persistent
    // (paper §4.2) — no commit step. We read the entire grid so stats and the
    // Paint Field 3D output reflect ALL painted cells, not just the window.
    // ======================================================================
    auto readback = [&](nvrhi::BufferHandle buf, int n) -> std::vector<float> {
        std::vector<float> data(n);
        auto rb = rc.create(
            nvrhi::BufferDesc{}
                .setByteSize(static_cast<size_t>(n) * sizeof(float))
                .setCpuAccess(nvrhi::CpuAccessMode::Read)
                .setDebugName("wb_readback"));
        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        cmd->copyBuffer(rb, 0, buf, 0, static_cast<size_t>(n) * sizeof(float));
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        void* mapped = device->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
        memcpy(data.data(), mapped, static_cast<size_t>(n) * sizeof(float));
        device->unmapBuffer(rb);
        rc.destroy(rb);
        rc.destroy(cmd);
        return data;
    };

    auto density_cpu = readback(field->density, grid_n3d);
    auto cr_cpu = readback(field->color_r, grid_n3d);
    auto cy_cpu = readback(field->color_y, grid_n3d);
    auto cb_cpu = readback(field->color_b, grid_n3d);

    float max_div = 0.0f, mean_div = 0.0f;
    {
        // divergence_buf is WINDOW-SIZED (paper §4.2 transient solve field) —
        // read only the window extent.
        const int win_n3d_div =
            std::min(WetbrushSimState::WIN_ALLOC_XY, field->grid_res) *
            std::min(WetbrushSimState::WIN_ALLOC_XY, field->grid_res) *
            field->grid_res_z;
        auto div_cpu = readback(field->divergence_buf, win_n3d_div);
        double div_sum = 0.0;
        int div_count = 0;
        for (int i = 0; i < win_n3d_div; ++i) {
            float ad = std::fabs(div_cpu[i]);
            max_div = std::max(max_div, ad);
            div_sum += ad;
            ++div_count;
        }
        mean_div =
            div_count > 0 ? static_cast<float>(div_sum / div_count) : 0.0f;
    }

    double tot_density = 0.0, tot_r = 0.0, tot_y = 0.0, tot_b = 0.0;
    for (int i = 0; i < grid_n3d; ++i) {
        tot_density += density_cpu[i];
        tot_r += cr_cpu[i];
        tot_y += cy_cpu[i];
        tot_b += cb_cpu[i];
    }

    // DIAGNOSTIC: particle rasterize accumulator (window-sized). If ptcl_d_sum
    // > 0 but tot_density == 0, rasterize wrote but merge dropped it. If
    // ptcl_d_sum == 0 with particles > 0, particles exist but rasterize wrote
    // nothing (position/alive problem).
    double ptcl_d_sum = 0.0;
    int win_n3d_diag = field->win_alloc_z > 0
                           ? WetbrushSimState::WIN_ALLOC_XY *
                                 WetbrushSimState::WIN_ALLOC_XY *
                                 field->win_alloc_z
                           : 0;
    if (field->ptcl_density && win_n3d_diag > 0) {
        auto ptcl_d_cpu = readback(field->ptcl_density, win_n3d_diag);
        for (int i = 0; i < win_n3d_diag; ++i)
            ptcl_d_sum += ptcl_d_cpu[i];
    }

    int ptcl_count = 0;
    float ptcl_mass = 0.0f;
    std::vector<float> ptcl_positions;
    std::vector<float> ptcl_colors;
    std::vector<uint32_t> ptcl_alive_flags;
    if (field->particles_initialized && field->ptcl_counter) {
        uint32_t cnt = 0;
        auto rb = rc.create(
            nvrhi::BufferDesc{}
                .setByteSize(sizeof(uint32_t))
                .setCpuAccess(nvrhi::CpuAccessMode::Read)
                .setDebugName("wb_ptcl_counter_rb"));
        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        cmd->copyBuffer(rb, 0, field->ptcl_counter, 0, sizeof(uint32_t));
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        void* mapped = device->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
        memcpy(&cnt, mapped, sizeof(uint32_t));
        device->unmapBuffer(rb);
        rc.destroy(rb);
        rc.destroy(cmd);
        ptcl_count = static_cast<int>(cnt);

        if (ptcl_count > 0) {
            int n = std::min(ptcl_count, max_ptcl);
            constexpr int STRIDE = 4;
            ptcl_positions.resize(n * STRIDE);
            ptcl_colors.resize(n * STRIDE);
            ptcl_alive_flags.resize(n);
            auto read_structured =
                [&](nvrhi::BufferHandle buf, int elem_bytes, void* dst) {
                    auto rb2 = rc.create(
                        nvrhi::BufferDesc{}
                            .setByteSize(static_cast<size_t>(n) * elem_bytes)
                            .setCpuAccess(nvrhi::CpuAccessMode::Read)
                            .setDebugName("wb_ptcl_rb"));
                    auto cmd2 = rc.create(CommandListDesc{});
                    cmd2->open();
                    cmd2->copyBuffer(
                        rb2, 0, buf, 0, static_cast<size_t>(n) * elem_bytes);
                    cmd2->close();
                    device->executeCommandList(cmd2);
                    device->waitForIdle();
                    void* mapped2 =
                        device->mapBuffer(rb2, nvrhi::CpuAccessMode::Read);
                    memcpy(dst, mapped2, static_cast<size_t>(n) * elem_bytes);
                    device->unmapBuffer(rb2);
                    rc.destroy(rb2);
                    rc.destroy(cmd2);
                };
            read_structured(
                field->ptcl_pos, sizeof(float) * STRIDE, ptcl_positions.data());
            read_structured(
                field->ptcl_color, sizeof(float) * STRIDE, ptcl_colors.data());
            read_structured(
                field->ptcl_alive, sizeof(uint32_t), ptcl_alive_flags.data());
            for (int i = 0; i < n; ++i)
                if (ptcl_alive_flags[i] != 0)
                    ptcl_mass += ptcl_colors[i * STRIDE + 3];
        }
    }

    // ======================================================================
    // WB_DEBUG_DUMP_PTCL=1: per-cook swarm kinematics + window grid-velocity
    // stats. Detects a frozen swarm (centroid drift ≈ 0, vmean ≈ 0) and a dead
    // velocity field (gridvmax ≈ 0) in one glance.
    // ======================================================================
    {
        static const bool dump_ptcl = [] {
            const char* e = std::getenv("WB_DEBUG_DUMP_PTCL");
            return e && e[0] == '1';
        }();
        static int dump_frame = 0;
        if (dump_ptcl) {
            ++dump_frame;
            float cx = 0, cy = 0, cz = 0;
            float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f,
                  minz = 1e30f, maxz = -1e30f;
            double vmean = 0.0;
            float vmax = 0.0f;
            int n_live = 0;
            if (ptcl_count > 0) {
                int n = std::min(ptcl_count, max_ptcl);
                std::vector<float> ptcl_vels(n * 4);
                auto rb = rc.create(
                    nvrhi::BufferDesc{}
                        .setByteSize(static_cast<size_t>(n) * 16)
                        .setCpuAccess(nvrhi::CpuAccessMode::Read)
                        .setDebugName("wb_ptcl_vel_rb"));
                auto cmd = rc.create(CommandListDesc{});
                cmd->open();
                cmd->copyBuffer(
                    rb, 0, field->ptcl_vel, 0, static_cast<size_t>(n) * 16);
                cmd->close();
                device->executeCommandList(cmd);
                device->waitForIdle();
                void* mapped =
                    device->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
                memcpy(ptcl_vels.data(), mapped, static_cast<size_t>(n) * 16);
                device->unmapBuffer(rb);
                rc.destroy(rb);
                rc.destroy(cmd);
                for (int i = 0; i < n; ++i) {
                    if (ptcl_alive_flags.empty() || ptcl_alive_flags[i] == 0)
                        continue;
                    float x = ptcl_positions[i * 4 + 0];
                    float y = ptcl_positions[i * 4 + 1];
                    float z = ptcl_positions[i * 4 + 2];
                    cx += x;
                    cy += y;
                    cz += z;
                    minx = std::min(minx, x);
                    maxx = std::max(maxx, x);
                    miny = std::min(miny, y);
                    maxy = std::max(maxy, y);
                    minz = std::min(minz, z);
                    maxz = std::max(maxz, z);
                    float sp = std::sqrt(
                        ptcl_vels[i * 4 + 0] * ptcl_vels[i * 4 + 0] +
                        ptcl_vels[i * 4 + 1] * ptcl_vels[i * 4 + 1] +
                        ptcl_vels[i * 4 + 2] * ptcl_vels[i * 4 + 2]);
                    vmean += sp;
                    vmax = std::max(vmax, sp);
                    ++n_live;
                }
                if (n_live > 0) {
                    cx /= float(n_live);
                    cy /= float(n_live);
                    cz /= float(n_live);
                }
            }
            // Window grid velocity (window-sized vel_x/y/z buffers).
            float gridvmax = 0.0f;
            float gv_argmax = 0.0f;
            int gv_argi = -1, gv_argcomp = -1, gv_over1 = 0;
            float pmax = 0.0f, dmax = 0.0f;
            {
                const int WIN_XY =
                    std::min(WetbrushSimState::WIN_ALLOC_XY, field->grid_res);
                const int win_n3d = WIN_XY * WIN_XY * field->grid_res_z;
                auto read_win = [&](nvrhi::BufferHandle buf) {
                    std::vector<float> data(win_n3d);
                    auto rb = rc.create(
                        nvrhi::BufferDesc{}
                            .setByteSize(
                                static_cast<size_t>(win_n3d) * sizeof(float))
                            .setCpuAccess(nvrhi::CpuAccessMode::Read)
                            .setDebugName("wb_winvel_rb"));
                    auto cmd = rc.create(CommandListDesc{});
                    cmd->open();
                    cmd->copyBuffer(
                        rb,
                        0,
                        buf,
                        0,
                        static_cast<size_t>(win_n3d) * sizeof(float));
                    cmd->close();
                    device->executeCommandList(cmd);
                    device->waitForIdle();
                    void* mapped =
                        device->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
                    memcpy(
                        data.data(),
                        mapped,
                        static_cast<size_t>(win_n3d) * sizeof(float));
                    device->unmapBuffer(rb);
                    rc.destroy(rb);
                    rc.destroy(cmd);
                    return data;
                };
                auto vx = read_win(field->vel_x);
                auto vy = read_win(field->vel_y);
                auto vz = read_win(field->vel_z);
                // Projection internals (eruption hunt): warm-started pressure
                // and the divergence rhs peaks — distinguishes "rhs spike"
                // from "pressure warm-start runaway".
                if (true) {
                    auto pa = read_win(field->pressure_a);
                    auto db = read_win(field->divergence_buf);
                    for (int i = 0; i < win_n3d; ++i) {
                        pmax = std::max(pmax, std::abs(pa[i]));
                        dmax = std::max(dmax, std::abs(db[i]));
                    }
                }
                // Argmax: WHERE the peak grid velocity lives (window-local
                // cell + dominant component) — distinguishes "floor ring",
                // "brush rim", "above the brush" etc. when hunting the
                // press-phase velocity buildup (blob-test explosion).
                for (int i = 0; i < win_n3d; ++i) {
                    float sp = std::sqrt(
                        vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i]);
                    gridvmax = std::max(gridvmax, sp);
                    if (sp > 1.0f)
                        ++gv_over1;
                    if (sp > gv_argmax) {
                        gv_argmax = sp;
                        gv_argi = i;
                        float ax = std::abs(vx[i]), ay = std::abs(vy[i]),
                              az = std::abs(vz[i]);
                        gv_argcomp =
                            (ax >= ay && ax >= az) ? 0 : (ay >= az ? 1 : 2);
                    }
                }
            }
            // Window-local cell of the argmax (grid z = lz, x/y = lx/ly).
            int gv_lx = -1, gv_ly = -1, gv_lz = -1;
            if (gv_argi >= 0) {
                const int WIN_XY_d =
                    std::min(WetbrushSimState::WIN_ALLOC_XY, field->grid_res);
                gv_lz = gv_argi / (WIN_XY_d * WIN_XY_d);
                int rem = gv_argi - gv_lz * WIN_XY_d * WIN_XY_d;
                gv_ly = rem / WIN_XY_d;
                gv_lx = rem - gv_ly * WIN_XY_d;
            }
            spdlog::info(
                "[wb-ptcl] f={} n={} live={} mass={:.3f} "
                "cen=({:.4f},{:.4f},{:.4f}) "
                "bbox=[{:.3f},{:.3f}]x[{:.3f},{:.3f}]x[{:.3f},{:.3f}] "
                "vmean={:.5f} vmax={:.5f} gridvmax={:.5f} "
                "gv@=(lx{},ly{},lz{},c{}) over1={} pmax={:.4f} dmax={:.6f}",
                dump_frame,
                ptcl_count,
                n_live,
                ptcl_mass,
                cx,
                cy,
                cz,
                minx,
                maxx,
                miny,
                maxy,
                minz,
                maxz,
                n_live > 0 ? vmean / n_live : 0.0,
                vmax,
                gridvmax,
                gv_lx,
                gv_ly,
                gv_lz,
                gv_argcomp,
                gv_over1,
                pmax,
                dmax);

            // Mass-balance split: grid vs swarm, and how much of the grid is
            // above the render threshold (kDensitySurface ⟺ raw d ≈ 0.006).
            // Discriminates "deposit starvation" (grid total ≪ emitted) from
            // "advection dilution" (grid total large, cells-above-threshold
            // collapsed). rastmax = swarm composite peak.
            {
                double tot_d = 0.0;
                int cells_vis = 0, cells_paint = 0;
                float dmax = 0.0f;
                for (int i = 0; i < grid_n3d; ++i) {
                    float d = density_cpu[i];
                    tot_d += d;
                    dmax = std::max(dmax, d);
                    if (d > 0.006f)
                        ++cells_vis;
                    if (d > 1e-5f)
                        ++cells_paint;
                }
                const int WIN_XY =
                    std::min(WetbrushSimState::WIN_ALLOC_XY, field->grid_res);
                const int win_n3d = WIN_XY * WIN_XY * field->grid_res_z;
                float rastmax = 0.0f;
                {
                    std::vector<float> rast(win_n3d);
                    auto rb = rc.create(
                        nvrhi::BufferDesc{}
                            .setByteSize(
                                static_cast<size_t>(win_n3d) * sizeof(float))
                            .setCpuAccess(nvrhi::CpuAccessMode::Read)
                            .setDebugName("wb_rast_rb"));
                    auto cmd = rc.create(CommandListDesc{});
                    cmd->open();
                    cmd->copyBuffer(
                        rb,
                        0,
                        field->ptcl_density,
                        0,
                        static_cast<size_t>(win_n3d) * sizeof(float));
                    cmd->close();
                    device->executeCommandList(cmd);
                    device->waitForIdle();
                    void* mapped =
                        device->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
                    memcpy(
                        rast.data(),
                        mapped,
                        static_cast<size_t>(win_n3d) * sizeof(float));
                    device->unmapBuffer(rb);
                    rc.destroy(rb);
                    rc.destroy(cmd);
                    for (int i = 0; i < win_n3d; ++i)
                        rastmax = std::max(rastmax, rast[i]);
                }
                spdlog::info(
                    "[wb-mass] f={} grid_tot={:.1f} grid_max={:.3f} "
                    "cells_vis={} cells_paint={} swarm={:.1f} rastmax={:.4f}",
                    dump_frame,
                    tot_d,
                    dmax,
                    cells_vis,
                    cells_paint,
                    ptcl_mass,
                    rastmax);
            }
        }
    }

    params.set_output("Max Divergence", max_div);
    params.set_output("Mean Divergence", mean_div);
    params.set_output("Total Density", static_cast<float>(tot_density));
    params.set_output("Total Color R", static_cast<float>(tot_r));
    params.set_output("Total Color Y", static_cast<float>(tot_y));
    params.set_output("Total Color B", static_cast<float>(tot_b));
    params.set_output("Particle Count", ptcl_count);
    params.set_output("Total Particle Mass", ptcl_mass);

    // ======================================================================
    // DEBUG DRAW PACK: build three debug-visualization buffers (live
    // active-window particles, non-zero grid voxels, bristle capsule
    // segments) in the renderer's blocked point/segment layout and register
    // them in the shared GPU buffer registry. Hd_RUZINO_Points prims whose
    // "debugKey" primvar names one of these keys consume them zero-copy —
    // see render_wetbrush_debug.py. Pure visualization: no CPU round-trip
    // except the 4-byte compacted voxel count.
    // ======================================================================
    {
        // Allocate once; capacity is fixed so buffer block offsets never move.
        // Flag set mirrors packed_paint's (deposit.cpp): the pack shaders bind
        // these as RWStructuredBuffer UAVs and the render side reads them as
        // RawBuffer_SRV — both view kinds must be creatable.
        auto alloc_debug =
            [&](nvrhi::BufferHandle& h, size_t n_floats, const char* name) {
                size_t bytes = n_floats * sizeof(float);
                if (h && h->getDesc().byteSize == bytes)
                    return;
                if (h)
                    rc.destroy(h);
                h = rc.create(
                    nvrhi::BufferDesc{}
                        .setByteSize(bytes)
                        .setStructStride(sizeof(float))
                        .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                        .setKeepInitialState(true)
                        .setCanHaveUAVs(true)
                        .setCanHaveTypedViews(true)
                        .setCanHaveRawViews(true)
                        .setDebugName(name));
            };
        alloc_debug(field->debug_ptcl_buf, size_t(max_ptcl) * 7, "wb_dbg_ptcl");
        alloc_debug(
            field->debug_voxel_buf,
            size_t(WetbrushSimState::DEBUG_MAX_VOXELS) * 7,
            "wb_dbg_voxel");
        alloc_debug(
            field->debug_bristle_buf,
            size_t(WetbrushSimState::NUM_BRISTLES) *
                (WetbrushSimState::VERTS_PER_BRISTLE - 1) * 10,
            "wb_dbg_bristle");
        if (!field->debug_voxel_counter)
            field->debug_voxel_counter = rc.create(
                nvrhi::BufferDesc{}
                    .setByteSize(sizeof(uint32_t))
                    .setStructStride(sizeof(uint32_t))
                    .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
                    .setKeepInitialState(true)
                    .setCanHaveUAVs(true)
                    .setCanHaveTypedViews(true)
                    .setCanHaveRawViews(true)
                    .setDebugName("wb_dbg_voxel_cnt"));

        if (!field->debug_pack_particles_program)
            field->debug_pack_particles_program =
                Ruzino::brush_compile_shader(rc, "debug_pack_particles.slang");
        if (!field->debug_pack_voxels_program)
            field->debug_pack_voxels_program =
                Ruzino::brush_compile_shader(rc, "debug_pack_voxels.slang");
        if (!field->debug_pack_bristles_program)
            field->debug_pack_bristles_program =
                Ruzino::brush_compile_shader(rc, "debug_pack_bristles.slang");

        Ruzino::brush_reset_counter(rc, device, field->debug_voxel_counter);

        // Debug-render radii, in WORLD units (res-independent). Env-tunable
        // (WB_DEBUG_PTCL_R / WB_DEBUG_VOXEL_R / WB_DEBUG_BRISTLE_R, also
        // world units). These used to be CELL fractions, tuned at res 512 —
        // at 1024 the bristle hair radius shrank to 0.44px and the brush
        // rendered as one sub-pixel thin line instead of a hair cluster (the
        // "blue line" report: geometry and capsule intersection were both
        // correct, only the display radius collapsed). Defaults sized
        // against the brush (radius 0.02): bristles 0.001 → hair diameter
        // 0.002 exceeds the ~0.0014 hair spacing, so the 600 hairs overlap
        // into a readable solid brush; voxels slightly bigger dots;
        // particles smaller still so the swarm reads distinct from paint.
        static const float ptcl_radius = [] {
            const char* env = std::getenv("WB_DEBUG_PTCL_R");
            return env ? std::max(std::atof(env), 1e-5) : 0.0005;
        }();
        static const float voxel_radius = [] {
            const char* env = std::getenv("WB_DEBUG_VOXEL_R");
            return env ? std::max(std::atof(env), 1e-5) : 0.0008;
        }();
        static const float bristle_radius = [] {
            const char* env = std::getenv("WB_DEBUG_BRISTLE_R");
            return env ? std::max(std::atof(env), 1e-5) : 0.001;
        }();

        // Live particles (only when the pool exists; register count=0
        // otherwise so the render prim sees an empty cloud, not a dead key).
        if (field->ptcl_pos && field->ptcl_color && ptcl_count > 0) {
            struct {
                int count;
                int capacity;
                float radius;
                float pad;
            } cb{};
            cb.count = ptcl_count;
            cb.capacity = max_ptcl;
            cb.radius = ptcl_radius;
            nvrhi::BufferHandle cb_buf;
            Ruzino::brush_upload_cb(
                rc, device, &cb, sizeof(cb), "wb_dbg_ptcl_cb", cb_buf);
            Ruzino::brush_dispatch(
                rc,
                field->debug_pack_particles_program,
                { { "ptcl_pos", field->ptcl_pos },
                  { "ptcl_color", field->ptcl_color } },
                { { "debug_out", field->debug_ptcl_buf } },
                cb_buf,
                max_ptcl);
            rc.destroy(cb_buf);
        }

        // Non-zero voxels (GPU compaction; same world mapping + threshold as
        // the Paint Field 3D output above).
        {
            struct {
                int res;
                int res_z;
                float cell_sz;
                float paper;
                float cx;
                float cy;
                float z_floor;
                float cell_z;
                float radius;
                float eps;
                int max_out;
                float pad;
            } cb{};
            cb.res = field->grid_res;
            cb.res_z = field->grid_res_z;
            cb.cell_sz = cell_sz;
            cb.paper = field->grid_paper;
            cb.cx = field->grid_center.x;
            cb.cy = field->grid_center.y;
            cb.z_floor = field->grid_center_z - field->grid_height * 0.5f;
            cb.cell_z =
                field->grid_height / static_cast<float>(field->grid_res_z);
            cb.radius = voxel_radius;
            cb.eps = 0.001f;
            cb.max_out = WetbrushSimState::DEBUG_MAX_VOXELS;
            nvrhi::BufferHandle cb_buf;
            Ruzino::brush_upload_cb(
                rc, device, &cb, sizeof(cb), "wb_dbg_voxel_cb", cb_buf);
            Ruzino::brush_dispatch(
                rc,
                field->debug_pack_voxels_program,
                { { "density", field->density },
                  { "color_r", field->color_r },
                  { "color_y", field->color_y },
                  { "color_b", field->color_b } },
                { { "debug_out", field->debug_voxel_buf },
                  { "counter", field->debug_voxel_counter } },
                cb_buf,
                grid_n3d);
            rc.destroy(cb_buf);
        }

        // Bristle capsule segments, colored by their current liquid load.
        if (field->bristle_data && field->bristles_initialized) {
            // Bristle-chain health diagnostic (cheap: 192KB readback, far
            // below the 4 grid readbacks above). NaN/degenerate segments are
            // exactly what the debug renderer needs to surface — degenerate
            // capsules render as a blob at the origin, NaN would corrupt DXR
            // traversal. Frame 1 legitimately reports all-zero chains (the
            // first simulate hasn't run yet).
            {
                const int nb = WetbrushSimState::NUM_BRISTLES;
                const int m = WetbrushSimState::VERTS_PER_BRISTLE;
                std::vector<float> bd(
                    size_t(nb) * m * 8);  // 2 float4 per vertex
                auto rb = rc.create(
                    nvrhi::BufferDesc{}
                        .setByteSize(bd.size() * sizeof(float))
                        .setCpuAccess(nvrhi::CpuAccessMode::Read)
                        .setDebugName("wb_bristle_rb"));
                auto cmd = rc.create(CommandListDesc{});
                cmd->open();
                cmd->copyBuffer(
                    rb, 0, field->bristle_data, 0, bd.size() * sizeof(float));
                cmd->close();
                device->executeCommandList(cmd);
                device->waitForIdle();
                void* mapped =
                    device->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
                memcpy(bd.data(), mapped, bd.size() * sizeof(float));
                device->unmapBuffer(rb);
                rc.destroy(rb);
                rc.destroy(cmd);
                // WB_DUMP_BRISTLES=<path-prefix>: write the raw bristle_data
                // readback (nb*m*2 float4: pos,vel per vertex) to
                // <prefix>_<frame>.bin for offline geometry analysis (the
                // pooled range above cannot tell a spread root disk from a
                // collapsed line).
                if (const char* dump = std::getenv("WB_DUMP_BRISTLES")) {
                    static int dump_frame = 0;
                    std::string path = std::string(dump) + "_" +
                                       std::to_string(dump_frame++) + ".bin";
                    std::ofstream f(path, std::ios::binary | std::ios::trunc);
                    if (f)
                        f.write(
                            reinterpret_cast<const char*>(bd.data()),
                            std::streamsize(bd.size() * sizeof(float)));
                }
                int nan_v = 0, inf_v = 0, degenerate = 0;
                float lo = 1e30f, hi = -1e30f;
                for (int i = 0; i < nb * m; ++i) {
                    float x = bd[i * 8 + 0], y = bd[i * 8 + 1],
                          z = bd[i * 8 + 2];
                    if (std::isnan(x) || std::isnan(y) || std::isnan(z)) {
                        ++nan_v;
                        continue;
                    }
                    if (std::isinf(x) || std::isinf(y) || std::isinf(z)) {
                        ++inf_v;
                        continue;
                    }
                    lo = std::min({ lo, x, y, z });
                    hi = std::max({ hi, x, y, z });
                }
                for (int b2 = 0; b2 < nb; ++b2)
                    for (int v2 = 0; v2 + 1 < m; ++v2) {
                        float ax = bd[(b2 * m + v2) * 8],
                              ay = bd[(b2 * m + v2) * 8 + 1],
                              az = bd[(b2 * m + v2) * 8 + 2];
                        float bx = bd[(b2 * m + v2 + 1) * 8],
                              by = bd[(b2 * m + v2 + 1) * 8 + 1],
                              bz = bd[(b2 * m + v2 + 1) * 8 + 2];
                        if (ax == bx && ay == by && az == bz)
                            ++degenerate;
                    }
                spdlog::info(
                    "wb_bristle_diag nan={} inf={} degenerate_segs={} "
                    "range=[{:.4f},{:.4f}]",
                    nan_v,
                    inf_v,
                    degenerate,
                    lo,
                    hi);
            }

            struct {
                int nb;
                int m;
                int s;
                float radius;
            } cb{};
            cb.nb = WetbrushSimState::NUM_BRISTLES;
            cb.m = WetbrushSimState::VERTS_PER_BRISTLE;
            cb.s = WetbrushSimState::SAMPLES_PER_BRISTLE;
            cb.radius = bristle_radius;
            nvrhi::BufferHandle cb_buf;
            Ruzino::brush_upload_cb(
                rc, device, &cb, sizeof(cb), "wb_dbg_bristle_cb", cb_buf);
            Ruzino::brush_dispatch(
                rc,
                field->debug_pack_bristles_program,
                { { "bristle_data", field->bristle_data },
                  { "sample_liquid", field->sample_liquid } },
                { { "debug_out", field->debug_bristle_buf } },
                cb_buf,
                cb.nb * (cb.m - 1));
            rc.destroy(cb_buf);
        }

        // Flush all three packs before registering — same reason as the
        // packed_paint flush above: the registry hands the renderer buffers
        // whose contents must already be written.
        {
            auto flush_cmd = rc.create(CommandListDesc{});
            flush_cmd->open();
            flush_cmd->close();
            device->executeCommandList(flush_cmd);
            device->waitForIdle();
            rc.destroy(flush_cmd);
        }

        // Read back the compacted voxel count (4 bytes) for the meta blob.
        uint32_t voxel_count = 0;
        {
            auto rb = rc.create(
                nvrhi::BufferDesc{}
                    .setByteSize(sizeof(uint32_t))
                    .setCpuAccess(nvrhi::CpuAccessMode::Read)
                    .setDebugName("wb_dbg_voxel_cnt_rb"));
            auto cmd = rc.create(CommandListDesc{});
            cmd->open();
            cmd->copyBuffer(
                rb, 0, field->debug_voxel_counter, 0, sizeof(uint32_t));
            cmd->close();
            device->executeCommandList(cmd);
            device->waitForIdle();
            void* mapped = device->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
            memcpy(&voxel_count, mapped, sizeof(uint32_t));
            device->unmapBuffer(rb);
            rc.destroy(rb);
            rc.destroy(cmd);
        }
        voxel_count =
            std::min(voxel_count, uint32_t(WetbrushSimState::DEBUG_MAX_VOXELS));

        // Layout contract with Hd_RUZINO_Points (points.h registry mode):
        // sphere points: [C pos float3][C radius][C rgb]; capsule segments:
        // [C A float3][C B float3][C radius][C rgb]. Blocks are spaced by
        // CAPACITY so the renderer can compute offsets from this meta alone.
        struct DebugDrawMeta {
            uint32_t count;
            uint32_t capacity;
            uint32_t isSegments;
            uint32_t pad;
        };
        auto reg = [&](const char* key,
                       nvrhi::BufferHandle buf,
                       uint32_t count,
                       uint32_t capacity,
                       uint32_t segs) {
            DebugDrawMeta meta{ count, capacity, segs, 0 };
            Ruzino::SharedGPUBufferRegistry::get().register_buffer(
                key,
                buf,
                size_t(capacity) * (segs ? 40 : 28),
                &meta,
                sizeof(meta));
        };
        const uint32_t bristle_segs = uint32_t(
            WetbrushSimState::NUM_BRISTLES *
            (WetbrushSimState::VERTS_PER_BRISTLE - 1));
        reg("wetbrush_debug_particles",
            field->debug_ptcl_buf,
            uint32_t(std::max(ptcl_count, 0)),
            uint32_t(max_ptcl),
            0);
        reg("wetbrush_debug_voxels",
            field->debug_voxel_buf,
            voxel_count,
            uint32_t(WetbrushSimState::DEBUG_MAX_VOXELS),
            0);
        reg("wetbrush_debug_bristles",
            field->debug_bristle_buf,
            field->bristles_initialized ? bristle_segs : 0u,
            bristle_segs,
            1);

        spdlog::info(
            "wb_debug_draw particles={} voxels={} bristle_segs={}",
            ptcl_count,
            voxel_count,
            field->bristles_initialized ? bristle_segs : 0u);
    }

    // DIAGNOSTIC: paint-mass accounting for the paper-faithful particle path.
    // density = grid paint mass (should be injected ONLY by particle
    // rasterize/transfer now that bristle direct-injection is removed).
    // particles = live particle count. ptcl_mass = summed particle mass.
    // If density stays ~0 the particle path isn't feeding the grid.
    spdlog::info(
        "wb_diag density={:.4f} color_r={:.4f} color_y={:.4f} color_b={:.4f} "
        "particles={} ptcl_mass={:.4f} ptcl_d_sum={:.4f}",
        tot_density,
        tot_r,
        tot_y,
        tot_b,
        ptcl_count,
        ptcl_mass,
        ptcl_d_sum);

    // ======================================================================
    // OUTPUT: "Paint Particles" — active FLIP/PIC particles with positions,
    // colors (RYB→RGB) and mass as width. Useful for debugging particle
    // distribution in Ruzino.exe via hdStorm.
    // ======================================================================
    {
        auto [particles, pts] = make_particles();
        const int N = static_cast<int>(ptcl_positions.size()) / 4;
        std::vector<glm::vec3> ptcl_pts_vec;
        std::vector<glm::vec3> ptcl_cols_vec;
        std::vector<float> ptcl_widths_vec;
        for (int i = 0; i < N; ++i) {
            if (ptcl_alive_flags[i] == 0)
                continue;
            float r_ryb = ptcl_colors[i * 4 + 0];
            float y_ryb = ptcl_colors[i * 4 + 1];
            float b_ryb = ptcl_colors[i * 4 + 2];
            float mass = ptcl_colors[i * 4 + 3];
            float rm = 1 - r_ryb, ym = 1 - y_ryb, bm = 1 - b_ryb;
            glm::vec3 rgb =
                rm * ym * bm * glm::vec3(1, 1, 1) +
                r_ryb * ym * bm * glm::vec3(1, 0, 0) +
                rm * y_ryb * bm * glm::vec3(1, 1, 0) +
                rm * ym * b_ryb * glm::vec3(0.163f, 0.373f, 0.6f) +
                r_ryb * y_ryb * bm * glm::vec3(1, 0.5f, 0) +
                r_ryb * ym * b_ryb * glm::vec3(0.5f, 0, 0.5f) +
                rm * y_ryb * b_ryb * glm::vec3(0, 0.66f, 0.2f) +
                r_ryb * y_ryb * b_ryb * glm::vec3(0.2f, 0.094f, 0.029f);
            ptcl_pts_vec.emplace_back(
                ptcl_positions[i * 4 + 0],
                ptcl_positions[i * 4 + 1],
                ptcl_positions[i * 4 + 2]);
            ptcl_cols_vec.push_back(rgb);
            // Width is in world units for USD point rendering. Use a small
            // fixed fraction of a cell so particles are visible but not huge;
            // the actual mass is not a size.
            ptcl_widths_vec.push_back(cell_sz * 0.5f);
        }
        pts->set_vertices(ptcl_pts_vec);
        pts->set_display_color(ptcl_cols_vec);
        pts->set_width(ptcl_widths_vec);
        params.set_output("Paint Particles", std::move(particles));
    }

    // ======================================================================
    // OUTPUT: the global 3D density grid (paper §6 render target). Emit one
    // point per painted 3D voxel of the GLOBAL grid, in world space. Widths
    // carry the instantaneous density (bounded, conserved). The render driver
    // accumulates these per-frame into one large 3D field. Color is the
    // per-voxel RYB->RGB mix (same cube model as above).
    // ======================================================================
    {
        auto [field3d_geom, field3d_pts] = make_particles();
        std::vector<glm::vec3> f3d_pts_vec;
        std::vector<glm::vec3> f3d_colors_vec;
        std::vector<float> f3d_widths_vec;
        const float cell_sz_f = cell_sz;
        const int N = field->grid_res;
        const int D = field->grid_res_z;
        constexpr float threshold = 0.001f;
        constexpr float rgb_white_cutoff = 0.9f;
        float canvas_floor_z = field->grid_center_z - field->grid_height * 0.5f;
        float cell_z = field->grid_height / static_cast<float>(D);
        for (int z = 0; z < D; ++z) {
            for (int y = 0; y < N; ++y) {
                for (int x = 0; x < N; ++x) {
                    // Global grid index (matches shader grid_idx_3d).
                    int gi = (z * N + y) * N + x;
                    float d = density_cpu[gi];
                    if (d <= threshold)
                        continue;
                    // Premultiplied color -> normalized RYB.
                    float r = std::min(
                        std::max(cr_cpu[gi] / (d + 1e-8f), 0.0f), 1.0f);
                    float yy = std::min(
                        std::max(cy_cpu[gi] / (d + 1e-8f), 0.0f), 1.0f);
                    float b = std::min(
                        std::max(cb_cpu[gi] / (d + 1e-8f), 0.0f), 1.0f);
                    float rm = 1 - r, ym = 1 - yy, bm = 1 - b;
                    glm::vec3 rgb =
                        rm * ym * bm * glm::vec3(1, 1, 1) +
                        r * ym * bm * glm::vec3(1, 0, 0) +
                        rm * yy * bm * glm::vec3(1, 1, 0) +
                        rm * ym * b * glm::vec3(0.163f, 0.373f, 0.6f) +
                        r * yy * bm * glm::vec3(1, 0.5f, 0) +
                        r * ym * b * glm::vec3(0.5f, 0, 0.5f) +
                        rm * yy * b * glm::vec3(0, 0.66f, 0.2f) +
                        r * yy * b * glm::vec3(0.2f, 0.094f, 0.029f);
                    if (std::min({ rgb.r, rgb.g, rgb.b }) >= rgb_white_cutoff)
                        continue;
                    // World position: global cell (x,y,z) center.
                    float wx = (x + 0.5f) * cell_sz_f -
                               field->grid_paper * 0.5f + field->grid_center.x;
                    float wy = (y + 0.5f) * cell_sz_f -
                               field->grid_paper * 0.5f + field->grid_center.y;
                    float wz = (z + 0.5f) * cell_z + canvas_floor_z;
                    f3d_pts_vec.emplace_back(wx, wy, wz);
                    f3d_colors_vec.push_back(rgb);
                    f3d_widths_vec.push_back(d);
                }
            }
        }
        field3d_pts->set_vertices(f3d_pts_vec);
        field3d_pts->set_display_color(f3d_colors_vec);
        field3d_pts->set_width(f3d_widths_vec);
        params.set_output("Paint Field 3D", std::move(field3d_geom));
    }

    params.set_output("State", zs);
    params.set_output("Stroke Curves", stroke);
    return true;
}

NODE_DECLARATION_UI(brush_wb_commit);

NODE_DECLARATION_ALWAYS_DIRTY(brush_wb_commit);

NODE_DEF_CLOSE_SCOPE
