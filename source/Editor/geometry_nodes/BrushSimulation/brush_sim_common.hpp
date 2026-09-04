// Common helpers, shader-constant structs, and the shared simulation-state
// types for the streaming Wetbrush brush-paint nodes.
//
// The streaming pipeline (brush_wb_sim -> brush_wb_commit, wired as a
// simulation-zone chain) includes this header so all nodes share the EXACT
// same buffer-layout, shader-binding, and constant-buffer conventions — no
// duplicated logic that can drift.
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
// glm::qua is NOT defined by glm.hpp in this vendored GLM (fwd-declared only,
// full type lives in the gtc extension) — StrokeSample::orientation needs it.
#include "glm/gtc/quaternion.hpp"
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
            // Raw views let the scatter shaders (particle_rasterize /
            // particle_to_grid / grid_to_particle) bind these buffers as
            // RWByteAddressBuffer and run the atomicFloatAdd CAS loop —
            // structured-buffer elements have no CAS in Slang, and plain +=
            // raced, drifting RYB over long loops (paper §5.2 Eq.15/16
            // conservation).
            .setCanHaveRawViews(true)
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
    size_t size_bytes,
    const char* debug_name,
    nvrhi::BufferHandle& out_buf)
{
    // REUSE the buffer across uploads instead of destroy+create per call.
    // The upload path runs per sub-step (the bristle pipeline re-dispatches
    // several times a frame), and destroy→create hands the same pool slot
    // back while the previous dispatch can still be in flight — reads then
    // alias whatever bytes live there now (observed: one frame's CB read as
    // a blend of four earlier frames' uploads, 2026-08-27). A stable handle
    // keeps the GPUVA fixed, so queue-ordered write→dispatch is always
    // self-consistent.
    if (!out_buf || out_buf->getDesc().byteSize < size_bytes) {
        if (out_buf)
            rc.destroy(out_buf);
        out_buf = rc.create(
            nvrhi::BufferDesc{}
                .setByteSize(size_bytes)
                .setIsConstantBuffer(true)
                .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
                .setKeepInitialState(true)
                .setDebugName(debug_name));
    }
    auto cmd = rc.create(CommandListDesc{});
    cmd->open();
    cmd->writeBuffer(out_buf, data, size_bytes);
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
    // Threshold for the brush-interior boundary in pressure projection (paper
    // §4.2). See common.slangh SimConstants.brush_boundary_gate. Must match the
    // shader layout (replaces the former _pad0).
    float brush_boundary_gate;
    // Per-dispatch velocity damping multiplier (fluid_damp_dry); host sends
    // pow(frame_damp, 1/substeps). Replaces former _pad1.
    float velocity_damp;
    // Scale on the brush velocity injected by brush_deposit. Replaces former
    // _pad2.
    float velocity_inject_scale;
    // field_copy_window mode: 0 = window→window, 1 = global→window,
    // 2 = window→global. See common.slangh.
    int copy_mode;
    // fluid_damp_dry mode: 0 = global wetness-only, 1 = window vel+wetness.
    int damp_mode;
    // fluid_advect: 1 = field_in window-local (velocity family), 0 = global
    // canvas field. field_out is always window-local.
    int advect_field_window_local;
    // Gravity (world units/s²). WORLD SCALE 1 unit = 1 cm → g = 981 cm/s² =
    // 981 u/s². Applied to fluid cells (§4.2 standard Eulerian body force)
    // and particles (§4.3 a_k). Direction-adjustable for experiments via
    // WB_GRAVITY_X/Y/Z. Must match common.slangh SimConstants.
    float gravity_x;
    float gravity_y;
    float gravity_z;
};

struct BristleConstants {
    // Pen orientation (StrokeSample.orientation) as rotation-matrix COLUMNS
    // stored per local axis: brush_R0/1/2.xyz = world image of local X/Y/Z
    // (i.e. the COLUMNS of mat3_cast(q)). Shader side uses
    // rot(v) = v.x*R0.xyz + v.y*R1.xyz + v.z*R2.xyz — no mul() row/column
    // ambiguity. Local frame: -Z = root→tip, XY = root disk. Identity =
    // upright brush.
    //
    // MUST stay the FIRST fields (offsets 0/16/32) and mirror common.slangh
    // BristleConstants field-for-field. They used to live at the END of the
    // struct, which was a CB-layout bug: the C++ side packs glm::vec4 tightly
    // (4-byte aligned — R0@184 here), while the slang/std140 view aligns each
    // float4 to 16 bytes (R0@192, +8-byte skew) and sizes the struct larger
    // than the host's 232 bytes — every row reached the shader rotated into
    // garbage (the "brush collapsed into a blue stick" bug of 2026-08-27).
    // Front-loading the rows makes offsets 0/16/32 agree under ANY packing
    // rule on both sides.
    glm::vec4 brush_R0{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec4 brush_R1{ 0.0f, 1.0f, 0.0f, 0.0f };
    glm::vec4 brush_R2{ 0.0f, 0.0f, 1.0f, 0.0f };

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
    // Pen state (from deposit's StrokeSample): pen-up means the brush is away
    // from the canvas — particle_to_grid deposits slow particles regardless
    // of d_{B,k}, and grid_to_particle suspends conversion.
    int pen_down;
    // Brush linear velocity (units/s) for particle_update's Eq.9/10 two-step
    // (the sample frame translates with the brush). Must match common.slangh
    // ParticleConstants (incl. the trailing field).
    float brush_vel_x, brush_vel_y, brush_vel_z;
    // §5.2 "moves slowly" absolute speed gate (see common.slangh).
    float slow_deposit_speed;
    // Gravity for particle_update's a_k (see common.slangh ParticleConstants).
    float gravity_x;
    float gravity_y;
    float gravity_z;
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
    int emit_budget;  // global per-step particle-creation cap (0 = unlimited)
    // Brush world position — per-frame entropy for the EMIT budget's
    // probabilistic pre-gate. Must match common.slangh.
    float brush_pos_x;
    float brush_pos_y;
    float brush_pos_z;
    int window_origin_x;
    int window_origin_y;
    int window_origin_z;
    int window_size_x;
    int window_size_z;
    // 1 = this frame is a dip (stroke_start): the §5.1 ABSORB pass
    // initializes m_j = M'_j(ψ) (Eq.12 with the CURRENT crowding), instead of
    // the host pre-writing m_j = M_max. See bristle_liquid_transfer.slang.
    int dip_frame;
    // Emitted-particle mass multiplier (host env WB_EMIT_MASS_SCALE, default
    // 1): per_particle = max(M_j*0.05, 0.002) * scale. Scalar appended at the
    // scalar tail — the past CB misalignment bug was a float4 appended after
    // scalars (host 4B vs slang 16B alignment); scalar offsets match.
    float emit_mass_scale;
};

struct ConstraintModeCB {
    int mode;
    float pad[3];
};

// ============================================================
// StrokeSample — one sample of the PEN DYNAMICS along a stroke, the
// per-frame contract between the trajectory source and the wetbrush zone.
//
// Produced by the pen-motion source (one per simulation frame) and
// consumed by node_brush_wb_sim (which advances the paint state by one step
// from this single sample). Defined in the shared header so the producer and
// the sim node see the SAME type identity and can pass it through a socket.
//
// Formerly `BrushPoint`: position-only, which forced deposit to
// finite-difference every derivative (accel = double-differentiated
// position noise) and GUESS an orientation from the velocity heading — a
// field no shader even consumed (brush_rotation is dead in common.slangh;
// the bristle footprint spiral was hardcoded upright in world XY). The pen
// input model is its full DYNAMIC state: pose AND derivatives.
//
// Orientation semantics: `orientation` rotates the brush LOCAL frame into
// world. Local convention (matches bristle_simulate.slang): local -Z points
// from the root plane toward the tip (identity = pen held vertical, tip
// down), local XY is the root disk. Identity ⇒ the old hardcoded upright
// brush, so the default is behavior-preserving.
//
// vel / angular_vel are the pen's world velocity (units/s) and angular
// velocity (rad/s). A source that knows them analytically sets
// has_dynamics = true and deposit uses them directly (only accel / omega_dot
// stay host-differenced — first-order on exact derivatives, no double
// amplification of position noise). Captured input without derivatives
// leaves has_dynamics = false and deposit falls back to finite differencing.
//
// Plain aggregate: auto-registered with entt::meta by get_socket_type<T>()
// on first use, the same way Geometry / Eigen::MatrixXd socket types work.
// ============================================================
struct StrokeSample {
    glm::vec3 pos{ 0.0f };  // pen reference point (root-plane center), world
    glm::quat orientation{ 1.0f, 0.0f, 0.0f, 0.0f };  // local→world, (w,x,y,z)
    glm::vec3 vel{ 0.0f };  // world velocity, units/s (valid if has_dynamics)
    glm::vec3 angular_vel{ 0.0f };  // world angular velocity, rad/s
    bool has_dynamics = false;  // vel/angular_vel are analytic — skip host FD
    float time = 0.0f;          // stroke-local time (seconds since pen-down)
    bool active = false;        // pen is currently down
    bool stroke_start = false;  // first sample of a new stroke (pen-down edge)
    glm::vec3 color{ 1.0f, 0.0f, 0.0f };  // RYB ink color (from the trajectory)
};

// ============================================================
// WetbrushSimState — the SHARED cross-frame state for the streaming
// simulation-zone brush chain (brush_wb_sim -> brush_wb_commit).
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

    // §5.1 sample saturation capacity scale M_max (Eq.12). Shared by the
    // deposit node (dip initialization m_j = M_max at stroke_start — a
    // "dipped-full brush") and the bristle node (BristleLiquidConstants).
    // Single definition so the dip load and the capacity cannot drift apart.
    // The paper never gives M_max; it is the free parameter that sets HOW
    // MUCH PAINT ONE DIP CARRIES (stroke length before the brush runs dry).
    // History: 0.03 drained in ~15 frames; 0.15 sustained one 60-frame
    // stroke but ran out ~60% into the 5 cm / 2 s fixture stroke at the
    // 1u=1cm scale; 0.30 covers the full fixture stroke. Must stay coupled
    // with rho_0 (bristle node) so the emission radius
    // R_j = cbrt(3·M_max/(4π·ρ₀)) ≈ 0.36×brush_radius stays inside D1
    // (ρ₀ = 12.3 for M_max = 0.3 → R_j ≈ 0.18 cm < D1 = 0.3 cm).
    static constexpr float WB_M_MAX = 0.30f;

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
    // (footprint ~20 cells, 80 roots → visible grain in the rasterized
    // density). 200 helped but close-up views still show grain. Paper's own
    // smoothness source is dense bristle sampling (up to 600×128 = 76800
    // samples), so use the paper's upper bound — fully paper-faithful, no XY
    // splat kernel (which the paper doesn't specify).
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
    // 262144 saturated in ~4 frames at the 73k/frame emission rate (60fps
    // stroke); the paper allows up to 2M particles (§4.3 "200K to 1M",
    // §7 "as many as 2M particles" for a large brush). 1M pinned at the cap
    // mid-stroke (emit floods the pool after the dip; deposits lag), which
    // throttled delivery and kept the per-particle passes at peak — 2M is
    // the paper's own ceiling. Memory: 6 float4 buffers (pos/vel/color +
    // ping-pong) + 2 uint buffers ≈ 2M × (6×16 + 2×4) B ≈ 208 MB.
    // The pool is only fully used while particles outpace deposition;
    // compact keeps it tight.
    static constexpr int MAX_PARTICLES = 2097152;

    // --- Cross-frame brush state (written by deposit every frame) ---
    // Pen-down flag from the current StrokeSample. Gates §5.1 absorb/emit in
    // the bristle node and §5.2 conversions in the fluid node: a pen-up brush
    // is not painting. Without it, a parked/stalled brush kept emitting
    // (+emit_budget particles/frame) while the fluid node skipped particle
    // maintenance — the pool wrapped with nothing depositing.
    bool pen_down = false;

    // Dip request for this frame (deposit sets it from bp.stroke_start; the
    // bristle node's §5.1 ABSORB pass consumes it by initializing
    // m_j = M'_j(ψ) inside the shader, then clears it).
    bool dip_frame = false;

    // WB_REDIP_EVERY 行笔计时器：pen_down 期间累计 dt，跨过周期即重触发
    // dip_frame（真实画家的"提笔回蘸"；见 node_brush_wb_sim.cpp 供墨注释）。
    float redip_clock = 0.0f;

    nvrhi::BufferHandle ptcl_pos;
    nvrhi::BufferHandle ptcl_vel;
    nvrhi::BufferHandle ptcl_color;
    nvrhi::BufferHandle ptcl_alive;
    nvrhi::BufferHandle ptcl_counter;
    // Global per-step particle-creation budget for the §5.1 EMIT pass
    // (1 uint, zeroed each frame by the bristle node). See
    // BristleLiquidConstants::emit_budget.
    nvrhi::BufferHandle emit_budget;
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

    // --- Debug-draw buffers (blocked layouts, see debug_pack_*.slang).
    // Packed each frame in the commit node and registered into
    // SharedGPUBufferRegistry for the debug visualization pipeline
    // (render_wetbrush_debug.py): live particles, non-zero grid voxels
    // (GPU-compacted, capped at DEBUG_MAX_VOXELS), and bristle capsule
    // segments. CanHaveRawViews so the render side can bind them
    // RawBuffer_SRV / ByteAddressBuffer. ---
    static constexpr int DEBUG_MAX_VOXELS = 1 << 20;  // 1M points, 28 MB
    nvrhi::BufferHandle debug_ptcl_buf;       // MAX_PARTICLES * 7 floats
    nvrhi::BufferHandle debug_voxel_buf;      // DEBUG_MAX_VOXELS * 7 floats
    nvrhi::BufferHandle debug_bristle_buf;    // Nb*(M-1) * 10 floats
    nvrhi::BufferHandle debug_voxel_counter;  // 1 uint, zeroed each frame
    ProgramHandle debug_pack_particles_program;
    ProgramHandle debug_pack_voxels_program;
    ProgramHandle debug_pack_bristles_program;

    // --- Compiled shader programs (lazily built on first use; persist so we
    // don't recompile every frame) ---
    ProgramHandle deposit_program;
    ProgramHandle advect_program;
    // Conservative flux-form (upwind FV) advection for the canvas SCALARS
    // (density/color/wetness/oil). Semi-Lagrangian (advect_program) stays
    // for the velocity family; scalars need the conservative scheme because
    // value-interpolation of an extensive field created/destroyed canvas
    // mass at boundaries (the ~35% tail leak — see the shader's header).
    ProgramHandle advect_scalar_program;
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
    // Render-time deposit-preview raster (Eq.16 kernel; see
    // particle_rasterize_render.slang). Run by the commit node before pack so
    // the swarm composite previews the on-canvas deposit instead of the sim
    // raster's compact 1-cell splat (which dark-saturated the window).
    ProgramHandle ptcl_raster_render_program;
    ProgramHandle ptcl_flip_pic_program;
    ProgramHandle ptcl_compact_program;
    ProgramHandle ptcl_to_grid_program;
    ProgramHandle grid_to_ptcl_program;
    // Window region copy (vel → vel_old FLIP snapshot). The active window is a
    // non-contiguous block inside the global buffer, so the snapshot cannot be
    // a plain copyBuffer(..., win_n3d) — that copies the corner at offset 0.
    ProgramHandle field_copy_window_program;
    // Re-bases the persistent window-sized fields (velocity family, pressure
    // warm-start) when the active window moves. See window_scroll.slang.
    ProgramHandle window_scroll_program;

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

    // Active-window XY allocation, in CELLS (paper §4.2: window "typically
    // 128×128×32" — a window in brush diameters, not absolute cells). The
    // window's WORLD coverage is win_cells × paper_size / grid_res and must
    // span several brush diameters (footprint + D0 shell + flow room). At
    // the design scale (1 unit = 1 cm, paper 10 cm, res 1024 → cell 0.00977
    // cm) 320 cells = 3.1 cm ≈ 6.25 brush radii — the same relative room the
    // paper's 128-cell window has around its brush. WB_WIN_XY overrides.
    static int win_alloc_xy()
    {
        static const int cells = [] {
            const char* e = std::getenv("WB_WIN_XY");
            const int n = e ? std::atoi(e) : 320;
            return n < 64 ? 320 : n;
        }();
        return cells;
    }
    int win_alloc_z = 0;
    int win_origin_x = 0;
    int win_origin_y = 0;
    int win_origin_z = 0;
    bool win_origin_set = false;
    // RENDER window (commit node only): the swarm-raster composite window
    // follows the alive-particle cloud, NOT the §4.2 solve window above.
    // pack_float4 only composites the raster inside its window, so a
    // brush-centered render window hard-cut black edges wherever riding
    // paint straddled the boundary (the tilted-stroke wet head trails
    // ~1.5 cm behind the root at speed). Diagnostics print both origins.
    int render_win_origin_x = 0;
    int render_win_origin_y = 0;

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
            release(emit_budget);
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
            release(debug_ptcl_buf);
            release(debug_voxel_buf);
            release(debug_bristle_buf);
            release(debug_voxel_counter);
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
        destroy_buf(emit_budget);
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
        destroy_buf(debug_ptcl_buf);
        destroy_buf(debug_voxel_buf);
        destroy_buf(debug_bristle_buf);
        destroy_buf(debug_voxel_counter);

        auto destroy_prog = [&](ProgramHandle& h) {
            if (h) {
                rc.destroy(h);
                h = nullptr;
            }
        };
        destroy_prog(deposit_program);
        destroy_prog(advect_program);
        destroy_prog(advect_scalar_program);
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
        destroy_prog(ptcl_raster_render_program);
        destroy_prog(ptcl_flip_pic_program);
        destroy_prog(ptcl_compact_program);
        destroy_prog(ptcl_to_grid_program);
        destroy_prog(grid_to_ptcl_program);
        destroy_prog(field_copy_window_program);
        destroy_prog(pack_program);
        destroy_prog(debug_pack_particles_program);
        destroy_prog(debug_pack_voxels_program);
        destroy_prog(debug_pack_bristles_program);
    }
};

// ============================================================
// WetbrushZoneState — the SINGLE typed value that crosses the simulation-zone
// boundary (simulation_in/out group slot) and is fed back frame-to-frame.
//
// Why this and not the old WetbrushFrame bundle: the zone boundary supports
// multiple typed slots, but the ONLY thing that must ride the feedback loop
// (simulation_out -> simulation_in, moved by the eager executor after each
// cook) is the accumulated paint FIELD. The per-frame StrokeSample is produced
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
