// node_brush_wb_commit — Wetbrush COMMIT + OUTPUT sub-step.
//
// Receives the final field from brush_wb_fluid and:
//   - commits the live 3D window into the persistent 2D canvas layer
//     (canvas_commit shader, brush_paint_sim ~2712-2760);
//   - reads back fidelity statistics into the debug output ports (~2568-2710);
//   - emits the Paint Particles geometry (one point per painted canvas cell,
//     ~2762-2855) so downstream consumers (write_usd, render) see the paint.
//
// The field (carrying the committed canvas) is forwarded on the "State" output
// so the zone feeds it back simulation_out -> simulation_in for the next frame.

#include <memory>

#include "GCore/Components/PointsComponent.h"
#include "GCore/GOP.h"
#include "GCore/geom_payload.hpp"
#include "GPUContext/compute_context.hpp"  // CommandListDesc
#include "RHI/ResourceManager/resource_allocator.hpp"
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

    const int WIN_XY =
        std::min(WetbrushSimState::WIN_ALLOC_XY, field->grid_res);
    const int WIN_Z = field->grid_res_z;
    const int win_n3d = WIN_XY * WIN_XY * WIN_Z;
    const float cell_sz =
        field->grid_paper / static_cast<float>(field->grid_res);
    const int max_ptcl = WetbrushSimState::MAX_PARTICLES;

    // Ensure the canvas_commit shader exists (deposit usually compiles it
    // first, but commit may run before any deposit on a pen-up-only sequence).
    if (!field->canvas_commit_program)
        field->canvas_commit_program =
            Ruzino::brush_compile_shader(rc, "canvas_commit.slang");

    // ======================================================================
    // FINAL COMMIT: flush the live 3D window into the 2D canvas layer
    // (brush_paint_sim ~2718-2760). Every frame bakes the current window so
    // the output reflects ALL painted regions, not just the live brush window.
    // ======================================================================
    {
        Ruzino::SimConstants commit_cb = {};
        commit_cb.res = field->grid_res;
        commit_cb.cell_size = cell_sz;
        commit_cb.paper_size = field->grid_paper;
        commit_cb.res_z = WIN_Z;
        commit_cb.height_extent = field->grid_height;
        commit_cb.grid_center_z = field->grid_center_z;
        commit_cb.window_origin_x = field->win_origin_x;
        commit_cb.window_origin_y = field->win_origin_y;
        commit_cb.window_origin_z = 0;
        commit_cb.window_size_x = WIN_XY;
        commit_cb.window_size_y = WIN_XY;
        commit_cb.window_size_z = WIN_Z;

        nvrhi::BufferHandle commit_cb_buf;
        Ruzino::brush_upload_cb(
            rc,
            device,
            &commit_cb,
            sizeof(commit_cb),
            "wb_final_commit_cb",
            commit_cb_buf);
        ProgramVars cv(rc, field->canvas_commit_program);
        cv["cb"] = commit_cb_buf.Get();
        cv["density"] = field->density.Get();
        cv["color_r"] = field->color_r.Get();
        cv["color_y"] = field->color_y.Get();
        cv["color_b"] = field->color_b.Get();
        cv["wetness"] = field->wetness.Get();
        cv["canvas_density"] = field->canvas_density.Get();
        cv["canvas_color_r"] = field->canvas_color_r.Get();
        cv["canvas_color_y"] = field->canvas_color_y.Get();
        cv["canvas_color_b"] = field->canvas_color_b.Get();
        cv["canvas_wetness"] = field->canvas_wetness.Get();
        cv.finish_setting_vars();
        ComputeContext cctx(rc, cv);
        cctx.finish_setting_pso();
        cctx.begin();
        cctx.dispatch({}, cv, WIN_XY * WIN_XY, 256);
        cctx.finish();
        rc.destroy(commit_cb_buf);
    }

    // ======================================================================
    // READBACK: window field totals + divergence stats + particle count/mass
    // (brush_paint_sim ~2568-2710).
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

    auto density_cpu = readback(field->density, win_n3d);
    auto cr_cpu = readback(field->color_r, win_n3d);
    auto cy_cpu = readback(field->color_y, win_n3d);
    auto cb_cpu = readback(field->color_b, win_n3d);

    float max_div = 0.0f, mean_div = 0.0f;
    {
        auto div_cpu = readback(field->divergence_buf, win_n3d);
        double div_sum = 0.0;
        int div_count = 0;
        for (int i = 0; i < win_n3d; ++i) {
            float ad = std::fabs(div_cpu[i]);
            max_div = std::max(max_div, ad);
            div_sum += ad;
            ++div_count;
        }
        mean_div =
            div_count > 0 ? static_cast<float>(div_sum / div_count) : 0.0f;
    }

    double tot_density = 0.0, tot_r = 0.0, tot_y = 0.0, tot_b = 0.0;
    for (int i = 0; i < win_n3d; ++i) {
        tot_density += density_cpu[i];
        tot_r += cr_cpu[i];
        tot_y += cy_cpu[i];
        tot_b += cb_cpu[i];
    }

    int ptcl_count = 0;
    float ptcl_mass = 0.0f;
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
            std::vector<float> cols(n * STRIDE);
            std::vector<uint32_t> alive(n);
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
                field->ptcl_color, sizeof(float) * STRIDE, cols.data());
            read_structured(field->ptcl_alive, sizeof(uint32_t), alive.data());
            for (int i = 0; i < n; ++i)
                if (alive[i] != 0)
                    ptcl_mass += cols[i * STRIDE + 3];
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
    // OUTPUT: read the 2D canvas layer and emit a point per painted cell
    // (brush_paint_sim ~2762-2855).
    // ======================================================================
    int n2d = field->grid_res * field->grid_res;
    auto readback_2d = [&](nvrhi::BufferHandle buf) -> std::vector<float> {
        return readback(buf, n2d);
    };
    auto cdensity = readback_2d(field->canvas_density);
    auto ccr = readback_2d(field->canvas_color_r);
    auto ccy = readback_2d(field->canvas_color_y);
    auto ccb = readback_2d(field->canvas_color_b);

    auto [particles, pts] = make_particles();
    std::vector<glm::vec3> out_pts;
    std::vector<glm::vec3> out_colors;
    std::vector<float> out_widths;

    constexpr float threshold = 0.001f;
    constexpr float rgb_white_cutoff = 0.9f;
    float canvas_floor_z = field->grid_center_z - field->grid_height * 0.5f;
    for (int y = 0; y < field->grid_res; ++y) {
        for (int x = 0; x < field->grid_res; ++x) {
            int gi = y * field->grid_res + x;
            if (cdensity[gi] <= threshold)
                continue;
            float d = cdensity[gi];
            float r = ccr[gi] / (d + 1e-8f);
            float yy = ccy[gi] / (d + 1e-8f);
            float b = ccb[gi] / (d + 1e-8f);
            r = std::min(std::max(r, 0.0f), 1.0f);
            yy = std::min(std::max(yy, 0.0f), 1.0f);
            b = std::min(std::max(b, 0.0f), 1.0f);
            float rm = 1 - r, ym = 1 - yy, bm = 1 - b;
            glm::vec3 rgb = rm * ym * bm * glm::vec3(1, 1, 1) +
                            r * ym * bm * glm::vec3(1, 0, 0) +
                            rm * yy * bm * glm::vec3(1, 1, 0) +
                            rm * ym * b * glm::vec3(0.163f, 0.373f, 0.6f) +
                            r * yy * bm * glm::vec3(1, 0.5f, 0) +
                            r * ym * b * glm::vec3(0.5f, 0, 0.5f) +
                            rm * yy * b * glm::vec3(0, 0.66f, 0.2f) +
                            r * yy * b * glm::vec3(0.2f, 0.094f, 0.029f);
            if (std::min({ rgb.r, rgb.g, rgb.b }) >= rgb_white_cutoff)
                continue;

            float gx = (x + 0.5f) * cell_sz - field->grid_paper * 0.5f;
            float gy = (y + 0.5f) * cell_sz - field->grid_paper * 0.5f;
            float gz =
                canvas_floor_z + cell_sz * std::min(std::sqrt(d * 10.0f), 2.0f);
            out_pts.push_back(
                glm::vec3(
                    gx + field->grid_center.x, gy + field->grid_center.y, gz));
            out_colors.push_back(rgb);
            out_widths.push_back(cell_sz);
        }
    }

    pts->set_vertices(out_pts);
    pts->set_display_color(out_colors);
    pts->set_width(out_widths);

    params.set_output("Paint Particles", std::move(particles));
    params.set_output("State", zs);
    params.set_output("Stroke Curves", stroke);
    return true;
}

NODE_DECLARATION_UI(brush_wb_commit);

NODE_DECLARATION_ALWAYS_DIRTY(brush_wb_commit);

NODE_DEF_CLOSE_SCOPE
