// node_brush_wb_bristle — Wetbrush BRISTLE liquid-transfer sub-step.
//
// Receives the paint field from brush_wb_deposit (which just ran the bristle
// deposit: spring -> constraint -> resample -> raster -> merge). This node owns
// the §5.1 bristle<->particle liquid transfer lifted 1:1 from brush_paint_sim
// ~1525-1612:
//   Pass 0 ABSORB: grid paint capacity -> sample_liquid (ping-pong _b out).
//   Pass 1 EMIT:   over-capacity sample liquid -> FLIP particles.
//
// The field is forwarded to brush_wb_fluid, which runs the particle cycle +
// fluid solve. The BristleSampleOutputs output carries handles to the same
// sample buffers for any readback consumer.

#include <memory>

#include "GCore/GOP.h"
#include "GCore/geom_payload.hpp"
#include "brush_sim_common.hpp"  // WetbrushSimState, WetbrushZoneState, BristleSampleOutputs, brush_* helpers
#include "geom_node_base.h"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(brush_wb_bristle)
{
    b.add_input<Ruzino::WetbrushZoneState>("State");
    b.add_input<float>("Brush Radius").default_val(0.02f).min(0.001f).max(0.5f);

    b.add_output<Ruzino::WetbrushZoneState>("State");
    // Carries handles to the same sample buffers for readback consumers.
    b.add_output<Ruzino::BristleSampleOutputs>("Bristle Samples");
}

NODE_EXECUTION_FUNCTION(brush_wb_bristle)
{
    using Ruzino::WetbrushSimState;
    using Ruzino::WetbrushZoneState;

    WetbrushZoneState zs = params.get_input<WetbrushZoneState>("State");
    auto& field = zs.state;
    float brush_radius = params.get_input<float>("Brush Radius");

    auto& rc = get_resource_allocator();
    auto device = RHI::get_device();
    auto payload = params.get_global_payload<GeomPayload>();

    Ruzino::BristleSampleOutputs samples{};
    if (field) {
        samples.sample_pos = field->sample_pos;
        samples.sample_color = field->sample_color;
        samples.sample_frame = field->sample_frame;
    }

    if (!field || !payload.is_simulating || !field->particles_initialized) {
        params.set_output("State", zs);
        params.set_output("Bristle Samples", samples);
        return true;
    }

    const int WIN_XY =
        std::min(WetbrushSimState::WIN_ALLOC_XY, field->grid_res);
    const int WIN_Z = field->grid_res_z;
    const float cell_sz =
        field->grid_paper / static_cast<float>(field->grid_res);
    const int Nb = WetbrushSimState::NUM_BRISTLES;
    const int S = WetbrushSimState::SAMPLES_PER_BRISTLE;
    const int max_ptcl = WetbrushSimState::MAX_PARTICLES;

    // ======================================================================
    // §5.1 Bristle-particle liquid transfer (brush_paint_sim ~1531-1612).
    // ABSORB (paint supply -> sample using Eq.12/13 capacity) then EMIT
    // (sample -> particles). Ping-pong on sample_liquid.
    // ======================================================================
    Ruzino::BristleLiquidConstants blc = {};
    blc.num_bristles = Nb;
    blc.samples_per_bristle = S;
    blc.mu = 0.5f;
    blc.M_max = 2.0f;
    blc.M_min = 0.1f;
    blc.rho_0 = 1e3f;
    blc.eps_emit = 0.1f;
    blc.max_emit_per_step = 10;
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
    blc.window_origin_x = field->win_origin_x;
    blc.window_origin_y = field->win_origin_y;
    blc.window_origin_z = 0;
    blc.window_size_x = WIN_XY;
    blc.window_size_z = WIN_Z;

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

    // Pass 0: ABSORB (sample_liquid SRV -> sample_liquid_b UAV)
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
        { { "sample_liquid_out", field->sample_liquid_b },
          { "sample_supply", field->sample_supply } },
        liquid_cb,
        Nb * S);
    std::swap(field->sample_liquid, field->sample_liquid_b);

    // Pass 1: EMIT (sample -> particles, hemisphere pattern).
    //
    // Do NOT reset the counter here. Paper §4.3: "Our system needs 200K to 1M
    // particles" that survive across frames. The counter is owned by the
    // fluid node's compact step, which resets it to 0 and rewrites only the
    // alive particles into [0, count). EMIT appends past that count via
    // InterlockedAdd. The previous reset here zeroed the counter every frame,
    // destroying the survivors the compact step had preserved and breaking
    // particle persistence — new particles overwrote slot 0+ and the
    // §5.1-emitted paint never accumulated.
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
          { "sample_supply", field->sample_supply },
          { "ptcl_counter", field->ptcl_counter },
          { "ptcl_pos_out", field->ptcl_pos },
          { "ptcl_vel_out", field->ptcl_vel },
          { "ptcl_color_out", field->ptcl_color },
          { "ptcl_alive_out", field->ptcl_alive } },
        liquid_cb,
        Nb * S);
    std::swap(field->sample_liquid, field->sample_liquid_b);

    rc.destroy(liquid_cb);

    samples.sample_pos = field->sample_pos;
    samples.sample_color = field->sample_color;
    samples.sample_frame = field->sample_frame;
    params.set_output("State", zs);
    params.set_output("Bristle Samples", samples);
    return true;
}

NODE_DECLARATION_UI(brush_wb_bristle);

NODE_DECLARATION_ALWAYS_DIRTY(brush_wb_bristle);

NODE_DEF_CLOSE_SCOPE
