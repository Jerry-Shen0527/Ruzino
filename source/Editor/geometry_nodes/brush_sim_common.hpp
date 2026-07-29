// Common helpers, shader-constant structs, and the shared simulation-state
// types for the streaming Wetbrush brush-paint nodes.
//
// The streaming pipeline (brush_wb_deposit -> brush_wb_bristle ->
// brush_wb_fluid -> brush_wb_commit, wired as a simulation-zone chain)
// includes this header so all four nodes share the EXACT same buffer-layout,
// shader-binding, and constant-buffer conventions — no duplicated logic that
// can drift.
//
// Everything here lives in namespace Ruzino. The helper functions are
// marked `inline` so multiple .cpp files can include this header without
// ODR violations.

#pragma once

#include <string>
#include <vector>

#include "GCore/algorithms/gpu_geometry.h"
#include "GPUContext/compute_context.hpp"
#include "RHI/ResourceManager/resource_allocator.hpp"
#include "RHI/shaderCompiler.h"
#include "glm/glm.hpp"
#include "nvrhi/nvrhi.h"
#include "spdlog/spdlog.h"

namespace Ruzino {

// ============================================================
// Shader directory
// ============================================================
inline std::string brush_shader_dir()
{
    return SlangShaderCompiler::get_shader_dir(ShaderDirType::GeomNodes)
               .string() +
           "/BrushSimulation/shaders/";
}

// ============================================================
// Buffer factories
// ============================================================
inline nvrhi::BufferHandle
brush_create_field_buffer(ResourceAllocator& rc, int n, const char* debug_name)
{
    return rc.create(
        nvrhi::BufferDesc{}
            .setByteSize(n * sizeof(float))
            .setStructStride(sizeof(float))
            .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
            .setKeepInitialState(true)
            .setCanHaveUAVs(true)
            .setCanHaveTypedViews(true)
            .setDebugName(debug_name));
}

inline nvrhi::BufferHandle brush_create_typed_buffer(
    ResourceAllocator& rc,
    int count,
    int stride,
    const char* debug_name)
{
    return rc.create(
        nvrhi::BufferDesc{}
            .setByteSize(count * stride)
            .setStructStride(stride)
            .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
            .setKeepInitialState(true)
            .setCanHaveUAVs(true)
            .setCanHaveTypedViews(true)
            .setDebugName(debug_name));
}

inline nvrhi::BufferHandle brush_create_byte_buffer(
    ResourceAllocator& rc,
    int size_bytes,
    const char* debug_name)
{
    return rc.create(
        nvrhi::BufferDesc{}
            .setByteSize(size_bytes)
            .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
            .setKeepInitialState(true)
            .setCanHaveUAVs(true)
            .setDebugName(debug_name));
}

// ============================================================
// Shader compile + dispatch
// ============================================================
inline ProgramHandle brush_compile_shader(
    ResourceAllocator& rc,
    const std::string& filename)
{
    ProgramDesc desc;
    desc.shaderType = nvrhi::ShaderType::Compute;
    desc.set_path(brush_shader_dir() + filename);
    desc.set_entry_name("main");
    auto prog = rc.create(desc);
    if (!prog->get_error_string().empty()) {
        spdlog::error(
            "Failed to compile {}: {}", filename, prog->get_error_string());
        rc.destroy(prog);
        return nullptr;
    }
    return prog;
}

inline void brush_dispatch(
    ResourceAllocator& rc,
    ProgramHandle prog,
    const std::vector<std::pair<std::string, nvrhi::BufferHandle>>& srvs,
    const std::vector<std::pair<std::string, nvrhi::BufferHandle>>& uavs,
    nvrhi::BufferHandle cb,
    int total_threads)
{
    ProgramVars vars(rc, prog);
    if (cb)
        vars["cb"] = cb.Get();
    for (auto& [name, buf] : srvs)
        vars[name.c_str()] = buf.Get();
    for (auto& [name, buf] : uavs)
        vars[name.c_str()] = buf.Get();
    vars.finish_setting_vars();

    ComputeContext ctx(rc, vars);
    ctx.finish_setting_pso();
    ctx.begin();
    ctx.dispatch({}, vars, total_threads, 256);
    ctx.finish();
}

inline void brush_upload_cb(
    ResourceAllocator& rc,
    nvrhi::IDevice* device,
    const void* data,
    size_t size,
    const char* debug_name,
    nvrhi::BufferHandle& out_buf)
{
    if (out_buf)
        rc.destroy(out_buf);
    out_buf = rc.create(
        nvrhi::BufferDesc{}
            .setByteSize(size)
            .setIsConstantBuffer(true)
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true)
            .setDebugName(debug_name));
    auto cmd = rc.create(CommandListDesc{});
    cmd->open();
    cmd->writeBuffer(out_buf, data, size);
    cmd->close();
    device->executeCommandList(cmd);
    device->waitForIdle();
    rc.destroy(cmd);
}

inline void brush_reset_counter(
    ResourceAllocator& rc,
    nvrhi::IDevice* device,
    nvrhi::BufferHandle& counter_buf)
{
    uint32_t zero = 0;
    auto cmd = rc.create(CommandListDesc{});
    cmd->open();
    cmd->writeBuffer(counter_buf, &zero, sizeof(uint32_t));
    cmd->close();
    device->executeCommandList(cmd);
    device->waitForIdle();
    rc.destroy(cmd);
}

// ============================================================
// Shader-constant structs — must match common.slangh structs
// ============================================================
struct SimConstants {
    int res;           // Grid XY resolution N
    float cell_size;   // paper_size / res
    float paper_size;  // Total paper extent (XY)
    float dt;          // Time step

    float viscosity;     // Viscosity coefficient (base, × oil_density per-cell)
    float diffusion;     // Diffusion rate
    float drying_rate;   // Drying rate
    float brush_radius;  // Brush radius (world units)

    float ink_amount;  // Ink deposit amount
    int num_vertices;  // Number of NEW curve vertices to deposit
    float center_x;    // Grid center X (world space)
    float center_y;    // Grid center Y (world space)

    float center_z;          // Grid center Z (world space)
    float effective_radius;  // brush_radius (world units)
    int jacobi_mode;         // 0 = diffuse, 1 = pressure
    float jacobi_alpha;      // alpha for Jacobi: dt*rate*N^2 (diffuse) or 1.0
                             // (pressure)

    int res_z;            // Grid Z (height) resolution D
    float height_extent;  // Total height extent in world units
    float grid_center_z;  // Z center

    int window_origin_x;
    int window_origin_y;
    int window_origin_z;
    int window_size_x;
    int window_size_y;
    int window_size_z;

    float oil_density_base;
    float _pad0, _pad1, _pad2;
};

struct BristleConstants {
    int num_bristles;
    int verts_per_bristle;
    int samples_per_bristle;
    float beta_B;

    float dt;
    float brush_pos_x, brush_pos_y, brush_pos_z;
    float brush_vel_x, brush_vel_y, brush_vel_z;
    float brush_angular_vel_x, brush_angular_vel_y, brush_angular_vel_z;
    float brush_rotation;

    float brush_radius;
    float spring_k;
    float damping;
    int grid_res;
    float cell_size;
    float paper_size;
    float grid_center_x, grid_center_y;
    // 3D extension
    int grid_res_z;
    float height_extent;
    float grid_center_z;
    // Frame-origin acceleration (a_B) and angular acceleration (ω̇) for the
    // Eq.2 non-inertial terms, finite-differenced on the host.
    float brush_accel_x, brush_accel_y, brush_accel_z;
    float brush_angular_accel_x, brush_angular_accel_y, brush_angular_accel_z;
    // Canvas contact (§4.1 splaying): pressure drives footprint spread;
    // canvas_z is the (impenetrable) paint-volume floor.
    float brush_pressure;
    float canvas_z;
    // Active-window origin/size (Wetbrush §4.2) — 3D buffers are window-sized.
    int window_origin_x;
    int window_origin_y;
    int window_origin_z;
    int window_size_x;
    int window_size_z;
    // Brush SWEEP (anti-zebra): previous-frame brush center (grid-relative).
    // bristle_rasterize splats each sample along prev→current so fast drag
    // deposits a continuous band instead of isolated blobs per frame.
    // has_prev_brush_pos != 0 means the prev position is valid (first frame
    // has no motion to sweep). sweep_steps is the number of sub-positions
    // along the prev→current arc that the rasterize shader will splat at —
    // the dispatch thread count is Nb*S*sweep_steps, so each (sample, step)
    // gets its own thread (no per-thread loop, no MAX_STEPS cap).
    float prev_brush_pos_x, prev_brush_pos_y, prev_brush_pos_z;
    int has_prev_brush_pos;
    int sweep_steps;  // >=1; 1 means no sweep (single-point splat)
    float _sweep_pad0, _sweep_pad1;
};

struct ParticleConstants {
    int max_particles;
    float dt;
    float D0;
    float friction_delta;

    float flip_gamma;
    int grid_res;
    int grid_res_z;
    float cell_size;
    float paper_size;
    float height_extent;
    float grid_center_x, grid_center_y, grid_center_z;
    float brush_pos_x, brush_pos_y, brush_pos_z;
    float brush_radius;
    int emit_mode;
    float D1;
    int num_bristles;
    int samples_per_bristle;
    float brush_accel_x, brush_accel_y, brush_accel_z;
    float brush_angular_accel_x, brush_angular_accel_y, brush_angular_accel_z;
    int window_origin_x;
    int window_origin_y;
    int window_origin_z;
    int window_size_x;
    int window_size_z;
};

struct BristleLiquidConstants {
    int num_bristles;
    int samples_per_bristle;
    float mu;
    float M_max;
    float M_min;
    float rho_0;
    float eps_emit;
    int max_emit_per_step;
    int grid_res;
    int grid_res_z;
    float cell_size;
    float paper_size;
    float height_extent;
    float grid_center_x, grid_center_y, grid_center_z;
    float D0;
    int max_particles;
    int window_origin_x;
    int window_origin_y;
    int window_origin_z;
    int window_size_x;
    int window_size_z;
};

struct ConstraintModeCB {
    int mode;
    float pad[3];
};

// ============================================================
// BrushPoint — the per-frame brush sample carried across the graph.
//
// Produced by node_mock_point_emitter (one per simulation frame, replaying
// the captured trajectory) and consumed by node_brush_wb_deposit (which
// advances the paint state by one step from this single sample). Defined
// here in the shared header so both nodes — compiled into separate .dlls —
// see the SAME type identity and can pass it through a socket.
//
// Plain aggregate: auto-registered with entt::meta by get_socket_type<T>()
// on first use, the same way Geometry / Eigen::MatrixXd socket types work.
// ============================================================
struct BrushPoint {
    glm::vec3 pos{ 0.0f };      // world position of the brush
    float time = 0.0f;          // stroke-local time (seconds since pen-down)
    bool active = false;        // pen is currently down
    bool stroke_start = false;  // first point of a new stroke (pen-down edge)
    glm::vec3 color{ 1.0f, 0.0f, 0.0f };  // RYB ink color (from the trajectory)
};

// ============================================================
// WetbrushSimState — the SHARED cross-node + cross-frame state for the
// streaming simulation-zone brush chain (brush_wb_deposit ->
// brush_wb_bristle -> brush_wb_fluid -> brush_wb_commit).
//
// This struct carries the FULL persistent buffer set. An earlier "lean subset"
// idea (only density/color/wetness/oil_density/canvas) was physically wrong:
// the Stable Fluids solver's velocity field, the bristle spring positions, and
// the FLIP/PIC particles MUST persist frame-to-frame, otherwise momentum,
// bristle dynamics and particle mass reset every tick and the result bears no
// resemblance to a real stroke. "node-local + auto-recycled" cannot work for
// state that accumulates across the whole stroke. Fix it at the source: carry
// everything that persists.
//
// It travels as a shared_ptr<WetbrushSimState> socket value (wrapped by
// WetbrushZoneState) across the 4 sub-step nodes (zero-copy: GPU handles are
// refcounted shared_ptrs) and is fed back through simulation_out ->
// simulation_in, instead of living in a single Node::storage. Because the
// framework auto-registers socket types, no manual entt::meta registration is
// needed.
// ============================================================
struct WetbrushSimState {
    static constexpr bool has_storage = false;

    // --- Global 3D fluid grid (allocated at gridRes × gridRes × gridRes_z;
    // the persistent paint store. Paper §4.2: a large 3D grid stores all
    // cells; simulation runs only inside a brush-centered active window, the
    // rest of the cells keep their values) ---
    nvrhi::BufferHandle density, density_tmp;
    nvrhi::BufferHandle color_r, color_y, color_b;
    // Each color channel needs its OWN ping-pong tmp: advect/diffuse swap
    // (f, tmp) per channel, and a shared tmp aliases the channels' buffers
    // across frames (the physical buffer behind color_r rotates each frame),
    // which at low-density edge cells produces a perfect even/odd-frame color
    // flip (the post-stroke red/blue flicker).
    nvrhi::BufferHandle color_r_tmp, color_y_tmp, color_b_tmp;
    nvrhi::BufferHandle vel_x, vel_x_tmp;
    nvrhi::BufferHandle vel_y, vel_y_tmp;
    nvrhi::BufferHandle vel_z, vel_z_tmp;
    nvrhi::BufferHandle wetness, wetness_tmp;
    nvrhi::BufferHandle oil_density, oil_density_tmp;
    nvrhi::BufferHandle height_field;
    nvrhi::BufferHandle pressure_a, pressure_b;
    nvrhi::BufferHandle divergence_buf;

    // --- Bristle chain state (spring positions + samples + liquid) ---
    // NUM_BRISTLES: paper §6 says brushes contain "40 to 600 bristles". 80 was
    // the lower end and left the XY footprint sparsely sampled at 1024 grid
    // (footprint ~20 cells, 80 roots → visible grain in the rasterized density).
    // 200 helped but close-up views still show grain. Paper's own smoothness
    // source is dense bristle sampling (up to 600×128 = 76800 samples), so use
    // the paper's upper bound — fully paper-faithful, no XY splat kernel (which
    // the paper doesn't specify).
    static constexpr int NUM_BRISTLES = 600;
    static constexpr int VERTS_PER_BRISTLE = 10;
    static constexpr int SAMPLES_PER_BRISTLE = 128;
    static constexpr int BRISTLE_VERTEX_STRIDE = sizeof(float) * 4 * 2;

    nvrhi::BufferHandle bristle_data;
    nvrhi::BufferHandle sample_pos;
    nvrhi::BufferHandle sample_vel;
    nvrhi::BufferHandle sample_color;
    nvrhi::BufferHandle sample_frame;
    nvrhi::BufferHandle lambda_buf;
    nvrhi::BufferHandle sample_liquid;
    nvrhi::BufferHandle sample_liquid_b;
    nvrhi::BufferHandle sample_supply;
    nvrhi::BufferHandle bristle_input_color_buf;

    // --- Bristle/particle accumulation grids (window-sized, reused as scratch
    // each sub-step, same as the monolith) ---
    nvrhi::BufferHandle bristle_density;
    nvrhi::BufferHandle bristle_vel_x;
    nvrhi::BufferHandle bristle_vel_y;
    nvrhi::BufferHandle bristle_vel_z;
    nvrhi::BufferHandle bristle_color_r;
    nvrhi::BufferHandle bristle_color_y;
    nvrhi::BufferHandle bristle_color_b;

    // --- FLIP/PIC particle buffers ---
    static constexpr int MAX_PARTICLES = 262144;

    nvrhi::BufferHandle ptcl_pos;
    nvrhi::BufferHandle ptcl_vel;
    nvrhi::BufferHandle ptcl_color;
    nvrhi::BufferHandle ptcl_alive;
    nvrhi::BufferHandle ptcl_counter;
    nvrhi::BufferHandle ptcl_density;
    nvrhi::BufferHandle ptcl_vel_x;
    nvrhi::BufferHandle ptcl_vel_y;
    nvrhi::BufferHandle ptcl_vel_z;
    nvrhi::BufferHandle ptcl_rast_r;
    nvrhi::BufferHandle ptcl_rast_y;
    nvrhi::BufferHandle ptcl_rast_b;
    nvrhi::BufferHandle vel_x_old;
    nvrhi::BufferHandle vel_y_old;
    nvrhi::BufferHandle vel_z_old;
    nvrhi::BufferHandle ptcl_pos_b;
    nvrhi::BufferHandle ptcl_vel_b;
    nvrhi::BufferHandle ptcl_color_b;
    nvrhi::BufferHandle ptcl_alive_b;

    // --- Float4 packed paint field (density,r,g,b interleaved). Produced by
    // the pack_float4 shader each frame in the commit node, registered into
    // SharedGPUBufferRegistry for zero-copy render consumption. Global grid
    // sized (res³). ---
    nvrhi::BufferHandle packed_paint;
    ProgramHandle pack_program;

    // --- Compiled shader programs (lazily built on first use; persist so we
    // don't recompile every frame) ---
    ProgramHandle deposit_program;
    ProgramHandle advect_program;
    ProgramHandle jacobi_program;
    ProgramHandle divergence_program;
    ProgramHandle gradient_program;
    ProgramHandle damp_dry_program;
    ProgramHandle bristle_sim_program;
    ProgramHandle bristle_density_constraint_program;
    ProgramHandle bristle_resample_program;
    ProgramHandle bristle_raster_program;
    ProgramHandle bristle_merge_program;
    ProgramHandle bri_liquid_transfer_program;
    ProgramHandle bri_liquid_emit_program;
    ProgramHandle field_clear_program;
    ProgramHandle ptcl_emit_program;
    ProgramHandle ptcl_update_program;
    ProgramHandle ptcl_raster_program;
    ProgramHandle ptcl_flip_pic_program;
    ProgramHandle ptcl_compact_program;
    ProgramHandle ptcl_to_grid_program;
    ProgramHandle grid_to_ptcl_program;

    // --- Control / grid bookkeeping (read by all nodes to set shader CBs) ---
    int grid_res = 0;
    int grid_res_z = 0;
    // grid_alloc_* track the resolution the buffers were allocated for, so a
    // resolution change triggers a realloc.
    int grid_alloc_res = 0;
    int grid_alloc_res_z = 0;
    float grid_paper = 0.0f;
    float grid_height = 0.0f;
    glm::vec2 grid_center = glm::vec2(0.0f);
    float grid_center_z = 0.0f;
    bool center_initialized = false;

    static constexpr int WIN_ALLOC_XY = 128;
    int win_alloc_z = 0;
    int win_origin_x = 0;
    int win_origin_y = 0;
    int win_origin_z = 0;
    bool win_origin_set = false;

    // --- Per-frame brush kinematics (finite-differenced frame-to-frame) ---
    int deposited_count = 0;
    float last_sim_time = -1.0f;
    glm::vec3 prev_brush_vel = glm::vec3(0.0f);
    glm::vec3 prev_angular_vel = glm::vec3(0.0f);
    glm::vec3 prev_brush_pos = glm::vec3(0.0f);
    bool has_prev_brush_pos = false;

    bool bristles_initialized = false;
    bool particles_initialized = false;

    ~WetbrushSimState()
    {
        if (!is_gpu_alive()) {
            auto release = [&](auto& h) { h = nullptr; };
            release(density);
            release(density_tmp);
            release(color_r);
            release(color_y);
            release(color_b);
            release(color_r_tmp);
            release(color_y_tmp);
            release(color_b_tmp);
            release(vel_x);
            release(vel_x_tmp);
            release(vel_y);
            release(vel_y_tmp);
            release(vel_z);
            release(vel_z_tmp);
            release(wetness);
            release(wetness_tmp);
            release(oil_density);
            release(oil_density_tmp);
            release(height_field);
            release(pressure_a);
            release(pressure_b);
            release(divergence_buf);
            release(bristle_data);
            release(sample_pos);
            release(sample_vel);
            release(sample_color);
            release(sample_frame);
            release(lambda_buf);
            release(sample_liquid);
            release(sample_liquid_b);
            release(sample_supply);
            release(bristle_input_color_buf);
            release(bristle_density);
            release(bristle_vel_x);
            release(bristle_vel_y);
            release(bristle_vel_z);
            release(bristle_color_r);
            release(bristle_color_y);
            release(bristle_color_b);
            release(ptcl_pos);
            release(ptcl_vel);
            release(ptcl_color);
            release(ptcl_alive);
            release(ptcl_counter);
            release(ptcl_density);
            release(ptcl_vel_x);
            release(ptcl_vel_y);
            release(ptcl_vel_z);
            release(ptcl_rast_r);
            release(ptcl_rast_y);
            release(ptcl_rast_b);
            release(vel_x_old);
            release(vel_y_old);
            release(vel_z_old);
            release(ptcl_pos_b);
            release(ptcl_vel_b);
            release(ptcl_color_b);
            release(ptcl_alive_b);
            release(packed_paint);
            return;
        }

        auto& rc = get_resource_allocator();
        auto destroy_buf = [&](nvrhi::BufferHandle& h) {
            if (h) {
                rc.destroy(h);
                h = nullptr;
            }
        };
        destroy_buf(density);
        destroy_buf(density_tmp);
        destroy_buf(color_r);
        destroy_buf(color_y);
        destroy_buf(color_b);
        destroy_buf(color_r_tmp);
        destroy_buf(color_y_tmp);
        destroy_buf(color_b_tmp);
        destroy_buf(vel_x);
        destroy_buf(vel_x_tmp);
        destroy_buf(vel_y);
        destroy_buf(vel_y_tmp);
        destroy_buf(vel_z);
        destroy_buf(vel_z_tmp);
        destroy_buf(wetness);
        destroy_buf(wetness_tmp);
        destroy_buf(oil_density);
        destroy_buf(oil_density_tmp);
        destroy_buf(height_field);
        destroy_buf(pressure_a);
        destroy_buf(pressure_b);
        destroy_buf(divergence_buf);
        destroy_buf(bristle_data);
        destroy_buf(sample_pos);
        destroy_buf(sample_vel);
        destroy_buf(sample_color);
        destroy_buf(sample_frame);
        destroy_buf(lambda_buf);
        destroy_buf(sample_liquid);
        destroy_buf(sample_liquid_b);
        destroy_buf(sample_supply);
        destroy_buf(bristle_input_color_buf);
        destroy_buf(bristle_density);
        destroy_buf(bristle_vel_x);
        destroy_buf(bristle_vel_y);
        destroy_buf(bristle_vel_z);
        destroy_buf(bristle_color_r);
        destroy_buf(bristle_color_y);
        destroy_buf(bristle_color_b);
        destroy_buf(ptcl_pos);
        destroy_buf(ptcl_vel);
        destroy_buf(ptcl_color);
        destroy_buf(ptcl_alive);
        destroy_buf(ptcl_counter);
        destroy_buf(ptcl_density);
        destroy_buf(ptcl_vel_x);
        destroy_buf(ptcl_vel_y);
        destroy_buf(ptcl_vel_z);
        destroy_buf(ptcl_rast_r);
        destroy_buf(ptcl_rast_y);
        destroy_buf(ptcl_rast_b);
        destroy_buf(vel_x_old);
        destroy_buf(vel_y_old);
        destroy_buf(vel_z_old);
        destroy_buf(ptcl_pos_b);
        destroy_buf(ptcl_vel_b);
        destroy_buf(ptcl_color_b);
        destroy_buf(ptcl_alive_b);
        destroy_buf(packed_paint);

        auto destroy_prog = [&](ProgramHandle& h) {
            if (h) {
                rc.destroy(h);
                h = nullptr;
            }
        };
        destroy_prog(deposit_program);
        destroy_prog(advect_program);
        destroy_prog(jacobi_program);
        destroy_prog(divergence_program);
        destroy_prog(gradient_program);
        destroy_prog(damp_dry_program);
        destroy_prog(bristle_sim_program);
        destroy_prog(bristle_density_constraint_program);
        destroy_prog(bristle_resample_program);
        destroy_prog(bristle_raster_program);
        destroy_prog(bristle_merge_program);
        destroy_prog(bri_liquid_transfer_program);
        destroy_prog(bri_liquid_emit_program);
        destroy_prog(field_clear_program);
        destroy_prog(ptcl_emit_program);
        destroy_prog(ptcl_update_program);
        destroy_prog(ptcl_raster_program);
        destroy_prog(ptcl_flip_pic_program);
        destroy_prog(ptcl_compact_program);
        destroy_prog(ptcl_to_grid_program);
        destroy_prog(grid_to_ptcl_program);
        destroy_prog(pack_program);
    }
};

// BristleSampleOutputs — 2-node field (written by brush_wb_bristle, read by
// brush_wb_fluid's particle emit/update). Carried as a regular socket value so
// callers that only want the samples (e.g. a readback node) don't have to
// unpack the whole WetbrushSimState. The authoritative buffers still live in
// WetbrushSimState; this carries handles to the same GPU memory.
struct BristleSampleOutputs {
    nvrhi::BufferHandle sample_pos;    // bristle sample positions (Nb*S)
    nvrhi::BufferHandle sample_color;  // bristle sample RYB color
    nvrhi::BufferHandle sample_frame;  // bristle Bishop frame (packed float4)

    static constexpr bool has_storage = false;
};

// ============================================================
// WetbrushZoneState — the SINGLE typed value that crosses the simulation-zone
// boundary (simulation_in/out group slot) and is fed back frame-to-frame.
//
// Why this and not the old WetbrushFrame bundle: the zone boundary supports
// multiple typed slots, but the ONLY thing that must ride the feedback loop
// (simulation_out -> simulation_in, moved by the eager executor after each
// cook) is the accumulated paint FIELD. The per-frame BrushPoint is produced
// INSIDE the zone every frame by mock_point_emitter and reaches deposit via an
// ordinary interior socket — it never crosses the boundary. The input stroke
// Geometry is static and enters the zone through its own simulation_in input
// slot — it does not ride feedback either. Bundling them (the old
// WetbrushFrame{stroke_curves, bp, state}) mixed a per-frame ephemeral input
// with cross-frame accumulated state; the clean design keeps only the field on
// the boundary.
//
// Carried as shared_ptr<WetbrushSimState> so the same GPU buffers persist
// across nodes AND across frames (zero-copy: nvrhi handles are refcounted
// shared_ptrs under the hood). The framework auto-registers socket types, so
// no manual entt::meta registration is needed.
struct WetbrushZoneState {
    std::shared_ptr<WetbrushSimState> state;  // the paint field (null on init)

    static constexpr bool has_storage = false;
};

}  // namespace Ruzino
