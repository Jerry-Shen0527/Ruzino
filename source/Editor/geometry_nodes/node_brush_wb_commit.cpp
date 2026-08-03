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
    // The stroke Geometry, forwarded unchanged. The zone's group sync mirrors
    // every boundary slot, so sim_out needs BOTH the fed-back State AND a
    // Stroke Curves slot (matching sim_in). The stroke is static input, not
    // feedback state -- we just carry it across so the slot is filled. The
    // emitter caches it on first sight, so re-feeding it each frame is a no-op.
    b.add_input<Geometry>("Stroke Curves");

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
    Geometry stroke = params.get_input<Geometry>("Stroke Curves");

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
        nvrhi::BufferHandle pack_cb_buf;
        Ruzino::brush_upload_cb(
            rc, device, &pack_cb, sizeof(pack_cb), "wb_pack_cb", pack_cb_buf);
        Ruzino::brush_dispatch(
            rc,
            field->pack_program,
            { { "density", field->density },
              { "color_r", field->color_r },
              { "color_y", field->color_y },
              { "color_b", field->color_b } },
            { { "packed_out", field->packed_paint } },
            pack_cb_buf,
            grid_n3d);
        rc.destroy(pack_cb_buf);

        // Flush the pack dispatch before registering the buffer. brush_dispatch
        // only enqueues the compute work; without this flush the registry
        // would hand the renderer a buffer whose contents are still being
        // written by the GPU. The setPermanentBufferState + commitBarriers
        // pair transitions packed_paint from its UnorderedAccess initial state
        // (set at allocation, keepInitialState=true keeps it tracked there
        // after each sim command list) into ShaderResource, which is the state
        // the render rprim's RawBuffer_SRV expects. waitForIdle then drains
        // the GPU so the interleaved test architecture (sim tick → render
        // frame) has implicit synchronization without double-buffering.
        {
            auto flush_cmd = rc.create(CommandListDesc{});
            flush_cmd->open();
            flush_cmd->setPermanentBufferState(
                field->packed_paint.Get(),
                nvrhi::ResourceStates::ShaderResource);
            flush_cmd->commitBarriers();
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
        auto div_cpu = readback(field->divergence_buf, grid_n3d);
        double div_sum = 0.0;
        int div_count = 0;
        for (int i = 0; i < grid_n3d; ++i) {
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

    params.set_output("Max Divergence", max_div);
    params.set_output("Mean Divergence", mean_div);
    params.set_output("Total Density", static_cast<float>(tot_density));
    params.set_output("Total Color R", static_cast<float>(tot_r));
    params.set_output("Total Color Y", static_cast<float>(tot_y));
    params.set_output("Total Color B", static_cast<float>(tot_b));
    params.set_output("Particle Count", ptcl_count);
    params.set_output("Total Particle Mass", ptcl_mass);

    // DIAGNOSTIC: paint-mass accounting for the paper-faithful particle path.
    // density = grid paint mass (should be injected ONLY by particle
    // rasterize/transfer now that bristle direct-injection is removed).
    // particles = live particle count. ptcl_mass = summed particle mass.
    // If density stays ~0 the particle path isn't feeding the grid.
    spdlog::info(
        "wb_diag density={:.4f} color_b={:.4f} particles={} ptcl_mass={:.4f} "
        "ptcl_d_sum={:.4f}",
        tot_density,
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
