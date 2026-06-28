// node_brush_wb_deposit — Wetbrush DEPOSIT sub-step (streaming zone chain).
//
// Topology (the simulation zone carries ONE typed value: WetbrushFrame, which
// bundles the per-frame BrushPoint + the shared WetbrushSimState):
//
//   mock_stroke --Stroke Curves--> [ simulation_in ]
//   [ simulation_in ] --Stroke Curves--> mock_point_emitter   (zone interior)
//   mock_point_emitter --BrushPoint--> brush_wb_deposit "Brush Point" (interior
//   edge, NOT the fed-back frame) brush_wb_deposit --WetbrushFrame-->
//   brush_wb_bristle --WetbrushFrame-->
//     brush_wb_fluid --WetbrushFrame--> brush_wb_commit
//   brush_wb_commit --WetbrushFrame--> [ simulation_out ]   (fed back)
//   [ simulation_out ] --WetbrushFrame--> write_usd
//
// This node owns the FIRST physics stage: position the active window on the
// brush and deposit paint (density + RYB color) from the per-frame BrushPoint.
// On the init frame (is_simulating == false) it allocates the shared
// WetbrushSimState (live fields + canvas layer + control bookkeeping) and
// bundles it into the outgoing WetbrushFrame, so downstream nodes + next
// frame's feedback receive a valid state.
//
// STATUS: buffer allocation lifted from brush_paint_sim (~533-628), restricted
// to the WetbrushSimState subset. The window-position + deposit dispatch
// physics is a Stage 3 TODO.

#include <algorithm>
#include <memory>
#include <vector>

#include "GCore/GOP.h"
#include "GCore/geom_payload.hpp"
#include "GPUContext/compute_context.hpp"  // CommandListDesc
#include "RHI/ResourceManager/resource_allocator.hpp"
#include "brush_sim_common.hpp"  // BrushPoint, WetbrushSimState, WetbrushFrame, brush_* helpers
#include "geom_node_base.h"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(brush_wb_deposit)
{
    // Per-frame brush sample from the emitter (interior edge, fresh each
    // frame).
    b.add_input<Ruzino::BrushPoint>("Brush Point");
    // The fed-back shared state (bundled in WetbrushFrame by the zone). On the
    // init frame this is empty/absent and we allocate it.
    b.add_input<Ruzino::WetbrushFrame>("Frame");
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
    // RYB ink color. Optional because vec3 sockets can't carry a default_val
    // through serialization; the exec falls back to red when unwired.
    b.add_input<glm::vec3>("Ink Color").optional(true);

    // Outgoing frame: bundles the per-frame BrushPoint + (allocated/updated)
    // SimState for the rest of the chain + next-frame feedback.
    b.add_output<Ruzino::WetbrushFrame>("Frame");
    // height_field written here, read by brush_wb_bristle (2-node field).
    b.add_output<nvrhi::BufferHandle>("Height Field");
}

NODE_EXECUTION_FUNCTION(brush_wb_deposit)
{
    using Ruzino::WetbrushFrame;
    using Ruzino::WetbrushSimState;

    Ruzino::BrushPoint bp = params.get_input<Ruzino::BrushPoint>("Brush Point");
    WetbrushFrame frame = params.get_input<Ruzino::WetbrushFrame>("Frame");
    auto& simstate = frame.state;  // shared_ptr<WetbrushSimState>
    int resolution = params.get_input<int>("Resolution");
    int resolution_z = params.get_input<int>("Resolution Z");
    float paper_size = params.get_input<float>("Paper Size");
    glm::vec2 canvas_center_xy(
        params.get_input<float>("Canvas Center X"),
        params.get_input<float>("Canvas Center Y"));
    float canvas_z = params.get_input<float>("Canvas Z");
    float canvas_height_in = params.get_input<float>("Canvas Height");

    auto& rc = get_resource_allocator();
    auto device = RHI::get_device();
    auto payload = params.get_global_payload<GeomPayload>();
    float dt = payload.delta_time > 0.0f ? payload.delta_time : (1.0f / 60.0f);

    // --- Allocate the shared SimState (live fields + canvas) when needed ---
    // On the init frame frame.state is null; we allocate it. Also realloc if
    // the resolution changed. Lifted from brush_paint_sim ~463-628.
    bool need_alloc = !simstate || !simstate->center_initialized ||
                      simstate->grid_alloc_res != resolution ||
                      simstate->grid_alloc_res_z != resolution_z;

    if (need_alloc) {
        if (!simstate) {
            simstate = std::make_shared<WetbrushSimState>();
        }
        int rz = resolution_z > 0 ? resolution_z : 32;
        float height = canvas_height_in > 1e-6f
                           ? canvas_height_in
                           : paper_size * static_cast<float>(rz) /
                                 static_cast<float>(resolution);
        simstate->grid_center = canvas_center_xy;
        simstate->grid_height = height;
        simstate->grid_center_z = canvas_z + height * 0.5f;
        simstate->grid_paper = paper_size;
        simstate->grid_res = resolution;
        simstate->grid_res_z = rz;
        simstate->center_initialized = true;
        simstate->grid_alloc_res = resolution;
        simstate->grid_alloc_res_z = rz;
        simstate->win_alloc_z = rz;
        simstate->win_origin_set = false;
        simstate->deposited_count = 0;
        simstate->last_sim_time = -1.0f;
        simstate->has_prev_brush_pos = false;
        simstate->prev_brush_vel = glm::vec3(0.0f);
        simstate->prev_angular_vel = glm::vec3(0.0f);

        int win_xy = std::min(WetbrushSimState::WIN_ALLOC_XY, resolution);
        int alloc_win_n3d = win_xy * win_xy * rz;
        int n2d = resolution * resolution;

        auto safe_destroy = [&](nvrhi::BufferHandle& h) {
            if (h) {
                rc.destroy(h);
                h = nullptr;
            }
        };
        safe_destroy(simstate->density);
        safe_destroy(simstate->color_r);
        safe_destroy(simstate->color_y);
        safe_destroy(simstate->color_b);
        safe_destroy(simstate->wetness);
        safe_destroy(simstate->oil_density);
        safe_destroy(simstate->canvas_density);
        safe_destroy(simstate->canvas_color_r);
        safe_destroy(simstate->canvas_color_y);
        safe_destroy(simstate->canvas_color_b);
        safe_destroy(simstate->canvas_wetness);

        simstate->density =
            Ruzino::brush_create_field_buffer(rc, alloc_win_n3d, "wb_density");
        simstate->color_r =
            Ruzino::brush_create_field_buffer(rc, alloc_win_n3d, "wb_color_r");
        simstate->color_y =
            Ruzino::brush_create_field_buffer(rc, alloc_win_n3d, "wb_color_y");
        simstate->color_b =
            Ruzino::brush_create_field_buffer(rc, alloc_win_n3d, "wb_color_b");
        simstate->wetness =
            Ruzino::brush_create_field_buffer(rc, alloc_win_n3d, "wb_wetness");
        simstate->oil_density = Ruzino::brush_create_field_buffer(
            rc, alloc_win_n3d, "wb_oil_density");
        simstate->canvas_density =
            Ruzino::brush_create_field_buffer(rc, n2d, "wb_canvas_density");
        simstate->canvas_color_r =
            Ruzino::brush_create_field_buffer(rc, n2d, "wb_canvas_color_r");
        simstate->canvas_color_y =
            Ruzino::brush_create_field_buffer(rc, n2d, "wb_canvas_color_y");
        simstate->canvas_color_b =
            Ruzino::brush_create_field_buffer(rc, n2d, "wb_canvas_color_b");
        simstate->canvas_wetness =
            Ruzino::brush_create_field_buffer(rc, n2d, "wb_canvas_wetness");

        std::vector<float> zeros3d(alloc_win_n3d, 0.0f);
        std::vector<float> zeros2d(n2d, 0.0f);
        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        for (auto* buf : { &simstate->density,
                           &simstate->color_r,
                           &simstate->color_y,
                           &simstate->color_b,
                           &simstate->wetness,
                           &simstate->oil_density }) {
            cmd->writeBuffer(
                *buf, zeros3d.data(), alloc_win_n3d * sizeof(float));
        }
        for (auto* buf : { &simstate->canvas_density,
                           &simstate->canvas_color_r,
                           &simstate->canvas_color_y,
                           &simstate->canvas_color_b,
                           &simstate->canvas_wetness }) {
            cmd->writeBuffer(*buf, zeros2d.data(), n2d * sizeof(float));
        }
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        rc.destroy(cmd);

        spdlog::info(
            "brush_wb_deposit: allocated SimState {}x{}x{} (win {}x{}x{}), "
            "paper={:.3f}, height={:.3f}",
            resolution,
            resolution,
            rz,
            win_xy,
            win_xy,
            rz,
            paper_size,
            height);
    }

    // Stamp the per-frame brush sample into the outgoing frame.
    frame.bp = bp;

    if (!payload.is_simulating || !bp.active) {
        params.set_output("Frame", frame);
        params.set_output("Height Field", nvrhi::BufferHandle{});
        return true;
    }

    spdlog::info(
        "brush_wb_deposit: advance pos=({:.3f},{:.3f},{:.3f}) t={:.3f} "
        "dt={:.4f} stroke_start={} (window+deposit physics TODO)",
        bp.pos.x,
        bp.pos.y,
        bp.pos.z,
        bp.time,
        dt,
        bp.stroke_start);

    if (bp.stroke_start) {
        simstate->has_prev_brush_pos = false;
        simstate->prev_brush_vel = glm::vec3(0.0f);
        simstate->prev_angular_vel = glm::vec3(0.0f);
    }
    simstate->prev_brush_pos = bp.pos;
    simstate->has_prev_brush_pos = true;

    params.set_output("Frame", frame);
    params.set_output("Height Field", nvrhi::BufferHandle{});
    return true;
}

NODE_DECLARATION_UI(brush_wb_deposit);

NODE_DECLARATION_ALWAYS_DIRTY(brush_wb_deposit);

NODE_DEF_CLOSE_SCOPE
