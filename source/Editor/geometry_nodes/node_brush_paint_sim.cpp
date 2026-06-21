// GPU brush paint simulation — Wetbrush-style Eulerian-Lagrangian solver
// Based on: Chen et al., "Wetbrush: GPU-based 3D Painting Simulation
// at the Bristle Level", SIGGRAPH Asia 2015.
//
// Uses: Stable Fluids (Stam 1999) for advection/diffusion,
//       Fixed-point pressure projection (Algorithm 1),
//       Brightness-preserving RYB color mixing (Algorithm 2),
//       Bristle-level brush model (Section 4.1),
//       FLIP/PIC hybrid particles (Section 4.3).

#include "RHI/shaderCompiler.h"
#include "GCore/Components/CurveComponent.h"
#include "GCore/Components/PointsComponent.h"
#include "GCore/GOP.h"
#include "GCore/algorithms/intersection.h"
#include "geom_node_base.h"
#include "GPUContext/compute_context.hpp"
#include "RHI/ResourceManager/resource_allocator.hpp"
#include "GCore/algorithms/gpu_geometry.h"
#include "nvrhi/nvrhi.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <vector>

// ============================================================
// Helpers (outside NODE_DEF_OPEN_SCOPE to avoid C-linkage issues)
// ============================================================

namespace Ruzino {
namespace {

// ============================================================
// Stroke recorder — writes the captured stroke to disk so the
// editor's real input can be replayed by a headless test.
// Enabled when the env var RUZINO_RECORD_STROKE is set to a path;
// the LAST execution's full stroke (all accumulated points +
// timestamps + stroke segmentation) overwrites the file. This
// lets a frame-by-frame editor run produce one final capture
// with the complete trajectory.
// ============================================================
struct StrokeRecord {
    std::vector<glm::vec3> points;
    std::vector<float> timestamps;
    std::vector<int> stroke_lengths;  // vert count per sub-stroke

    // Snapshot of every brush_paint_sim input that affects the
    // physics — the test reads these back so its replay uses the
    // EXACT same parameters the editor run used.
    int resolution = 0;
    int resolution_z = 0;
    float paper_size = 0.0f;
    float canvas_center_x = 0.0f;
    float canvas_center_y = 0.0f;
    float canvas_z = 0.0f;
    float canvas_height = 0.0f;
    float brush_radius = 0.0f;
    float brush_pressure = 0.0f;
    float ink_amount = 0.0f;
    float viscosity = 0.0f;
    float oil_density = 0.0f;
    float diffusion = 0.0f;
    float pickup_rate = 0.0f;
    float drying_rate = 0.0f;

    // brush_input params (color + width + pressure) — captured from the
    // curve's display_color/width so the replay matches what the editor
    // actually fed to paint_sim.
    float ink_r_ryb = 1.0f;
    float ink_y_ryb = 0.0f;
    float ink_b_ryb = 0.0f;
    float brush_width = 0.02f;
};

bool stroke_recorder_enabled()
{
    return std::getenv("RUZINO_RECORD_STROKE") != nullptr;
}

std::string stroke_recorder_path()
{
    const char* p = std::getenv("RUZINO_RECORD_STROKE");
    if (p && *p) return std::string(p);
    // Default location: next to the binary, easy to find.
    return "brush_stroke_capture.json";
}

void write_stroke_record(const StrokeRecord& rec)
{
    std::ofstream f(stroke_recorder_path());
    if (!f) {
        spdlog::warn(
            "brush_paint_sim: stroke recorder could not open '{}'",
            stroke_recorder_path());
        return;
    }
    f << std::fixed << std::setprecision(6);
    f << "{\n";
    f << "  \"points\": [";
    for (size_t i = 0; i < rec.points.size(); ++i) {
        if (i) f << ", ";
        f << "[" << rec.points[i].x << ", " << rec.points[i].y << ", "
          << rec.points[i].z << "]";
    }
    f << "],\n";
    f << "  \"timestamps\": [";
    for (size_t i = 0; i < rec.timestamps.size(); ++i) {
        if (i) f << ", ";
        f << rec.timestamps[i];
    }
    f << "],\n";
    f << "  \"stroke_lengths\": [";
    for (size_t i = 0; i < rec.stroke_lengths.size(); ++i) {
        if (i) f << ", ";
        f << rec.stroke_lengths[i];
    }
    f << "],\n";
    f << "  \"resolution\": " << rec.resolution << ",\n";
    f << "  \"resolution_z\": " << rec.resolution_z << ",\n";
    f << "  \"paper_size\": " << rec.paper_size << ",\n";
    f << "  \"canvas_center_x\": " << rec.canvas_center_x << ",\n";
    f << "  \"canvas_center_y\": " << rec.canvas_center_y << ",\n";
    f << "  \"canvas_z\": " << rec.canvas_z << ",\n";
    f << "  \"canvas_height\": " << rec.canvas_height << ",\n";
    f << "  \"brush_radius\": " << rec.brush_radius << ",\n";
    f << "  \"brush_pressure\": " << rec.brush_pressure << ",\n";
    f << "  \"ink_amount\": " << rec.ink_amount << ",\n";
    f << "  \"viscosity\": " << rec.viscosity << ",\n";
    f << "  \"oil_density\": " << rec.oil_density << ",\n";
    f << "  \"diffusion\": " << rec.diffusion << ",\n";
    f << "  \"pickup_rate\": " << rec.pickup_rate << ",\n";
    f << "  \"drying_rate\": " << rec.drying_rate << ",\n";
    f << "  \"ink_r_ryb\": " << rec.ink_r_ryb << ",\n";
    f << "  \"ink_y_ryb\": " << rec.ink_y_ryb << ",\n";
    f << "  \"ink_b_ryb\": " << rec.ink_b_ryb << ",\n";
    f << "  \"brush_width\": " << rec.brush_width << "\n";
    f << "}\n";
    spdlog::info(
        "brush_paint_sim: recorded {} pts ({} strokes) to '{}'",
        rec.points.size(), rec.stroke_lengths.size(),
        stroke_recorder_path());
}


std::string shader_dir()
{
    return SlangShaderCompiler::get_shader_dir(ShaderDirType::GeomNodes)
        .string() + "/BrushSimulation/shaders/";
}

nvrhi::BufferHandle create_field_buffer(
    ResourceAllocator& rc, int n, const char* debug_name)
{
    return rc.create(nvrhi::BufferDesc{}
        .setByteSize(n * sizeof(float))
        .setStructStride(sizeof(float))
        .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
        .setKeepInitialState(true)
        .setCanHaveUAVs(true)
        .setCanHaveTypedViews(true)
        .setDebugName(debug_name));
}

nvrhi::BufferHandle create_typed_buffer(
    ResourceAllocator& rc, int count, int stride, const char* debug_name)
{
    return rc.create(nvrhi::BufferDesc{}
        .setByteSize(count * stride)
        .setStructStride(stride)
        .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
        .setKeepInitialState(true)
        .setCanHaveUAVs(true)
        .setCanHaveTypedViews(true)
        .setDebugName(debug_name));
}

nvrhi::BufferHandle create_byte_buffer(
    ResourceAllocator& rc, int size_bytes, const char* debug_name)
{
    return rc.create(nvrhi::BufferDesc{}
        .setByteSize(size_bytes)
        .setInitialState(nvrhi::ResourceStates::UnorderedAccess)
        .setKeepInitialState(true)
        .setCanHaveUAVs(true)
        .setDebugName(debug_name));
}

ProgramHandle compile_shader(
    ResourceAllocator& rc, const std::string& filename)
{
    ProgramDesc desc;
    desc.shaderType = nvrhi::ShaderType::Compute;
    desc.set_path(shader_dir() + filename);
    desc.set_entry_name("main");
    auto prog = rc.create(desc);
    if (!prog->get_error_string().empty()) {
        spdlog::error("Failed to compile {}: {}", filename,
                      prog->get_error_string());
        rc.destroy(prog);
        return nullptr;
    }
    return prog;
}

void dispatch_field(
    ResourceAllocator& rc, ProgramHandle prog,
    const std::vector<std::pair<std::string, nvrhi::BufferHandle>>& srvs,
    const std::vector<std::pair<std::string, nvrhi::BufferHandle>>& uavs,
    nvrhi::BufferHandle cb, int total_threads)
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

void dispatch_raw(
    ResourceAllocator& rc, ProgramHandle prog,
    const std::vector<std::pair<std::string, nvrhi::BufferHandle>>& srvs,
    const std::vector<std::pair<std::string, nvrhi::BufferHandle>>& uavs,
    nvrhi::BufferHandle cb, int total_threads)
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

void upload_constant_buffer(
    ResourceAllocator& rc, nvrhi::IDevice* device,
    const void* data, size_t size, const char* debug_name,
    nvrhi::BufferHandle& out_buf)
{
    if (out_buf)
        rc.destroy(out_buf);
    out_buf = rc.create(nvrhi::BufferDesc{}
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

void reset_counter(
    ResourceAllocator& rc, nvrhi::IDevice* device,
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

} // anonymous namespace
} // namespace Ruzino

NODE_DEF_OPEN_SCOPE

// Shader constants — must match common.slangh structs
struct SimConstants {
    int res;                // Grid XY resolution N
    float cell_size;        // paper_size / res
    float paper_size;       // Total paper extent (XY)
    float dt;               // Time step

    float viscosity;        // Viscosity coefficient (base, × oil_density per-cell)
    float diffusion;        // Diffusion rate
    float drying_rate;      // Drying rate
    float brush_radius;     // Brush radius (world units)

    float ink_amount;       // Ink deposit amount
    int num_vertices;       // Number of NEW curve vertices to deposit
    float center_x;         // Grid center X (world space)
    float center_y;         // Grid center Y (world space)

    float center_z;         // Grid center Z (world space)
    float effective_radius; // brush_radius (world units)
    int jacobi_mode;        // 0 = diffuse, 1 = pressure
    float jacobi_alpha;     // alpha for Jacobi: dt*rate*N^2 (diffuse) or 1.0 (pressure)

    // --- 3D grid extension ---
    int res_z;              // Grid Z (height) resolution D
    float height_extent;    // Total height extent in world units
    float grid_center_z;    // Z center

    // --- Active window (Wetbrush §4.2) ---
    int window_origin_x;
    int window_origin_y;
    int window_origin_z;
    int window_size_x;
    int window_size_y;
    int window_size_z;

    // --- Oil density (Wetbrush §6) ---
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
    // Eq.9 frame-origin acceleration a_L and angular acceleration ω̇_L.
    float brush_accel_x, brush_accel_y, brush_accel_z;
    float brush_angular_accel_x, brush_angular_accel_y, brush_angular_accel_z;
    // Active-window origin/size (Wetbrush §4.2) — 3D buffers are window-sized.
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
    // Active-window origin/size (Wetbrush §4.2) — 3D buffers are window-sized.
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
// Storage
// ============================================================

struct PaintSimStorage {
    static constexpr bool has_storage = false;

    // Grid field buffers (3D: res × res × res_z)
    nvrhi::BufferHandle density, density_tmp;
    nvrhi::BufferHandle color_r, color_y, color_b, color_tmp;
    nvrhi::BufferHandle vel_x, vel_x_tmp;
    nvrhi::BufferHandle vel_y, vel_y_tmp;
    nvrhi::BufferHandle vel_z, vel_z_tmp;          // 3D: vertical velocity
    nvrhi::BufferHandle wetness, wetness_tmp;
    nvrhi::BufferHandle oil_density, oil_density_tmp;  // §6 oil density field (per-cell)
    nvrhi::BufferHandle height_field;
    nvrhi::BufferHandle pressure_a, pressure_b;
    nvrhi::BufferHandle divergence_buf;

    // --- 2D canvas layer (Wetbrush §4.2: dried paint = "canvas") ---
    // Full-resolution 2D fields storing paint that has been committed from the
    // 3D active window. Once paint dries or the brush moves away, the window's
    // Z-column is collapsed (density summed, color mass-weighted) into these
    // 2D fields. This is what lets the 3D window follow the brush WITHOUT
    // losing previously painted regions: the canvas remembers everything.
    // Memory: res*res * 5 fields * 4 bytes (e.g. 335MB at 4096²) — far smaller
    // than a full 3D grid and independent of paint height.
    nvrhi::BufferHandle canvas_density;   // res*res — accumulated paint height
    nvrhi::BufferHandle canvas_color_r;   // res*res — RYB-R
    nvrhi::BufferHandle canvas_color_y;   // res*res — RYB-Y
    nvrhi::BufferHandle canvas_color_b;   // res*res — RYB-B
    nvrhi::BufferHandle canvas_wetness;   // res*res — dryness (0=wet,1=dry)

    // Per-frame upload buffers
    nvrhi::BufferHandle vertex_buf;
    nvrhi::BufferHandle color_buf;

    // --- Bristle model (3D, §4.1) ---
    static constexpr int NUM_BRISTLES = 80;
    static constexpr int VERTS_PER_BRISTLE = 10;
    static constexpr int SAMPLES_PER_BRISTLE = 128;
    // Each bristle vertex = 2 float4 (pos.xyz+pad, vel.xyz+pad) -> stride 32.
    static constexpr int BRISTLE_VERTEX_STRIDE = sizeof(float) * 4 * 2;

    nvrhi::BufferHandle bristle_data;   // float4 * (Nb*M*2) — 3D pos/vel packed
    nvrhi::BufferHandle sample_pos;     // float4 * Nb*S — 3D position (.xyz)
    nvrhi::BufferHandle sample_vel;     // float4 * Nb*S — 3D velocity (.xyz)
    nvrhi::BufferHandle sample_color;   // float4 * Nb*S
    nvrhi::BufferHandle sample_frame;   // SampleFrame (3 float4) * Nb*S — Bishop frame + omega_L
    nvrhi::BufferHandle bristle_density; // N*N*D (accumulation)
    nvrhi::BufferHandle bristle_vel_x;  // N*N*D
    nvrhi::BufferHandle bristle_vel_y;  // N*N*D
    nvrhi::BufferHandle bristle_vel_z;  // N*N*D
    nvrhi::BufferHandle bristle_color_r; // N*N*D
    nvrhi::BufferHandle bristle_color_y; // N*N
    nvrhi::BufferHandle bristle_color_b; // N*N

    // --- Bristle liquid transfer (Section 5.1) ---
    nvrhi::BufferHandle sample_liquid;       // SampleLiquid * Nb*S (mass + RYB pigment)
    nvrhi::BufferHandle sample_liquid_b;      // ping-pong for liquid transfer
    nvrhi::BufferHandle sample_supply;        // per-sample paint supply (consumed by ABSORB)
    nvrhi::BufferHandle bristle_input_color_buf; // float4: user paint color RYB

    // --- FLIP/PIC particles ---
    static constexpr int MAX_PARTICLES = 262144;  // 256K (paper: 210K-2M)

    nvrhi::BufferHandle ptcl_pos;
    nvrhi::BufferHandle ptcl_vel;
    nvrhi::BufferHandle ptcl_color;
    nvrhi::BufferHandle ptcl_alive;
    nvrhi::BufferHandle ptcl_counter;   // ByteAddressBuffer, 4 bytes
    nvrhi::BufferHandle ptcl_density;   // N*N*D
    nvrhi::BufferHandle ptcl_vel_x;     // N*N*D
    nvrhi::BufferHandle ptcl_vel_y;     // N*N*D
    nvrhi::BufferHandle ptcl_vel_z;     // N*N*D (3D particle z-velocity accum)
    nvrhi::BufferHandle ptcl_rast_r;    // N*N*D (particle rasterized RYB color)
    nvrhi::BufferHandle ptcl_rast_y;    // N*N
    nvrhi::BufferHandle ptcl_rast_b;    // N*N
    nvrhi::BufferHandle vel_x_old;      // N*N*D (snapshot for FLIP)
    nvrhi::BufferHandle vel_y_old;      // N*N*D
    nvrhi::BufferHandle vel_z_old;      // N*N*D
    // Ping-pong particle buffers for update
    nvrhi::BufferHandle ptcl_pos_b;
    nvrhi::BufferHandle ptcl_vel_b;
    nvrhi::BufferHandle ptcl_color_b;
    nvrhi::BufferHandle ptcl_alive_b;

    // Shader programs — fluid
    ProgramHandle deposit_program;
    ProgramHandle advect_program;
    ProgramHandle jacobi_program;
    ProgramHandle divergence_program;
    ProgramHandle gradient_program;
    ProgramHandle damp_dry_program;

    // Bristle density constraint (PBF, Macklin & Müller 2013)
    nvrhi::BufferHandle lambda_buf;   // float * Nb*M

    // Shader programs — bristle
    ProgramHandle bristle_sim_program;
    ProgramHandle bristle_density_constraint_program;
    ProgramHandle bristle_resample_program;
    ProgramHandle bristle_raster_program;
    ProgramHandle bristle_merge_program;
    ProgramHandle bri_liquid_transfer_program;  // ABSORB pass
    ProgramHandle bri_liquid_emit_program;      // EMIT pass
    ProgramHandle field_clear_program;

    // Shader programs — particle
    ProgramHandle ptcl_emit_program;
    ProgramHandle ptcl_update_program;
    ProgramHandle ptcl_raster_program;
    ProgramHandle ptcl_flip_pic_program;
    ProgramHandle ptcl_compact_program;
    ProgramHandle ptcl_to_grid_program;
    ProgramHandle grid_to_ptcl_program;

    // Canvas commit program — collapses 3D window columns into 2D canvas layer
    ProgramHandle canvas_commit_program;

    // Grid state
    int grid_res = 0;
    int grid_res_z = 0;       // Z (height) resolution, default like 32
    int grid_alloc_res = 0;
    int grid_alloc_res_z = 0;
    float grid_paper = 0.0f;
    float grid_height = 0.0f; // Height extent in world units
    glm::vec2 grid_center = glm::vec2(0.0f);
    float grid_center_z = 0.0f;
    bool center_initialized = false;

    // --- Active window allocation (Wetbrush §4.2) ---
    // 3D fluid fields are allocated at WINDOW size, not full-grid size. The
    // window follows the brush; paint that leaves the window is committed to
    // the 2D canvas layer. This makes 3D memory independent of resolution.
    // WIN_ALLOC_* are the fixed allocation dimensions (set once at alloc).
    static constexpr int WIN_ALLOC_XY = 128;  // paper's active-window XY size
    int win_alloc_z = 0;                      // = res_z at alloc time
    // Current window origin in global grid coords (top-left corner). Updated
    // each frame to track the brush. When it changes, the old region is
    // committed to the canvas layer and cleared before moving.
    int win_origin_x = 0;
    int win_origin_y = 0;
    int win_origin_z = 0;
    bool win_origin_set = false;  // first-frame flag (no commit needed)

    int deposited_count = 0;
    float last_sim_time = -1.0f;
    // Previous-frame brush state for finite-difference inertial terms
    // (brush acceleration a_B, angular acceleration ω̇). Eq.2/Eq.9.
    glm::vec3 prev_brush_vel = glm::vec3(0.0f);
    glm::vec3 prev_angular_vel = glm::vec3(0.0f);

    // Bristle state
    bool bristles_initialized = false;

    // Particle state
    bool particles_initialized = false;

    ~PaintSimStorage()
    {
        if (!is_gpu_alive()) {
            // GPU already torn down via atexit — just release refs.
            auto release = [&](auto& h) { h = nullptr; };
            release(density); release(density_tmp);
            release(color_r); release(color_y); release(color_b); release(color_tmp);
            release(vel_x); release(vel_x_tmp);
            release(vel_y); release(vel_y_tmp);
            release(vel_z); release(vel_z_tmp);
            release(wetness); release(wetness_tmp);
            release(oil_density); release(oil_density_tmp);
            release(height_field);
            release(pressure_a); release(pressure_b);
            release(divergence_buf);
            release(canvas_density); release(canvas_color_r);
            release(canvas_color_y); release(canvas_color_b);
            release(canvas_wetness);
            release(vertex_buf); release(color_buf);
            release(bristle_data); release(sample_pos); release(sample_vel);
            release(sample_color); release(sample_frame); release(lambda_buf);
            release(sample_liquid); release(sample_liquid_b); release(sample_supply); release(bristle_input_color_buf);
            release(bristle_density); release(bristle_vel_x); release(bristle_vel_y); release(bristle_vel_z);
            release(bristle_color_r); release(bristle_color_y); release(bristle_color_b);
            release(ptcl_pos); release(ptcl_vel); release(ptcl_color);
            release(ptcl_alive); release(ptcl_counter);
            release(ptcl_density); release(ptcl_vel_x); release(ptcl_vel_y); release(ptcl_vel_z);
            release(ptcl_rast_r); release(ptcl_rast_y); release(ptcl_rast_b);
            release(vel_x_old); release(vel_y_old); release(vel_z_old);
            release(ptcl_pos_b); release(ptcl_vel_b); release(ptcl_color_b); release(ptcl_alive_b);
            release(deposit_program); release(advect_program);
            release(jacobi_program); release(divergence_program);
            release(gradient_program); release(damp_dry_program);
            release(bristle_sim_program); release(bristle_raster_program);
            release(bristle_merge_program); release(field_clear_program);
            release(bristle_density_constraint_program);
            release(bristle_resample_program);
            release(bri_liquid_transfer_program); release(bri_liquid_emit_program);
            release(ptcl_emit_program); release(ptcl_update_program);
            release(ptcl_raster_program); release(ptcl_flip_pic_program);
            release(ptcl_compact_program); release(ptcl_to_grid_program);
            release(grid_to_ptcl_program);
            release(canvas_commit_program);
            return;
        }

        // GPU alive — properly return resources to allocator cache.
        auto& rc = get_resource_allocator();
        auto destroy_buf = [&](nvrhi::BufferHandle& h) {
            if (h) { rc.destroy(h); h = nullptr; }
        };
        destroy_buf(density); destroy_buf(density_tmp);
        destroy_buf(color_r); destroy_buf(color_y); destroy_buf(color_b); destroy_buf(color_tmp);
        destroy_buf(vel_x); destroy_buf(vel_x_tmp);
        destroy_buf(vel_y); destroy_buf(vel_y_tmp);
        destroy_buf(vel_z); destroy_buf(vel_z_tmp); destroy_buf(wetness_tmp);
        destroy_buf(oil_density); destroy_buf(oil_density_tmp);
        destroy_buf(height_field);
        destroy_buf(pressure_a); destroy_buf(pressure_b);
        destroy_buf(divergence_buf);
        destroy_buf(canvas_density); destroy_buf(canvas_color_r);
        destroy_buf(canvas_color_y); destroy_buf(canvas_color_b);
        destroy_buf(canvas_wetness);
        destroy_buf(vertex_buf); destroy_buf(color_buf);
        destroy_buf(bristle_data); destroy_buf(sample_pos); destroy_buf(sample_vel);
            destroy_buf(sample_color); destroy_buf(sample_frame); destroy_buf(lambda_buf);
        destroy_buf(sample_liquid); destroy_buf(sample_liquid_b); destroy_buf(sample_supply); destroy_buf(bristle_input_color_buf);
        destroy_buf(bristle_density); destroy_buf(bristle_vel_x); destroy_buf(bristle_vel_y); destroy_buf(bristle_vel_z);
        destroy_buf(bristle_color_r); destroy_buf(bristle_color_y); destroy_buf(bristle_color_b);
        destroy_buf(ptcl_pos); destroy_buf(ptcl_vel); destroy_buf(ptcl_color);
        destroy_buf(ptcl_alive); destroy_buf(ptcl_counter);
        destroy_buf(ptcl_density); destroy_buf(ptcl_vel_x); destroy_buf(ptcl_vel_y); destroy_buf(ptcl_vel_z);
        destroy_buf(ptcl_rast_r); destroy_buf(ptcl_rast_y); destroy_buf(ptcl_rast_b);
        destroy_buf(vel_x_old); destroy_buf(vel_y_old); destroy_buf(vel_z_old);
        destroy_buf(ptcl_pos_b); destroy_buf(ptcl_vel_b); destroy_buf(ptcl_color_b); destroy_buf(ptcl_alive_b);

        auto destroy_prog = [&](ProgramHandle& h) {
            if (h) { rc.destroy(h); h = nullptr; }
        };
        destroy_prog(deposit_program); destroy_prog(advect_program);
        destroy_prog(jacobi_program); destroy_prog(divergence_program);
        destroy_prog(gradient_program); destroy_prog(damp_dry_program);
        destroy_prog(bristle_sim_program); destroy_prog(bristle_raster_program);
        destroy_prog(bristle_merge_program); destroy_prog(field_clear_program);
        destroy_prog(bristle_density_constraint_program);
        destroy_prog(bristle_resample_program);
        destroy_prog(bri_liquid_transfer_program);
        destroy_prog(bri_liquid_emit_program);
        destroy_prog(ptcl_emit_program); destroy_prog(ptcl_update_program);
        destroy_prog(ptcl_raster_program); destroy_prog(ptcl_flip_pic_program);
        destroy_prog(ptcl_compact_program); destroy_prog(ptcl_to_grid_program);
        destroy_prog(grid_to_ptcl_program);
        destroy_prog(canvas_commit_program);
    }
};

// ============================================================
// Node declaration
// ============================================================

NODE_DECLARATION_FUNCTION(brush_paint_sim)
{
    b.add_input<Geometry>("Brush Strokes");
    b.add_input<int>("Resolution").default_val(512).min(64).max(4096);
    b.add_input<int>("Resolution Z").default_val(32).min(4).max(128);
    b.add_input<float>("Paper Size").default_val(1.0f).min(0.1f).max(10.0f);
    // Fixed canvas domain (AABB). The simulation grid is anchored to this box
    // and NEVER re-anchors or resizes when new strokes arrive — this keeps
    // previously painted cells at fixed world positions instead of being
    // stretched/relabeled when a later stroke touches the boundary. Strokes
    // outside the AABB are simply ignored by the deposit (clamped at the edge).
    // Center defaults to origin; canvas Z is the bottom face of the box (paper
    // surface), paint stacks upward from there.
    b.add_input<float>("Canvas Center X").default_val(0.0f);
    b.add_input<float>("Canvas Center Y").default_val(0.0f);
    b.add_input<float>("Canvas Z").default_val(0.0f);
    b.add_input<float>("Canvas Height").default_val(0.0f)
        .min(0.0f).max(2.0f);  // 0 = auto (isotropic cells)
    b.add_input<float>("Brush Radius").default_val(0.02f).min(0.001f).max(0.5f);
    b.add_input<float>("Brush Pressure").default_val(1.0f).min(0.0f).max(4.0f);
    b.add_input<float>("Ink Amount").default_val(0.8f).min(0.0f).max(2.0f);
    b.add_input<float>("Viscosity").default_val(0.5f).min(0.0f).max(10.0f);
    b.add_input<float>("Oil Density").default_val(0.5f).min(0.0f).max(1.0f);
    b.add_input<float>("Diffusion Rate")
        .default_val(0.0001f).min(0.0f).max(0.01f);
    b.add_input<float>("Pickup Rate").default_val(0.1f).min(0.0f).max(1.0f);
    b.add_input<float>("Drying Rate").default_val(0.1f).min(0.0f).max(2.0f);
    b.add_output<Geometry>("Paint Particles");
    // --- Debug / fidelity statistics (read back from GPU buffers for tests) ---
    // These are full-grid or active-window aggregates exposed as floats so
    // Python tests (see tests/test_brush_sim_fidelity.py) can assert on
    // physical correctness without binding volumetric grid components.
    b.add_output<float>("Max Divergence");   // max|div u| in the active window (post-projection)
    b.add_output<float>("Mean Divergence");  // mean |div u| in the active window
    b.add_output<float>("Total Density");    // sum of grid density (paint mass proxy)
    b.add_output<float>("Total Color R");    // sum of grid RYB-R channel
    b.add_output<float>("Total Color Y");    // sum of grid RYB-Y channel
    b.add_output<float>("Total Color B");    // sum of grid RYB-B channel
    b.add_output<int>("Particle Count");     // number of alive FLIP/PIC particles
    b.add_output<float>("Total Particle Mass"); // sum of alive particle mass (color.w)
}

// ============================================================
// Node execution
// ============================================================

NODE_EXECUTION_FUNCTION(brush_paint_sim)
{
    auto& storage = params.get_storage<PaintSimStorage&>();
    auto& rc = get_resource_allocator();
    auto device = RHI::get_device();

    auto brush_strokes = params.get_input<Geometry>("Brush Strokes");
    int resolution = params.get_input<int>("Resolution");
    int resolution_z = params.get_input<int>("Resolution Z");
    float paper_size = params.get_input<float>("Paper Size");
    glm::vec2 canvas_center_xy(
        params.get_input<float>("Canvas Center X"),
        params.get_input<float>("Canvas Center Y"));
    float canvas_z_input = params.get_input<float>("Canvas Z");
    float canvas_height_input = params.get_input<float>("Canvas Height");
    float brush_radius = params.get_input<float>("Brush Radius");
    float brush_pressure = params.get_input<float>("Brush Pressure");
    float ink_amount = params.get_input<float>("Ink Amount");
    float viscosity = params.get_input<float>("Viscosity");
    float oil_density_in = params.get_input<float>("Oil Density");
    float diffusion = params.get_input<float>("Diffusion Rate");
    float drying_rate = params.get_input<float>("Drying Rate");

    auto make_particles = [&]() -> std::pair<Geometry, PointsComponent*> {
        Geometry geom;
        auto pts = std::make_shared<PointsComponent>(&geom);
        geom.attach_component(pts);
        return {std::move(geom), pts.get()};
    };

    // Zero-fill the debug output ports on early-return paths (no curve /
    // shader failure) so downstream consumers always see all ports set.
    auto emit_zero_debug = [&]() {
        params.set_output("Max Divergence", 0.0f);
        params.set_output("Mean Divergence", 0.0f);
        params.set_output("Total Density", 0.0f);
        params.set_output("Total Color R", 0.0f);
        params.set_output("Total Color Y", 0.0f);
        params.set_output("Total Color B", 0.0f);
        params.set_output("Particle Count", 0);
        params.set_output("Total Particle Mass", 0.0f);
    };

    auto curve = brush_strokes.get_component<CurveComponent>();
    if (!curve || curve->get_vertices().empty()) {
        auto [geom, pts] = make_particles();
        params.set_output("Paint Particles", std::move(geom));
        emit_zero_debug();
        params.set_storage(storage);
        return true;
    }

    auto vertices = curve->get_vertices();
    auto colors = curve->get_display_color();

    // Detect stroke reset (node re-evaluated with fewer vertices than before)
    if (static_cast<int>(vertices.size()) < storage.deposited_count) {
        storage.deposited_count = 0;
        storage.last_sim_time = -1.0f;
        storage.center_initialized = false;
        storage.bristles_initialized = false;
    }

    int already_deposited = storage.deposited_count;
    int new_count = static_cast<int>(vertices.size()) - already_deposited;

    // ---- Stroke recorder (optional) ----
    // When RUZINO_RECORD_STROKE is set, dump the FULL stroke (every point
    // accumulated so far, with timestamps and stroke segmentation) plus all
    // node inputs to a JSON file. The file is overwritten each cook, so the
    // final state of the file after a complete editor run holds the whole
    // trajectory. A headless test can then replay this exact input.
    if (stroke_recorder_enabled()) {
        StrokeRecord rec;
        rec.points = vertices;
        rec.timestamps = curve->get_vertex_scalar_quantity("timestamp");
        rec.stroke_lengths = curve->get_vert_count();
        rec.resolution = resolution;
        rec.resolution_z = resolution_z;
        rec.paper_size = paper_size;
        rec.canvas_center_x = canvas_center_xy.x;
        rec.canvas_center_y = canvas_center_xy.y;
        rec.canvas_z = canvas_z_input;
        rec.canvas_height = canvas_height_input;
        rec.brush_radius = brush_radius;
        rec.brush_pressure = brush_pressure;
        rec.ink_amount = ink_amount;
        rec.viscosity = viscosity;
        rec.oil_density = oil_density_in;
        rec.diffusion = diffusion;
        rec.pickup_rate = params.get_input<float>("Pickup Rate");
        rec.drying_rate = drying_rate;
        // Recover ink color + width from the curve's display_color/width so
        // the replay uses what the editor actually fed in (these come from
        // brush_input, which multiplies by ink_amount).
        if (!colors.empty()) {
            rec.ink_r_ryb = colors[0].x;
            rec.ink_y_ryb = colors[0].y;
            rec.ink_b_ryb = colors[0].z;
        }
        auto widths = curve->get_width();
        if (!widths.empty()) rec.brush_width = widths[0];
        write_stroke_record(rec);
    }

    // ---- Fixed canvas domain (Wetbrush §4.2: "space ABOVE canvas") ----
    // The grid is anchored to a user-specified AABB and NEVER re-anchors or
    // resizes when new strokes arrive. This is the fix for the "domain
    // stretching" symptom: previously the grid was recomputed from the stroke
    // bounding box, so a later stroke touching the boundary enlarged
    // grid_paper and relabeled every already-painted cell to a new world
    // position — visually the whole canvas stretched. Now the AABB is fixed;
    // strokes outside it are simply clamped at deposit time (see brush_deposit).
    // Canvas Z = the bottom face of the box = the paper surface. Paint stacks
    // only upward (z >= 0 in grid space), giving a single-sided paint layer
    // instead of the previous z-mirror image across the stroke plane.
    if (!storage.center_initialized) {
        storage.grid_center = canvas_center_xy;
        // grid_center_z is the CENTER of the Z extent; canvas (paper) is the
        // bottom face. canvas_z_input names the paper surface directly.
        float height = canvas_height_input > 1e-6f
            ? canvas_height_input
            : paper_size * static_cast<float>(resolution_z)
                / static_cast<float>(resolution);  // auto: isotropic Z cells
        storage.grid_height = height;
        storage.grid_center_z = canvas_z_input + height * 0.5f;
        storage.grid_paper = paper_size;
        storage.grid_res = resolution;
        storage.grid_res_z = resolution_z;
        storage.center_initialized = true;

        spdlog::info(
            "brush_paint_sim: grid {}x{}x{}, paper={:.3f}, height={:.3f}, cell={:.5f}, "
            "canvas_z={:.3f}",
            resolution, resolution, resolution_z, storage.grid_paper, storage.grid_height,
            storage.grid_paper / static_cast<float>(resolution), canvas_z_input);
    }

    // Helper: safely destroy a buffer via resource allocator before recreation
    auto safe_destroy_buf = [&](nvrhi::BufferHandle& h) {
        if (h) { rc.destroy(h); h = nullptr; }
    };

    // Create or resize GPU buffers
    int n  = storage.grid_res * storage.grid_res;
    int rz = storage.grid_res_z > 0 ? storage.grid_res_z : resolution_z;
    // 3D fluid fields are allocated at active-WINDOW size (Wetbrush §4.2),
    // NOT full grid. This keeps 3D memory ~constant (~25 fields × 128³×res_z)
    // independent of the canvas resolution. The 2D canvas layer carries the
    // full-resolution committed paint.
    int win_xy = std::min(PaintSimStorage::WIN_ALLOC_XY, storage.grid_res);
    int alloc_win_n3d = win_xy * win_xy * rz;
    int n2d = n;  // 2D canvas layer = full grid XY
    if (storage.grid_alloc_res != storage.grid_res || storage.grid_alloc_res_z != rz) {
        storage.grid_alloc_res = storage.grid_res;
        storage.grid_alloc_res_z = rz;
        storage.win_alloc_z = rz;
        storage.win_origin_set = false;  // reset window tracking on realloc
        storage.deposited_count = 0;
        storage.last_sim_time = -1.0f;
        already_deposited = 0;
        new_count = static_cast<int>(vertices.size());
        storage.bristles_initialized = false;
        storage.particles_initialized = false;

        // Release old grid buffers before creating new ones
        safe_destroy_buf(storage.density);      safe_destroy_buf(storage.density_tmp);
        safe_destroy_buf(storage.color_r);      safe_destroy_buf(storage.color_y);
        safe_destroy_buf(storage.color_b);      safe_destroy_buf(storage.color_tmp);
        safe_destroy_buf(storage.vel_x);        safe_destroy_buf(storage.vel_x_tmp);
        safe_destroy_buf(storage.vel_y);        safe_destroy_buf(storage.vel_y_tmp);
        safe_destroy_buf(storage.vel_z);        safe_destroy_buf(storage.vel_z_tmp);
        safe_destroy_buf(storage.wetness);      safe_destroy_buf(storage.wetness_tmp);
        safe_destroy_buf(storage.oil_density);  safe_destroy_buf(storage.oil_density_tmp);
        safe_destroy_buf(storage.height_field);
        safe_destroy_buf(storage.pressure_a);   safe_destroy_buf(storage.pressure_b);
        safe_destroy_buf(storage.divergence_buf);
        safe_destroy_buf(storage.bristle_density);  safe_destroy_buf(storage.bristle_vel_x);
        safe_destroy_buf(storage.bristle_vel_y);    safe_destroy_buf(storage.bristle_vel_z);
        safe_destroy_buf(storage.bristle_color_r);
        safe_destroy_buf(storage.bristle_color_y);  safe_destroy_buf(storage.bristle_color_b);
        safe_destroy_buf(storage.ptcl_density);     safe_destroy_buf(storage.ptcl_vel_x);
        safe_destroy_buf(storage.ptcl_vel_y);       safe_destroy_buf(storage.ptcl_vel_z);
        safe_destroy_buf(storage.ptcl_rast_r);
        safe_destroy_buf(storage.ptcl_rast_y);     safe_destroy_buf(storage.ptcl_rast_b);
        safe_destroy_buf(storage.vel_x_old);
        safe_destroy_buf(storage.vel_y_old);
        safe_destroy_buf(storage.vel_z_old);
        // Release old canvas layer
        safe_destroy_buf(storage.canvas_density);
        safe_destroy_buf(storage.canvas_color_r);
        safe_destroy_buf(storage.canvas_color_y);
        safe_destroy_buf(storage.canvas_color_b);
        safe_destroy_buf(storage.canvas_wetness);

        auto make_buf = [&](const char* name) -> nvrhi::BufferHandle {
            return create_field_buffer(rc, alloc_win_n3d, name);
        };
        auto make_canvas = [&](const char* name) -> nvrhi::BufferHandle {
            return create_field_buffer(rc, n2d, name);
        };

        storage.density      = make_buf("density");
        storage.density_tmp  = make_buf("density_tmp");
        storage.color_r      = make_buf("color_r");
        storage.color_y      = make_buf("color_y");
        storage.color_b      = make_buf("color_b");
        storage.color_tmp    = make_buf("color_tmp");
        storage.vel_x        = make_buf("vel_x");
        storage.vel_x_tmp    = make_buf("vel_x_tmp");
        storage.vel_y        = make_buf("vel_y");
        storage.vel_y_tmp    = make_buf("vel_y_tmp");
        storage.vel_z        = make_buf("vel_z");
        storage.vel_z_tmp    = make_buf("vel_z_tmp");
        storage.wetness      = make_buf("wetness");
        storage.wetness_tmp  = make_buf("wetness_tmp");
        storage.oil_density  = make_buf("oil_density");
        storage.oil_density_tmp = make_buf("oil_density_tmp");
        storage.height_field = make_buf("height");
        storage.pressure_a   = make_buf("pressure_a");
        storage.pressure_b   = make_buf("pressure_b");
        storage.divergence_buf = make_buf("divergence");

        // Bristle accumulation grids
        storage.bristle_density  = make_buf("bristle_density");
        storage.bristle_vel_x   = make_buf("bristle_vel_x");
        storage.bristle_vel_y   = make_buf("bristle_vel_y");
        storage.bristle_vel_z   = make_buf("bristle_vel_z");
        storage.bristle_color_r = make_buf("bristle_color_r");
        storage.bristle_color_y = make_buf("bristle_color_y");
        storage.bristle_color_b = make_buf("bristle_color_b");

        // Particle accumulation grids + FLIP snapshot
        storage.ptcl_density  = make_buf("ptcl_density");
        storage.ptcl_vel_x    = make_buf("ptcl_vel_x");
        storage.ptcl_vel_y    = make_buf("ptcl_vel_y");
        storage.ptcl_vel_z    = make_buf("ptcl_vel_z");
        storage.ptcl_rast_r  = make_buf("ptcl_rast_r");
        storage.ptcl_rast_y  = make_buf("ptcl_rast_y");
        storage.ptcl_rast_b  = make_buf("ptcl_rast_b");
        storage.vel_x_old     = make_buf("vel_x_old");
        storage.vel_y_old     = make_buf("vel_y_old");
        storage.vel_z_old     = make_buf("vel_z_old");

        // 2D canvas layer (full-resolution, persistent committed paint)
        storage.canvas_density  = make_canvas("canvas_density");
        storage.canvas_color_r  = make_canvas("canvas_color_r");
        storage.canvas_color_y  = make_canvas("canvas_color_y");
        storage.canvas_color_b  = make_canvas("canvas_color_b");
        storage.canvas_wetness  = make_canvas("canvas_wetness");

        // Zero-init all field buffers (3D window + 2D canvas)
        std::vector<float> zeros3d(alloc_win_n3d, 0.0f);
        std::vector<float> zeros2d(n2d, 0.0f);
        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        for (auto* buf : {&storage.density, &storage.density_tmp,
                          &storage.color_r, &storage.color_y, &storage.color_b,
                          &storage.color_tmp, &storage.vel_x, &storage.vel_x_tmp,
                          &storage.vel_y, &storage.vel_y_tmp,
                          &storage.vel_z, &storage.vel_z_tmp,
                          &storage.wetness, &storage.wetness_tmp,
                          &storage.oil_density, &storage.oil_density_tmp,
                          &storage.height_field,
                          &storage.pressure_a, &storage.pressure_b,
                          &storage.divergence_buf,
                          &storage.bristle_density, &storage.bristle_vel_x,
                          &storage.bristle_vel_y, &storage.bristle_vel_z,
                          &storage.bristle_color_r, &storage.bristle_color_y,
                          &storage.bristle_color_b,
                          &storage.ptcl_density, &storage.ptcl_vel_x,
                          &storage.ptcl_vel_y, &storage.ptcl_vel_z,
                          &storage.ptcl_rast_r, &storage.ptcl_rast_y,
                          &storage.ptcl_rast_b,
                          &storage.vel_x_old, &storage.vel_y_old, &storage.vel_z_old}) {
            cmd->writeBuffer(*buf, zeros3d.data(), alloc_win_n3d * sizeof(float));
        }
        // Zero-init the 2D canvas layer
        for (auto* buf : {&storage.canvas_density, &storage.canvas_color_r,
                          &storage.canvas_color_y, &storage.canvas_color_b,
                          &storage.canvas_wetness}) {
            cmd->writeBuffer(*buf, zeros2d.data(), n2d * sizeof(float));
        }
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        rc.destroy(cmd);
    }

    // Initialize bristle buffers
    int Nb = PaintSimStorage::NUM_BRISTLES;
    int M = PaintSimStorage::VERTS_PER_BRISTLE;
    int S = PaintSimStorage::SAMPLES_PER_BRISTLE;

    if (!storage.bristles_initialized) {
        // Release old bristle buffers before creating new ones
        safe_destroy_buf(storage.bristle_data);
        safe_destroy_buf(storage.lambda_buf);
        safe_destroy_buf(storage.sample_pos);
        safe_destroy_buf(storage.sample_vel);
        safe_destroy_buf(storage.sample_color);
        safe_destroy_buf(storage.sample_frame);
        safe_destroy_buf(storage.sample_liquid);
        safe_destroy_buf(storage.sample_liquid_b);
        safe_destroy_buf(storage.bristle_input_color_buf);

        storage.bristle_data  = create_typed_buffer(
            rc, Nb * M, PaintSimStorage::BRISTLE_VERTEX_STRIDE, "bristle_data"); // 2×float4 (pos3+vel3)
        storage.lambda_buf    = create_typed_buffer(
            rc, Nb * M, sizeof(float), "lambda_buf");
        storage.sample_pos   = create_typed_buffer(
            rc, Nb * S, sizeof(float) * 4, "sample_pos");   // float4 (.xyz + pad)
        storage.sample_vel   = create_typed_buffer(
            rc, Nb * S, sizeof(float) * 4, "sample_vel");   // float4 (.xyz + pad)
        storage.sample_color = create_typed_buffer(
            rc, Nb * S, sizeof(float) * 4, "sample_color");
        // Bishop frame: SampleFrame { tangent, normal, binormal } — 3 float4 = 48 bytes
        storage.sample_frame = create_typed_buffer(
            rc, Nb * S, sizeof(float) * 4 * 3, "sample_frame");

        // Bristle liquid state: SampleLiquid {float mass, float3 pigment} per sample
        storage.sample_liquid = create_typed_buffer(
            rc, Nb * S, sizeof(float) * 4, "sample_liquid");

        // Ping-pong buffer for liquid transfer
        storage.sample_liquid_b = create_typed_buffer(
            rc, Nb * S, sizeof(float) * 4, "sample_liquid_b");

        // Per-sample paint supply reservoir (consumed by ABSORB, §5.1).
        // Refilled to ink_amount at the start of each cook (see below).
        storage.sample_supply = create_typed_buffer(
            rc, Nb * S, sizeof(float), "sample_supply");

        // Single-color input for resample (user RYB paint color)
        storage.bristle_input_color_buf = create_typed_buffer(
            rc, 1, sizeof(float) * 4, "bristle_input_color");

        // Zero-init
        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        std::vector<float> zeros_bristle(Nb * M * (PaintSimStorage::BRISTLE_VERTEX_STRIDE / sizeof(float)), 0.0f);
        cmd->writeBuffer(storage.bristle_data, zeros_bristle.data(),
                         zeros_bristle.size() * sizeof(float));
        std::vector<float> zeros_sample(Nb * S * 4, 0.0f);
        cmd->writeBuffer(storage.sample_pos, zeros_sample.data(),
                         Nb * S * sizeof(float) * 4);
        cmd->writeBuffer(storage.sample_vel, zeros_sample.data(),
                         Nb * S * sizeof(float) * 4);
        cmd->writeBuffer(storage.sample_color, zeros_sample.data(),
                         Nb * S * sizeof(float) * 4);
        // sample_frame holds SampleFrame{tangent,normal,binormal} = 3 float4
        // per sample, so it needs a zero buffer 3x the size of zeros_sample.
        // (Using zeros_sample here previously read 3x past its end -> crash.)
        std::vector<float> zeros_frame(Nb * S * 4 * 3, 0.0f);
        cmd->writeBuffer(storage.sample_frame, zeros_frame.data(),
                         Nb * S * sizeof(float) * 4 * 3);
        cmd->writeBuffer(storage.sample_liquid, zeros_sample.data(),
                         Nb * S * sizeof(float) * 4);
        cmd->writeBuffer(storage.sample_liquid_b, zeros_sample.data(),
                         Nb * S * sizeof(float) * 4);

        // Set user paint color (from stroke color, default RYB red)
        glm::vec3 ink_color = (colors.size() > 0)
            ? colors.back() : glm::vec3(1.0f, 0.0f, 0.0f);
        float input_color[4] = { ink_color.r, ink_color.g, ink_color.b, ink_amount };
        cmd->writeBuffer(storage.bristle_input_color_buf, input_color, sizeof(float) * 4);

        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        rc.destroy(cmd);

        storage.bristles_initialized = true;
    }

    // Initialize particle buffers
    int max_ptcl = PaintSimStorage::MAX_PARTICLES;
    if (!storage.particles_initialized) {
        // Release old particle buffers before creating new ones
        safe_destroy_buf(storage.ptcl_pos);      safe_destroy_buf(storage.ptcl_vel);
        safe_destroy_buf(storage.ptcl_color);    safe_destroy_buf(storage.ptcl_alive);
        safe_destroy_buf(storage.ptcl_counter);
        safe_destroy_buf(storage.ptcl_pos_b);    safe_destroy_buf(storage.ptcl_vel_b);
        safe_destroy_buf(storage.ptcl_color_b);  safe_destroy_buf(storage.ptcl_alive_b);

        storage.ptcl_pos    = create_typed_buffer(rc, max_ptcl, sizeof(float) * 4, "ptcl_pos");
        storage.ptcl_vel    = create_typed_buffer(rc, max_ptcl, sizeof(float) * 4, "ptcl_vel");
        storage.ptcl_color  = create_typed_buffer(rc, max_ptcl, sizeof(float) * 4, "ptcl_color");
        storage.ptcl_alive  = create_typed_buffer(rc, max_ptcl, sizeof(uint32_t), "ptcl_alive");
        storage.ptcl_counter = create_byte_buffer(rc, sizeof(uint32_t), "ptcl_counter");
        storage.ptcl_pos_b   = create_typed_buffer(rc, max_ptcl, sizeof(float) * 4, "ptcl_pos_b");
        storage.ptcl_vel_b   = create_typed_buffer(rc, max_ptcl, sizeof(float) * 4, "ptcl_vel_b");
        storage.ptcl_color_b = create_typed_buffer(rc, max_ptcl, sizeof(float) * 4, "ptcl_color_b");
        storage.ptcl_alive_b = create_typed_buffer(rc, max_ptcl, sizeof(uint32_t), "ptcl_alive_b");

        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        std::vector<float> zeros_ptcl(max_ptcl * 4, 0.0f);
        cmd->writeBuffer(storage.ptcl_pos, zeros_ptcl.data(), max_ptcl * sizeof(float) * 3);
        cmd->writeBuffer(storage.ptcl_vel, zeros_ptcl.data(), max_ptcl * sizeof(float) * 3);
        cmd->writeBuffer(storage.ptcl_color, zeros_ptcl.data(), max_ptcl * sizeof(float) * 4);
        std::vector<uint32_t> zeros_u(max_ptcl, 0);
        cmd->writeBuffer(storage.ptcl_alive, zeros_u.data(), max_ptcl * sizeof(uint32_t));
        uint32_t zero_c = 0;
        cmd->writeBuffer(storage.ptcl_counter, &zero_c, sizeof(uint32_t));
        cmd->writeBuffer(storage.ptcl_pos_b, zeros_ptcl.data(), max_ptcl * sizeof(float) * 3);
        cmd->writeBuffer(storage.ptcl_vel_b, zeros_ptcl.data(), max_ptcl * sizeof(float) * 3);
        cmd->writeBuffer(storage.ptcl_color_b, zeros_ptcl.data(), max_ptcl * sizeof(float) * 4);
        cmd->writeBuffer(storage.ptcl_alive_b, zeros_u.data(), max_ptcl * sizeof(uint32_t));
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        rc.destroy(cmd);

        storage.particles_initialized = true;
    }

    // Compile shaders lazily
    if (!storage.deposit_program)
        storage.deposit_program = compile_shader(rc, "brush_deposit.slang");
    if (!storage.advect_program)
        storage.advect_program = compile_shader(rc, "fluid_advect.slang");
    if (!storage.jacobi_program)
        storage.jacobi_program = compile_shader(rc, "fluid_jacobi.slang");
    if (!storage.divergence_program)
        storage.divergence_program = compile_shader(rc, "fluid_divergence.slang");
    if (!storage.gradient_program)
        storage.gradient_program = compile_shader(rc, "fluid_gradient.slang");
    if (!storage.damp_dry_program)
        storage.damp_dry_program = compile_shader(rc, "fluid_damp_dry.slang");

    // Bristle shaders
    if (!storage.bristle_sim_program)
        storage.bristle_sim_program = compile_shader(rc, "bristle_simulate.slang");
    if (!storage.bristle_density_constraint_program)
        storage.bristle_density_constraint_program = compile_shader(rc, "bristle_density_constraint.slang");
    if (!storage.bristle_resample_program)
        storage.bristle_resample_program = compile_shader(rc, "bristle_resample.slang");
    if (!storage.bristle_raster_program)
        storage.bristle_raster_program = compile_shader(rc, "bristle_rasterize.slang");
    if (!storage.bristle_merge_program)
        storage.bristle_merge_program = compile_shader(rc, "bristle_merge.slang");
    if (!storage.field_clear_program)
        storage.field_clear_program = compile_shader(rc, "field_clear.slang");

    // Particle shaders
    if (!storage.ptcl_emit_program)
        storage.ptcl_emit_program = compile_shader(rc, "particle_emit.slang");
    if (!storage.ptcl_update_program)
        storage.ptcl_update_program = compile_shader(rc, "particle_update.slang");
    if (!storage.ptcl_raster_program)
        storage.ptcl_raster_program = compile_shader(rc, "particle_rasterize.slang");
    if (!storage.ptcl_flip_pic_program)
        storage.ptcl_flip_pic_program = compile_shader(rc, "particle_flip_pic.slang");
    if (!storage.ptcl_compact_program)
        storage.ptcl_compact_program = compile_shader(rc, "particle_compact.slang");
    if (!storage.ptcl_to_grid_program)
        storage.ptcl_to_grid_program = compile_shader(rc, "particle_to_grid.slang");
    if (!storage.grid_to_ptcl_program)
        storage.grid_to_ptcl_program = compile_shader(rc, "grid_to_particle.slang");

    // Bristle liquid transfer shaders (Section 5.1)
    if (!storage.bri_liquid_transfer_program)
        storage.bri_liquid_transfer_program = compile_shader(rc, "bristle_liquid_transfer.slang");
    if (!storage.bri_liquid_emit_program)
        storage.bri_liquid_emit_program = compile_shader(rc, "bristle_liquid_emit.slang");

    // Canvas commit shader (2D canvas layer persistence)
    if (!storage.canvas_commit_program)
        storage.canvas_commit_program = compile_shader(rc, "canvas_commit.slang");

    if (!storage.deposit_program || !storage.advect_program ||
        !storage.jacobi_program || !storage.divergence_program ||
        !storage.gradient_program || !storage.damp_dry_program ||
        !storage.bristle_sim_program || !storage.bristle_density_constraint_program ||
        !storage.bristle_resample_program || !storage.bristle_raster_program ||
        !storage.bristle_merge_program || !storage.field_clear_program ||
        !storage.bri_liquid_transfer_program || !storage.bri_liquid_emit_program ||
        !storage.ptcl_emit_program || !storage.ptcl_update_program ||
        !storage.ptcl_raster_program || !storage.ptcl_flip_pic_program ||
        !storage.ptcl_compact_program || !storage.ptcl_to_grid_program ||
        !storage.grid_to_ptcl_program) {
        spdlog::error("brush_paint_sim: shader compilation failed");
        auto [geom, pts] = make_particles();
        params.set_output("Paint Particles", std::move(geom));
        emit_zero_debug();
        params.set_storage(storage);
        return false;
    }

    // === Derive brush transform from latest vertex ===
    float cell_sz = storage.grid_paper / static_cast<float>(storage.grid_res);
    // Effective deposit radius equals the user-specified brush radius exactly.
    // Previously this was max(brush_radius, cell_sz*3), which enlarged thin
    // brushes (e.g. 0.02 became 0.06 when cell_sz≈0.02) — the deposit footprint
    // no longer matched the brush. The minimum-cell-coverage guarantee belongs
    // in the shader (spread_xy = max(1, ceil(r/cell))), not in the world radius.
    float eff_radius = brush_radius;

    glm::vec3 brush_pos_3d(0.0f);
    glm::vec3 brush_vel_3d(0.0f);
    glm::vec3 brush_accel_3d(0.0f);
    float brush_rotation = 0.0f;
    // Brush rotates about the canvas normal (Z axis); the stroke is planar so
    // ω is dominantly z. x/y components stay 0 unless the canvas tilts.
    glm::vec3 brush_angular_vel(0.0f);
    glm::vec3 brush_angular_accel(0.0f);

    const float frame_dt = 0.016f;

    if (!vertices.empty()) {
        int last = static_cast<int>(vertices.size()) - 1;
        brush_pos_3d = vertices[last];
        brush_pos_3d.x -= storage.grid_center.x;
        brush_pos_3d.y -= storage.grid_center.y;

        if (last > 0) {
            glm::vec3 new_vel = vertices[last] - vertices[last - 1];
            // Frame-origin linear acceleration a_B = Δv/Δt (Eq.2 rectilinear).
            brush_accel_3d = (new_vel - storage.prev_brush_vel) / frame_dt;
            brush_vel_3d = new_vel;

            // Heading angle in the XY plane
            brush_rotation = atan2(brush_vel_3d.y, brush_vel_3d.x);
            if (last > 1) {
                float prev_rot = atan2(
                    vertices[last-1].y - vertices[last-2].y,
                    vertices[last-1].x - vertices[last-2].x);
                // Angular velocity about canvas normal (Z), in rad/s, wrapped
                // to [-π, π] to handle the ±π seam.
                float dtheta = brush_rotation - prev_rot;
                dtheta = atan2(sin(dtheta), cos(dtheta));
                glm::vec3 new_omega(0.0f, 0.0f, dtheta / frame_dt);
                // ω̇ = Δω/Δt for Eq.2 Euler angular term.
                brush_angular_accel = (new_omega - storage.prev_angular_vel) / frame_dt;
                brush_angular_vel = new_omega;
            }
        }
    }
    storage.prev_brush_vel = brush_vel_3d;
    storage.prev_angular_vel = brush_angular_vel;

    // === ACTIVE WINDOW ORIGIN (Wetbrush §4.2) ===
    // Compute the window origin for THIS frame, centered on the brush and
    // clamped to the canvas. The 3D buffers are allocated at fixed window size
    // (WIN_ALLOC_XY × WIN_ALLOC_XY × res_z), so only cells inside the window
    // are live; everything else lives in the committed 2D canvas layer.
    //
    // Window tracking: when the origin moves, the OLD window region must be
    // committed to the canvas layer and cleared BEFORE moving, otherwise the
    // paint there is lost (overwritten by the next frame's new region). On the
    // very first frame (win_origin_set == false) no commit is needed.
    const int WIN_XY = std::min(PaintSimStorage::WIN_ALLOC_XY, storage.grid_res);
    const int WIN_Z  = rz;
    const int win_n3d = WIN_XY * WIN_XY * WIN_Z;
    {
        float half_p = storage.grid_paper * 0.5f;
        float bgx = (brush_pos_3d.x - storage.grid_center.x + half_p) / cell_sz;
        float bgy = (brush_pos_3d.y - storage.grid_center.y + half_p) / cell_sz;
        int new_wox = static_cast<int>(bgx) - WIN_XY / 2;
        int new_woy = static_cast<int>(bgy) - WIN_XY / 2;
        new_wox = std::max(0, std::min(new_wox, storage.grid_res - WIN_XY));
        new_woy = std::max(0, std::min(new_woy, storage.grid_res - WIN_XY));

        // If the window moved (or first frame), commit old region then update.
        bool moved = !storage.win_origin_set
                  || new_wox != storage.win_origin_x
                  || new_woy != storage.win_origin_y;
        if (moved && storage.win_origin_set) {
            // Commit the current window's Z-columns into the 2D canvas layer,
            // using the OLD origin (still in storage). This bakes the live 3D
            // paint into the persistent canvas before the window slides away.
            SimConstants commit_cb = {};
            commit_cb.res = storage.grid_res;
            commit_cb.cell_size = cell_sz;
            commit_cb.paper_size = storage.grid_paper;
            commit_cb.res_z = rz;
            commit_cb.height_extent = storage.grid_height;
            commit_cb.grid_center_z = storage.grid_center_z;
            commit_cb.window_origin_x = storage.win_origin_x;
            commit_cb.window_origin_y = storage.win_origin_y;
            commit_cb.window_origin_z = 0;
            commit_cb.window_size_x = WIN_XY;
            commit_cb.window_size_y = WIN_XY;
            commit_cb.window_size_z = WIN_Z;

            nvrhi::BufferHandle commit_cb_buf;
            upload_constant_buffer(rc, device, &commit_cb, sizeof(SimConstants),
                                   "commit_cb", commit_cb_buf);
            ProgramVars cv(rc, storage.canvas_commit_program);
            cv["cb"] = commit_cb_buf.Get();
            cv["density"]  = storage.density.Get();
            cv["color_r"]  = storage.color_r.Get();
            cv["color_y"]  = storage.color_y.Get();
            cv["color_b"]  = storage.color_b.Get();
            cv["wetness"]  = storage.wetness.Get();
            cv["canvas_density"] = storage.canvas_density.Get();
            cv["canvas_color_r"] = storage.canvas_color_r.Get();
            cv["canvas_color_y"] = storage.canvas_color_y.Get();
            cv["canvas_color_b"] = storage.canvas_color_b.Get();
            cv["canvas_wetness"] = storage.canvas_wetness.Get();
            cv.finish_setting_vars();
            ComputeContext cctx(rc, cv);
            cctx.finish_setting_pso();
            cctx.begin();
            cctx.dispatch({}, cv, WIN_XY * WIN_XY, 256);  // one thread per column
            cctx.finish();
            rc.destroy(commit_cb_buf);

            // Clear the 3D window fields so the new region starts empty.
            auto clr = rc.create(CommandListDesc{});
            clr->open();
            std::vector<float> z(win_n3d, 0.0f);
            for (auto* b : {&storage.density, &storage.color_r, &storage.color_y,
                            &storage.color_b, &storage.wetness, &storage.oil_density,
                            &storage.height_field, &storage.vel_x, &storage.vel_y,
                            &storage.vel_z})
                clr->writeBuffer(*b, z.data(), win_n3d * sizeof(float));
            clr->close();
            device->executeCommandList(clr);
            device->waitForIdle();
            rc.destroy(clr);
        }
        storage.win_origin_x = new_wox;
        storage.win_origin_y = new_woy;
        storage.win_origin_z = 0;
        storage.win_origin_set = true;
    }

    // === BRISTLE SIMULATION ===
    {
        // Refill the per-sample paint supply reservoir to ink_amount — the
        // brush is "re-dipped" each frame. ABSORB consumes it (§5.1), so the
        // supply is finite within a frame and mass is conserved.
        {
            std::vector<float> supply(Nb * S, ink_amount);
            auto cmd = rc.create(CommandListDesc{});
            cmd->open();
            cmd->writeBuffer(storage.sample_supply, supply.data(),
                             supply.size() * sizeof(float));
            cmd->close();
            device->executeCommandList(cmd);
            device->waitForIdle();
            rc.destroy(cmd);
        }

        BristleConstants bc = {};
        bc.num_bristles = Nb;
        bc.verts_per_bristle = M;
        bc.samples_per_bristle = S;
        bc.beta_B = 0.05f;
        bc.dt = 0.016f;
        bc.brush_pos_x = brush_pos_3d.x;
        bc.brush_pos_y = brush_pos_3d.y;
        bc.brush_pos_z = brush_pos_3d.z;
        bc.brush_vel_x = brush_vel_3d.x;
        bc.brush_vel_y = brush_vel_3d.y;
        bc.brush_vel_z = brush_vel_3d.z;
        bc.brush_angular_vel_x = brush_angular_vel.x;
        bc.brush_angular_vel_y = brush_angular_vel.y;
        bc.brush_angular_vel_z = brush_angular_vel.z;
        bc.brush_rotation = brush_rotation;
        bc.brush_accel_x = brush_accel_3d.x;
        bc.brush_accel_y = brush_accel_3d.y;
        bc.brush_accel_z = brush_accel_3d.z;
        bc.brush_angular_accel_x = brush_angular_accel.x;
        bc.brush_angular_accel_y = brush_angular_accel.y;
        bc.brush_angular_accel_z = brush_angular_accel.z;
        bc.brush_pressure = brush_pressure;
        bc.canvas_z = storage.grid_center_z - storage.grid_height * 0.5f;
        bc.brush_radius = brush_radius;
        bc.spring_k = 50.0f;
        bc.damping = 5.0f;
        bc.grid_res = storage.grid_res;
        bc.grid_res_z = rz;
        bc.height_extent = storage.grid_height;
        bc.grid_center_z = storage.grid_center_z;
        bc.cell_size = cell_sz;
        bc.paper_size = storage.grid_paper;
        bc.grid_center_x = storage.grid_center.x;
        bc.grid_center_y = storage.grid_center.y;
        bc.window_origin_x = storage.win_origin_x;
        bc.window_origin_y = storage.win_origin_y;
        bc.window_origin_z = 0;
        bc.window_size_x = WIN_XY;
        bc.window_size_z = WIN_Z;

        nvrhi::BufferHandle bristle_cb;
        upload_constant_buffer(rc, device, &bc, sizeof(BristleConstants),
                               "bristle_cb", bristle_cb);

        // Step 1: Bristle spring dynamics
        dispatch_raw(rc, storage.bristle_sim_program,
            {},
            {{"bristle_data", storage.bristle_data}},
            bristle_cb, Nb);

        // Step 2: Density constraint (PBF, Macklin & Müller 2013)
        // Paper: "estimate the bristle vertex density at each vertex
        // using a smoothed kernel function, and then enforce a minimal
        // density constraint" — iterate 3 times.
        int total_verts = Nb * M;
        for (int dc_iter = 0; dc_iter < 3; dc_iter++) {
            // Pass 0: compute density and λ
            {
                ConstraintModeCB mode0 = {0, {0,0,0}};
                nvrhi::BufferHandle mode0_cb;
                upload_constant_buffer(rc, device, &mode0, sizeof(ConstraintModeCB),
                                       "dc_mode0_cb", mode0_cb);

                ProgramVars v0(rc, storage.bristle_density_constraint_program);
                v0["cb"] = bristle_cb.Get();
                v0["bristle_data"] = storage.bristle_data.Get();
                v0["lambda_buf"] = storage.lambda_buf.Get();
                v0["mode_cb"] = mode0_cb.Get();
                v0.finish_setting_vars();
                ComputeContext c0(rc, v0);
                c0.finish_setting_pso();
                c0.begin();
                c0.dispatch({}, v0, total_verts, 256);
                c0.finish();

                rc.destroy(mode0_cb);
            }

            // Pass 1: apply position correction using λ
            {
                ConstraintModeCB mode1 = {1, {0,0,0}};
                nvrhi::BufferHandle mode1_cb;
                upload_constant_buffer(rc, device, &mode1, sizeof(ConstraintModeCB),
                                       "dc_mode1_cb", mode1_cb);

                ProgramVars v1(rc, storage.bristle_density_constraint_program);
                v1["cb"] = bristle_cb.Get();
                v1["bristle_data"] = storage.bristle_data.Get();
                v1["lambda_buf"] = storage.lambda_buf.Get();
                v1["mode_cb"] = mode1_cb.Get();
                v1.finish_setting_vars();
                ComputeContext c1(rc, v1);
                c1.finish_setting_pso();
                c1.begin();
                c1.dispatch({}, v1, total_verts, 256);
                c1.finish();

                rc.destroy(mode1_cb);
            }
        }

        // Step 3: Resample bristle chains → samples (with user paint color)
        dispatch_raw(rc, storage.bristle_resample_program,
            {{"bristle_data", storage.bristle_data},
             {"bristle_input_color", storage.bristle_input_color_buf}},
            {{"sample_pos", storage.sample_pos},
             {"sample_vel", storage.sample_vel},
             {"sample_color", storage.sample_color},
             {"sample_frame", storage.sample_frame}},
            bristle_cb, Nb);

        // Step 4: Clear bristle accumulation grids
        dispatch_field(rc, storage.field_clear_program,
            {},
            {{"field", storage.bristle_density}}, nullptr, win_n3d);
        dispatch_field(rc, storage.field_clear_program,
            {},
            {{"field", storage.bristle_vel_x}}, nullptr, win_n3d);
        dispatch_field(rc, storage.field_clear_program,
            {},
            {{"field", storage.bristle_vel_y}}, nullptr, win_n3d);
        dispatch_field(rc, storage.field_clear_program,
            {},
            {{"field", storage.bristle_vel_z}}, nullptr, win_n3d);
        dispatch_field(rc, storage.field_clear_program,
            {},
            {{"field", storage.bristle_color_r}}, nullptr, win_n3d);
        dispatch_field(rc, storage.field_clear_program,
            {},
            {{"field", storage.bristle_color_y}}, nullptr, win_n3d);
        dispatch_field(rc, storage.field_clear_program,
            {},
            {{"field", storage.bristle_color_b}}, nullptr, win_n3d);

        // Step 5: Rasterize samples → accumulation grids
        dispatch_raw(rc, storage.bristle_raster_program,
            {{"sample_pos", storage.sample_pos},
             {"sample_color", storage.sample_color},
             {"sample_vel", storage.sample_vel}},
            {{"bristle_density", storage.bristle_density},
             {"bristle_vel_x", storage.bristle_vel_x},
             {"bristle_vel_y", storage.bristle_vel_y},
             {"bristle_vel_z", storage.bristle_vel_z},
             {"bristle_color_r", storage.bristle_color_r},
             {"bristle_color_y", storage.bristle_color_y},
             {"bristle_color_b", storage.bristle_color_b}},
            bristle_cb, Nb * S);

        // Step 6: Merge bristle grids into main simulation grids
        nvrhi::BufferHandle merge_cb;
        SimConstants mc = {};
        mc.res = storage.grid_res;
        mc.res_z = rz;
        mc.height_extent = storage.grid_height;
        mc.grid_center_z = storage.grid_center_z;
        mc.cell_size = cell_sz;
        mc.paper_size = storage.grid_paper;
        mc.ink_amount = ink_amount;
        mc.oil_density_base = oil_density_in;
        mc.window_origin_x = storage.win_origin_x;
        mc.window_origin_y = storage.win_origin_y;
        mc.window_origin_z = 0;
        mc.window_size_x = WIN_XY;
        mc.window_size_y = WIN_XY;
        mc.window_size_z = WIN_Z;
        upload_constant_buffer(rc, device, &mc, sizeof(SimConstants),
                               "merge_cb", merge_cb);

        dispatch_raw(rc, storage.bristle_merge_program,
            {{"bristle_density", storage.bristle_density},
             {"bristle_vel_x", storage.bristle_vel_x},
             {"bristle_vel_y", storage.bristle_vel_y},
             {"bristle_vel_z", storage.bristle_vel_z},
             {"bristle_color_r", storage.bristle_color_r},
             {"bristle_color_y", storage.bristle_color_y},
             {"bristle_color_b", storage.bristle_color_b}},
            {{"density", storage.density},
             {"color_r", storage.color_r},
             {"color_y", storage.color_y},
             {"color_b", storage.color_b},
             {"vel_x", storage.vel_x},
             {"vel_y", storage.vel_y},
             {"vel_z", storage.vel_z},
             {"wetness", storage.wetness},
             {"oil_density", storage.oil_density}},
            merge_cb, win_n3d);

        rc.destroy(bristle_cb);
        rc.destroy(merge_cb);

        // ================================================================
        // Step 7: Bristle-particle liquid transfer (Section 5.1)
        // ABSORB: grid → sample (absorb paint capacity), then
        // EMIT: sample → particles (release if over capacity)
        // Uses ping-pong on sample_liquid buffer (in-place for now)
        // ================================================================
        if (storage.particles_initialized) {
            BristleLiquidConstants blc = {};
            blc.num_bristles = Nb;
            blc.samples_per_bristle = S;
            blc.mu = 0.5f;
            blc.M_max = 2.0f;
            blc.M_min = 0.1f;
            blc.rho_0 = 1e3f;
            blc.eps_emit = 0.1f;
            blc.max_emit_per_step = 10;
            blc.grid_res = storage.grid_res;
            blc.grid_res_z = rz;
            blc.height_extent = storage.grid_height;
            blc.grid_center_z = storage.grid_center_z;
            blc.cell_size = cell_sz;
            blc.paper_size = storage.grid_paper;
            blc.grid_center_x = storage.grid_center.x;
            blc.grid_center_y = storage.grid_center.y;
            blc.D0 = brush_radius * 3.0f;
            blc.max_particles = max_ptcl;
            blc.window_origin_x = storage.win_origin_x;
            blc.window_origin_y = storage.win_origin_y;
            blc.window_origin_z = 0;
            blc.window_size_x = WIN_XY;
            blc.window_size_z = WIN_Z;

            nvrhi::BufferHandle liquid_cb;
            upload_constant_buffer(rc, device, &blc, sizeof(BristleLiquidConstants),
                                   "liquid_cb", liquid_cb);

            // Pass 0: ABSORB (paint supply → sample using Eq.12/13 capacity from ψ)
            // Use ping-pong: sample_liquid (SRV) → sample_liquid_b (UAV)
            dispatch_raw(rc, storage.bri_liquid_transfer_program,
                {{"sample_pos", storage.sample_pos},
                 {"sample_color", storage.sample_color},
                 {"sample_liquid_in", storage.sample_liquid},
                 {"bristle_psi", storage.bristle_density},
                 {"grid_density", storage.density},
                 {"grid_color_r", storage.color_r},
                 {"grid_color_y", storage.color_y},
                 {"grid_color_b", storage.color_b}},
                {{"sample_liquid_out", storage.sample_liquid_b},
                 {"sample_supply", storage.sample_supply}},
                liquid_cb, Nb * S);
            std::swap(storage.sample_liquid, storage.sample_liquid_b);

            // Pass 1: EMIT (sample → particles, hemisphere pattern)
            // Now sample_liquid has ABSORB result, write to _b
            reset_counter(rc, device, storage.ptcl_counter);
            dispatch_raw(rc, storage.bri_liquid_emit_program,
                {{"sample_pos", storage.sample_pos},
                 {"sample_color", storage.sample_color},
                 {"sample_vel", storage.sample_vel},
                 {"sample_liquid_in", storage.sample_liquid},
                 {"bristle_psi", storage.bristle_density},
                 {"grid_density", storage.density},
                 {"grid_color_r", storage.color_r},
                 {"grid_color_y", storage.color_y},
                 {"grid_color_b", storage.color_b}},
                {{"sample_liquid_out", storage.sample_liquid_b},
                 {"sample_supply", storage.sample_supply},
                 {"ptcl_counter", storage.ptcl_counter},
                 {"ptcl_pos_out", storage.ptcl_pos},
                 {"ptcl_vel_out", storage.ptcl_vel},
                 {"ptcl_color_out", storage.ptcl_color},
                 {"ptcl_alive_out", storage.ptcl_alive}},
                liquid_cb, Nb * S);
            std::swap(storage.sample_liquid, storage.sample_liquid_b);

            rc.destroy(liquid_cb);
        }
    }

    // === PARTICLE EMIT + UPDATE ===
    if (storage.particles_initialized && new_count > 0) {
        ParticleConstants pc = {};
        pc.max_particles = max_ptcl;
        pc.dt = 0.016f;
        pc.D0 = brush_radius * 3.0f;
        // Eq.8 friction range δ (paper Table 1: δ = 1/0.2 cm). Our world units
        // are scaled by D0, so set δ = 5/D0 → active band = D0/5, matching the
        // paper's 0.2 cm band relative to D0 = 1 cm.
        pc.friction_delta = 5.0f / pc.D0;
        pc.flip_gamma = 0.8f;
        pc.grid_res = storage.grid_res;
        pc.grid_res_z = rz;
        pc.height_extent = storage.grid_height;
        pc.grid_center_z = storage.grid_center_z;
        pc.cell_size = cell_sz;
        pc.paper_size = storage.grid_paper;
        pc.grid_center_x = storage.grid_center.x;
        pc.grid_center_y = storage.grid_center.y;
        pc.window_origin_x = storage.win_origin_x;
        pc.window_origin_y = storage.win_origin_y;
        pc.window_origin_z = 0;
        pc.window_size_x = WIN_XY;
        pc.window_size_z = WIN_Z;
        pc.brush_pos_x = brush_pos_3d.x;
        pc.brush_pos_y = brush_pos_3d.y;
        pc.brush_pos_z = brush_pos_3d.z;
        pc.brush_radius = brush_radius;
        pc.D1 = brush_radius * 0.9f;  // D1/D0 ≈ 0.3 (paper Table 1: 0.3cm/1cm)
        pc.num_bristles = Nb;
        pc.samples_per_bristle = S;
        pc.brush_accel_x = brush_accel_3d.x;
        pc.brush_accel_y = brush_accel_3d.y;
        pc.brush_accel_z = brush_accel_3d.z;
        pc.brush_angular_accel_x = brush_angular_accel.x;
        pc.brush_angular_accel_y = brush_angular_accel.y;
        pc.brush_angular_accel_z = brush_angular_accel.z;

        nvrhi::BufferHandle ptcl_cb;
        upload_constant_buffer(rc, device, &pc, sizeof(ParticleConstants),
                               "ptcl_cb", ptcl_cb);

        // Reset particle counter before emission
        reset_counter(rc, device, storage.ptcl_counter);

        // Emit from bristle samples (mode 0)
        pc.emit_mode = 0;
        nvrhi::BufferHandle emit0_cb;
        upload_constant_buffer(rc, device, &pc, sizeof(ParticleConstants),
                               "emit0_cb", emit0_cb);
        dispatch_raw(rc, storage.ptcl_emit_program,
            {{"sample_pos", storage.sample_pos},
             {"sample_color", storage.sample_color},
             {"density", storage.density}},
            {{"ptcl_counter", storage.ptcl_counter},
             {"ptcl_pos", storage.ptcl_pos},
             {"ptcl_vel", storage.ptcl_vel},
             {"ptcl_color", storage.ptcl_color},
             {"ptcl_alive", storage.ptcl_alive}},
            emit0_cb, Nb * S);
        rc.destroy(emit0_cb);

        // Emit from grid cells (mode 1)
        // Bind all declared SRVs even if mode 1 doesn't use sample_pos/sample_color
        pc.emit_mode = 1;
        nvrhi::BufferHandle emit1_cb;
        upload_constant_buffer(rc, device, &pc, sizeof(ParticleConstants),
                               "emit1_cb", emit1_cb);
        dispatch_raw(rc, storage.ptcl_emit_program,
            {{"sample_pos", storage.sample_pos},
             {"sample_color", storage.sample_color},
             {"density", storage.density}},
            {{"ptcl_counter", storage.ptcl_counter},
             {"ptcl_pos", storage.ptcl_pos},
             {"ptcl_vel", storage.ptcl_vel},
             {"ptcl_color", storage.ptcl_color},
             {"ptcl_alive", storage.ptcl_alive}},
            emit1_cb, win_n3d);
        rc.destroy(emit1_cb);

        // Update particles (ping-pong)
        dispatch_raw(rc, storage.ptcl_update_program,
            {{"ptcl_pos", storage.ptcl_pos},
             {"ptcl_vel", storage.ptcl_vel},
             {"ptcl_color", storage.ptcl_color},
             {"ptcl_alive", storage.ptcl_alive},
             {"sample_pos", storage.sample_pos},
             {"sample_frame", storage.sample_frame},
             {"grid_vel_x", storage.vel_x},
             {"grid_vel_y", storage.vel_y},
             {"grid_vel_z", storage.vel_z}},
            {{"ptcl_pos_out", storage.ptcl_pos_b},
             {"ptcl_vel_out", storage.ptcl_vel_b},
             {"ptcl_alive_out", storage.ptcl_alive_b}},
            ptcl_cb, max_ptcl);
        std::swap(storage.ptcl_pos, storage.ptcl_pos_b);
        std::swap(storage.ptcl_vel, storage.ptcl_vel_b);
        std::swap(storage.ptcl_alive, storage.ptcl_alive_b);

        // Clear particle accum grids (density + velocity + color)
        dispatch_field(rc, storage.field_clear_program,
            {}, {{"field", storage.ptcl_density}}, nullptr, win_n3d);
        dispatch_field(rc, storage.field_clear_program,
            {}, {{"field", storage.ptcl_vel_x}}, nullptr, win_n3d);
        dispatch_field(rc, storage.field_clear_program,
            {}, {{"field", storage.ptcl_vel_y}}, nullptr, win_n3d);
        dispatch_field(rc, storage.field_clear_program,
            {}, {{"field", storage.ptcl_vel_z}}, nullptr, win_n3d);
        dispatch_field(rc, storage.field_clear_program,
            {}, {{"field", storage.ptcl_rast_r}}, nullptr, win_n3d);
        dispatch_field(rc, storage.field_clear_program,
            {}, {{"field", storage.ptcl_rast_y}}, nullptr, win_n3d);
        dispatch_field(rc, storage.field_clear_program,
            {}, {{"field", storage.ptcl_rast_b}}, nullptr, win_n3d);

        // Rasterize particles (density + velocity + RYB color)
        dispatch_raw(rc, storage.ptcl_raster_program,
            {{"ptcl_pos", storage.ptcl_pos},
             {"ptcl_color", storage.ptcl_color},
             {"ptcl_vel", storage.ptcl_vel},
             {"ptcl_alive", storage.ptcl_alive}},
            {{"ptcl_density", storage.ptcl_density},
             {"ptcl_vel_x", storage.ptcl_vel_x},
             {"ptcl_vel_y", storage.ptcl_vel_y},
             {"ptcl_vel_z", storage.ptcl_vel_z},
             {"ptcl_color_r", storage.ptcl_rast_r},
             {"ptcl_color_y", storage.ptcl_rast_y},
             {"ptcl_color_b", storage.ptcl_rast_b}},
            ptcl_cb, max_ptcl);

        // Merge particle grids into main grids (reuse bristle merge logic)
        nvrhi::BufferHandle merge_cb;
        SimConstants mc2 = {};
        mc2.res = storage.grid_res;
        mc2.cell_size = cell_sz;
        mc2.paper_size = storage.grid_paper;
        mc2.ink_amount = ink_amount;
        mc2.oil_density_base = oil_density_in;
        mc2.window_origin_x = storage.win_origin_x;
        mc2.window_origin_y = storage.win_origin_y;
        mc2.window_origin_z = 0;
        mc2.window_size_x = WIN_XY;
        mc2.window_size_y = WIN_XY;
        mc2.window_size_z = WIN_Z;
        upload_constant_buffer(rc, device, &mc2, sizeof(SimConstants),
                               "ptcl_merge_cb", merge_cb);

        dispatch_raw(rc, storage.bristle_merge_program,
            {{"bristle_density", storage.ptcl_density},
             {"bristle_vel_x", storage.ptcl_vel_x},
             {"bristle_vel_y", storage.ptcl_vel_y},
             {"bristle_vel_z", storage.ptcl_vel_z},
             {"bristle_color_r", storage.ptcl_rast_r},
             {"bristle_color_y", storage.ptcl_rast_y},
             {"bristle_color_b", storage.ptcl_rast_b}},
            {{"density", storage.density},
             {"color_r", storage.color_r},
             {"color_y", storage.color_y},
             {"color_b", storage.color_b},
             {"vel_x", storage.vel_x},
             {"vel_y", storage.vel_y},
             {"vel_z", storage.vel_z},
             {"wetness", storage.wetness},
             {"oil_density", storage.oil_density}},
            merge_cb, win_n3d);
        rc.destroy(merge_cb);

        rc.destroy(ptcl_cb);
    }

    // === DEPOSIT remaining vertices (fallback for non-bristle) ===
    // DISABLED: this curve-vertex deposit path is not in the Wetbrush paper.
    // The paper (§4.1 line 118) deposits paint purely via bristle samples
    // rasterized into the grid ("we rasterize them into a density field").
    // This curve path was a simplification that caused two problems:
    //   1. "Zebra" gaps between sparse stroke samples (fixed with path-fill,
    //      but path-fill is itself non-physical).
    //   2. Double-deposit when running alongside the bristle rasterize→merge
    //      path (Steps 5-6 above), which is the physically correct source.
    // Bristle deposit is now the sole source of paint (per the paper).
    // Commented out via block comment to preserve the code for reference /
    // easy re-enable during debugging. If bristle output is insufficient,
    // re-enable this block and investigate bristle contact density.
    /*
    if (new_count > 0) {
        std::vector<float> vert_data(new_count * 4);
        std::vector<float> col_data(new_count * 4);
        const auto& timestamps = curve->get_vertex_scalar_quantity("timestamp");

        for (int i = 0; i < new_count; i++) {
            int vi = already_deposited + i;
            const auto& v = vertices[vi];
            vert_data[i * 4 + 0] = v.x;
            vert_data[i * 4 + 1] = v.y;
            vert_data[i * 4 + 2] = v.z;
            vert_data[i * 4 + 3] = (vi < static_cast<int>(timestamps.size()))
                ? timestamps[vi] : static_cast<float>(vi) / 60.0f;

            glm::vec3 ink = (vi < static_cast<int>(colors.size()))
                ? colors[vi] : glm::vec3(1, 0, 0);
            col_data[i * 4 + 0] = ink.r;
            col_data[i * 4 + 1] = ink.g;
            col_data[i * 4 + 2] = ink.b;
            col_data[i * 4 + 3] = 0.0f;
        }

        if (storage.vertex_buf)
            rc.destroy(storage.vertex_buf);
        if (storage.color_buf)
            rc.destroy(storage.color_buf);

        storage.vertex_buf = rc.create(nvrhi::BufferDesc{}
            .setByteSize(vert_data.size() * sizeof(float))
            .setStructStride(sizeof(float) * 4)
            .setInitialState(nvrhi::ResourceStates::ShaderResource)
            .setKeepInitialState(true)
            .setCanHaveTypedViews(true)
            .setDebugName("deposit_vertices"));

        storage.color_buf = rc.create(nvrhi::BufferDesc{}
            .setByteSize(col_data.size() * sizeof(float))
            .setStructStride(sizeof(float) * 4)
            .setInitialState(nvrhi::ResourceStates::ShaderResource)
            .setKeepInitialState(true)
            .setCanHaveTypedViews(true)
            .setDebugName("deposit_colors"));

        auto upload_cmd = rc.create(CommandListDesc{});
        upload_cmd->open();
        upload_cmd->writeBuffer(storage.vertex_buf, vert_data.data(),
                                vert_data.size() * sizeof(float));
        upload_cmd->writeBuffer(storage.color_buf, col_data.data(),
                                col_data.size() * sizeof(float));
        upload_cmd->close();
        device->executeCommandList(upload_cmd);
        device->waitForIdle();
        rc.destroy(upload_cmd);

        SimConstants dep_cb = {};
        dep_cb.res = storage.grid_res;
        dep_cb.res_z = rz;
        dep_cb.height_extent = storage.grid_height;
        dep_cb.grid_center_z = storage.grid_center_z;
        dep_cb.cell_size = cell_sz;
        dep_cb.paper_size = storage.grid_paper;
        dep_cb.dt = 0.0f;
        dep_cb.ink_amount = ink_amount;
        dep_cb.num_vertices = new_count;
        dep_cb.center_x = storage.grid_center.x;
        dep_cb.center_y = storage.grid_center.y;
        dep_cb.center_z = storage.grid_center_z;
        dep_cb.effective_radius = eff_radius;
        dep_cb.oil_density_base = oil_density_in;
        dep_cb.window_origin_x = storage.win_origin_x;
        dep_cb.window_origin_y = storage.win_origin_y;
        dep_cb.window_origin_z = 0;
        dep_cb.window_size_x = WIN_XY;
        dep_cb.window_size_y = WIN_XY;
        dep_cb.window_size_z = WIN_Z;

        nvrhi::BufferHandle dep_cb_buf;
        upload_constant_buffer(rc, device, &dep_cb, sizeof(SimConstants),
                               "dep_cb", dep_cb_buf);

        ProgramVars vars(rc, storage.deposit_program);
        vars["cb"] = dep_cb_buf.Get();
        vars["vertices"] = storage.vertex_buf.Get();
        vars["colors"] = storage.color_buf.Get();
        vars["density"] = storage.density.Get();
        vars["color_r"] = storage.color_r.Get();
        vars["color_y"] = storage.color_y.Get();
        vars["color_b"] = storage.color_b.Get();
        vars["vel_x"] = storage.vel_x.Get();
        vars["vel_y"] = storage.vel_y.Get();
        vars["vel_z"] = storage.vel_z.Get();
        vars["wetness"] = storage.wetness.Get();
        vars["oil_density"] = storage.oil_density.Get();
        vars["height"] = storage.height_field.Get();
        vars.finish_setting_vars();

        ComputeContext ctx(rc, vars);
        ctx.finish_setting_pso();
        ctx.begin();
        ctx.dispatch({}, vars, 1, 1);
        ctx.finish();

        rc.destroy(dep_cb_buf);
    }
    */

    storage.deposited_count = static_cast<int>(vertices.size());

    // === FLUID SIMULATION ===
    const auto& timestamps = curve->get_vertex_scalar_quantity("timestamp");
    auto get_time = [&](int i) -> float {
        if (i < static_cast<int>(timestamps.size()))
            return timestamps[i];
        return static_cast<float>(i) / 60.0f;
    };

    float current_time = !vertices.empty() ? get_time(0) : 0.0f;
    for (const auto& v : vertices) {
        int i = static_cast<int>(&v - &vertices[0]);
        current_time = std::max(current_time, get_time(i));
    }

    float sim_dt = 0.0f;
    if (storage.last_sim_time < 0.0f)
        sim_dt = std::min(current_time, 0.05f);
    else
        sim_dt = std::min(current_time - storage.last_sim_time, 0.05f);
    sim_dt = std::max(sim_dt, 0.0f);
    storage.last_sim_time = current_time;

    // --- Active window origin/size were computed above (early binding) ---
    // Reuse storage.win_origin_* and WIN_XY/WIN_Z. wox/woy aliases for the
    // fluid-sim block below.
    int wox = storage.win_origin_x;
    int woy = storage.win_origin_y;

    if (sim_dt > 1e-6f) {  // TEMP reverted: fluid re-enabled
        float max_sub_dt = 2.0f / static_cast<float>(storage.grid_res);
        int substeps = std::max(1, static_cast<int>(std::ceil(sim_dt / max_sub_dt)));
        substeps = std::min(substeps, 16);
        float sub_dt = sim_dt / static_cast<float>(substeps);

        int window_total = WIN_XY * WIN_XY * WIN_Z;

        for (int s = 0; s < substeps; s++) {
            SimConstants fluid_cb = {};
            fluid_cb.res = storage.grid_res;
            fluid_cb.res_z = rz;
            fluid_cb.height_extent = storage.grid_height;
            fluid_cb.grid_center_z = storage.grid_center_z;
            fluid_cb.cell_size = cell_sz;
            fluid_cb.paper_size = storage.grid_paper;
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

            nvrhi::BufferHandle cb_buf;
            upload_constant_buffer(rc, device, &fluid_cb, sizeof(SimConstants),
                                   "fluid_cb", cb_buf);

            // Snapshot velocity for FLIP
            {
                auto snap_cmd = rc.create(CommandListDesc{});
                snap_cmd->open();
                snap_cmd->copyBuffer(storage.vel_x_old, 0, storage.vel_x, 0, win_n3d * sizeof(float));
                snap_cmd->copyBuffer(storage.vel_y_old, 0, storage.vel_y, 0, win_n3d * sizeof(float));
                snap_cmd->copyBuffer(storage.vel_z_old, 0, storage.vel_z, 0, win_n3d * sizeof(float));
                snap_cmd->close();
                device->executeCommandList(snap_cmd);
                device->waitForIdle();
                rc.destroy(snap_cmd);
            }

            // --- Velocity step ---
            fluid_cb.jacobi_mode = 0;
            fluid_cb.jacobi_alpha = sub_dt * viscosity *
                static_cast<float>(storage.grid_res * storage.grid_res);
            {
                nvrhi::BufferHandle jcb;
                upload_constant_buffer(rc, device, &fluid_cb, sizeof(SimConstants),
                                       "jacobi_cb", jcb);

                dispatch_field(rc, storage.jacobi_program,
                    {{"field_in", storage.vel_x}, {"rhs", storage.vel_x},
                     {"bristle_psi", storage.bristle_density},
                     {"wetness", storage.wetness}},
                    {{"field_out", storage.vel_x_tmp}}, jcb, window_total);
                std::swap(storage.vel_x, storage.vel_x_tmp);

                dispatch_field(rc, storage.jacobi_program,
                    {{"field_in", storage.vel_y}, {"rhs", storage.vel_y},
                     {"bristle_psi", storage.bristle_density},
                     {"wetness", storage.wetness}},
                    {{"field_out", storage.vel_y_tmp}}, jcb, window_total);
                std::swap(storage.vel_y, storage.vel_y_tmp);

                dispatch_field(rc, storage.jacobi_program,
                    {{"field_in", storage.vel_z}, {"rhs", storage.vel_z},
                     {"bristle_psi", storage.bristle_density},
                     {"wetness", storage.wetness}},
                    {{"field_out", storage.vel_z_tmp}}, jcb, window_total);
                std::swap(storage.vel_z, storage.vel_z_tmp);

                rc.destroy(jcb);
            }

            // Project (Fixed-point, Algorithm 1: L=3, 2 Jacobi per L)
            for (int fp = 0; fp < 3; fp++) {
                dispatch_field(rc, storage.divergence_program,
                    {{"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}, {"vel_z", storage.vel_z},
                     {"bristle_psi", storage.bristle_density},
                     {"wetness", storage.wetness}},
                    {{"div_out", storage.divergence_buf}}, cb_buf, window_total);

                fluid_cb.jacobi_mode = 1;
                nvrhi::BufferHandle pcb;
                upload_constant_buffer(rc, device, &fluid_cb, sizeof(SimConstants),
                                       "press_cb", pcb);

                for (int ji = 0; ji < 2; ji++) {
                    dispatch_field(rc, storage.jacobi_program,
                        {{"field_in", storage.pressure_a},
                         {"rhs", storage.divergence_buf},
                         {"bristle_psi", storage.bristle_density},
                         {"wetness", storage.wetness}},
                        {{"field_out", storage.pressure_b}}, pcb, window_total);
                    std::swap(storage.pressure_a, storage.pressure_b);
                }
                rc.destroy(pcb);

                dispatch_field(rc, storage.gradient_program,
                    {{"pressure", storage.pressure_a},
                     {"bristle_psi", storage.bristle_density},
                     {"wetness", storage.wetness}},
                    {{"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}, {"vel_z", storage.vel_z}},
                    cb_buf, window_total);
            }

            // Advect velocity
            dispatch_field(rc, storage.advect_program,
                {{"field_in", storage.vel_x},
                 {"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}, {"vel_z", storage.vel_z}},
                {{"field_out", storage.vel_x_tmp}}, cb_buf, window_total);
            std::swap(storage.vel_x, storage.vel_x_tmp);

            dispatch_field(rc, storage.advect_program,
                {{"field_in", storage.vel_y},
                 {"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}, {"vel_z", storage.vel_z}},
                {{"field_out", storage.vel_y_tmp}}, cb_buf, window_total);
            std::swap(storage.vel_y, storage.vel_y_tmp);

            dispatch_field(rc, storage.advect_program,
                {{"field_in", storage.vel_z},
                 {"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}, {"vel_z", storage.vel_z}},
                {{"field_out", storage.vel_z_tmp}}, cb_buf, window_total);
            std::swap(storage.vel_z, storage.vel_z_tmp);

            // Project again
            for (int fp = 0; fp < 3; fp++) {
                dispatch_field(rc, storage.divergence_program,
                    {{"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}, {"vel_z", storage.vel_z},
                     {"bristle_psi", storage.bristle_density},
                     {"wetness", storage.wetness}},
                    {{"div_out", storage.divergence_buf}}, cb_buf, window_total);

                fluid_cb.jacobi_mode = 1;
                nvrhi::BufferHandle pcb2;
                upload_constant_buffer(rc, device, &fluid_cb, sizeof(SimConstants),
                                       "press2_cb", pcb2);

                for (int ji = 0; ji < 2; ji++) {
                    dispatch_field(rc, storage.jacobi_program,
                        {{"field_in", storage.pressure_a},
                         {"rhs", storage.divergence_buf},
                         {"bristle_psi", storage.bristle_density},
                         {"wetness", storage.wetness}},
                        {{"field_out", storage.pressure_b}}, pcb2, window_total);
                    std::swap(storage.pressure_a, storage.pressure_b);
                }
                rc.destroy(pcb2);

                dispatch_field(rc, storage.gradient_program,
                    {{"pressure", storage.pressure_a},
                     {"bristle_psi", storage.bristle_density},
                     {"wetness", storage.wetness}},
                    {{"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}, {"vel_z", storage.vel_z}},
                    cb_buf, window_total);
            }

            // --- Scalar step ---
            dispatch_field(rc, storage.advect_program,
                {{"field_in", storage.density},
                 {"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}, {"vel_z", storage.vel_z}},
                {{"field_out", storage.density_tmp}}, cb_buf, window_total);
            std::swap(storage.density, storage.density_tmp);

            dispatch_field(rc, storage.advect_program,
                {{"field_in", storage.color_r},
                 {"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}, {"vel_z", storage.vel_z}},
                {{"field_out", storage.color_tmp}}, cb_buf, window_total);
            std::swap(storage.color_r, storage.color_tmp);

            dispatch_field(rc, storage.advect_program,
                {{"field_in", storage.color_y},
                 {"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}, {"vel_z", storage.vel_z}},
                {{"field_out", storage.color_tmp}}, cb_buf, window_total);
            std::swap(storage.color_y, storage.color_tmp);

            dispatch_field(rc, storage.advect_program,
                {{"field_in", storage.color_b},
                 {"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}, {"vel_z", storage.vel_z}},
                {{"field_out", storage.color_tmp}}, cb_buf, window_total);
            std::swap(storage.color_b, storage.color_tmp);

            dispatch_field(rc, storage.advect_program,
                {{"field_in", storage.wetness},
                 {"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}, {"vel_z", storage.vel_z}},
                {{"field_out", storage.wetness_tmp}}, cb_buf, window_total);
            std::swap(storage.wetness, storage.wetness_tmp);

            // Advect oil density (§6 — advect all fields)
            dispatch_field(rc, storage.advect_program,
                {{"field_in", storage.oil_density},
                 {"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}, {"vel_z", storage.vel_z}},
                {{"field_out", storage.oil_density_tmp}}, cb_buf, window_total);
            std::swap(storage.oil_density, storage.oil_density_tmp);

            // Diffuse density + wetness
            fluid_cb.jacobi_mode = 0;
            fluid_cb.jacobi_alpha = sub_dt * diffusion *
                static_cast<float>(storage.grid_res * storage.grid_res);
            {
                nvrhi::BufferHandle dcb;
                upload_constant_buffer(rc, device, &fluid_cb, sizeof(SimConstants),
                                       "diff_cb", dcb);

                dispatch_field(rc, storage.jacobi_program,
                    {{"field_in", storage.density}, {"rhs", storage.density},
                     {"bristle_psi", storage.bristle_density},
                     {"wetness", storage.wetness}},
                    {{"field_out", storage.density_tmp}}, dcb, window_total);
                std::swap(storage.density, storage.density_tmp);

                dispatch_field(rc, storage.jacobi_program,
                    {{"field_in", storage.wetness}, {"rhs", storage.wetness},
                     {"bristle_psi", storage.bristle_density},
                     {"wetness", storage.wetness}},
                    {{"field_out", storage.wetness_tmp}}, dcb, window_total);
                std::swap(storage.wetness, storage.wetness_tmp);

                // Diffuse RYB color channels — MUST stay in sync with density
                // diffusion. Wetbrush §4.2: "we diffuse all of the fields".
                // Without this, density bleeds into neighbouring cells whose
                // color stays 0, producing white "ghost" particles on readback
                // (ryb_to_rgb(0,0,0) = white). Reuses color_tmp like advection.
                dispatch_field(rc, storage.jacobi_program,
                    {{"field_in", storage.color_r}, {"rhs", storage.color_r},
                     {"bristle_psi", storage.bristle_density},
                     {"wetness", storage.wetness}},
                    {{"field_out", storage.color_tmp}}, dcb, window_total);
                std::swap(storage.color_r, storage.color_tmp);

                dispatch_field(rc, storage.jacobi_program,
                    {{"field_in", storage.color_y}, {"rhs", storage.color_y},
                     {"bristle_psi", storage.bristle_density},
                     {"wetness", storage.wetness}},
                    {{"field_out", storage.color_tmp}}, dcb, window_total);
                std::swap(storage.color_y, storage.color_tmp);

                dispatch_field(rc, storage.jacobi_program,
                    {{"field_in", storage.color_b}, {"rhs", storage.color_b},
                     {"bristle_psi", storage.bristle_density},
                     {"wetness", storage.wetness}},
                    {{"field_out", storage.color_tmp}}, dcb, window_total);
                std::swap(storage.color_b, storage.color_tmp);
                rc.destroy(dcb);
            }

            // Damp + dry
            dispatch_field(rc, storage.damp_dry_program,
                {{"oil_density", storage.oil_density}},
                {{"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}, {"vel_z", storage.vel_z},
                 {"wetness", storage.wetness}},
                cb_buf, window_total);

            // FLIP/PIC velocity update for particles
            if (storage.particles_initialized) { // re-enable for testing
                ParticleConstants pc = {};
                pc.max_particles = max_ptcl;
                pc.dt = sub_dt;
                pc.D0 = brush_radius * 3.0f;
                pc.flip_gamma = 0.8f;
                pc.grid_res = storage.grid_res;
                pc.grid_res_z = rz;
                pc.height_extent = storage.grid_height;
                pc.grid_center_z = storage.grid_center_z;
                pc.cell_size = cell_sz;
                pc.paper_size = storage.grid_paper;
                pc.grid_center_x = storage.grid_center.x;
                pc.grid_center_y = storage.grid_center.y;
                pc.window_origin_x = storage.win_origin_x;
                pc.window_origin_y = storage.win_origin_y;
                pc.window_origin_z = 0;
                pc.window_size_x = WIN_XY;
                pc.window_size_z = WIN_Z;
                pc.brush_pos_x = brush_pos_3d.x;
                pc.brush_pos_y = brush_pos_3d.y;
                pc.brush_pos_z = brush_pos_3d.z;
                pc.brush_radius = brush_radius;

                nvrhi::BufferHandle flip_cb;
                upload_constant_buffer(rc, device, &pc, sizeof(ParticleConstants),
                                       "flip_cb", flip_cb);

                dispatch_raw(rc, storage.ptcl_flip_pic_program,
                    {{"ptcl_pos", storage.ptcl_pos},
                     {"ptcl_alive", storage.ptcl_alive},
                     {"vel_x_old", storage.vel_x_old},
                     {"vel_y_old", storage.vel_y_old},
                     {"vel_z_old", storage.vel_z_old},
                     {"vel_x_new", storage.vel_x},
                     {"vel_y_new", storage.vel_y},
                     {"vel_z_new", storage.vel_z}},
                    {{"ptcl_vel", storage.ptcl_vel}},
                    flip_cb, max_ptcl);

                rc.destroy(flip_cb);
            }

            rc.destroy(cb_buf);
        }
    }

    // === POST-FLUID: particle maintenance ===
    if (storage.particles_initialized) {
        ParticleConstants pc = {};
        pc.max_particles = max_ptcl;
        pc.dt = 0.016f;
        pc.D0 = brush_radius * 3.0f;
        pc.grid_res = storage.grid_res;
        pc.grid_res_z = rz;
        pc.height_extent = storage.grid_height;
        pc.grid_center_z = storage.grid_center_z;
        pc.cell_size = cell_sz;
        pc.paper_size = storage.grid_paper;
        pc.grid_center_x = storage.grid_center.x;
        pc.grid_center_y = storage.grid_center.y;
        pc.window_origin_x = storage.win_origin_x;
        pc.window_origin_y = storage.win_origin_y;
        pc.window_origin_z = 0;
        pc.window_size_x = WIN_XY;
        pc.window_size_z = WIN_Z;
        pc.brush_pos_x = brush_pos_3d.x;
        pc.brush_pos_y = brush_pos_3d.y;
        pc.brush_pos_z = brush_pos_3d.z;
        pc.brush_radius = brush_radius;

        nvrhi::BufferHandle maint_cb;
        upload_constant_buffer(rc, device, &pc, sizeof(ParticleConstants),
                               "maint_cb", maint_cb);

        // Particle to grid (absorb distant slow particles, Section 5.2 Eq.16)
        dispatch_raw(rc, storage.ptcl_to_grid_program,
            {{"ptcl_pos", storage.ptcl_pos},
             {"ptcl_vel", storage.ptcl_vel},
             {"ptcl_color", storage.ptcl_color},
             {"ptcl_alive", storage.ptcl_alive}},
            {{"density", storage.density},
             {"color_r", storage.color_r},
             {"color_y", storage.color_y},
             {"color_b", storage.color_b},
             {"ptcl_alive_out", storage.ptcl_alive_b}},
            maint_cb, max_ptcl);
        std::swap(storage.ptcl_alive, storage.ptcl_alive_b);

        // Reset counter again before grid-to-particle emission
        reset_counter(rc, device, storage.ptcl_counter);

        // Grid to particle (emit near brush, with stratified sampling + Eq.15 density subtraction)
        // Uses density_tmp as output to avoid read-write hazard, then swap
        dispatch_raw(rc, storage.grid_to_ptcl_program,
            {{"density", storage.density},
             {"color_r", storage.color_r},
             {"color_y", storage.color_y},
             {"color_b", storage.color_b},
             {"vel_x", storage.vel_x},
             {"vel_y", storage.vel_y},
             {"vel_z", storage.vel_z}},
            {{"ptcl_counter", storage.ptcl_counter},
             {"ptcl_pos", storage.ptcl_pos},
             {"ptcl_vel", storage.ptcl_vel},
             {"ptcl_color", storage.ptcl_color},
             {"ptcl_alive", storage.ptcl_alive},
             {"density_out", storage.density_tmp}},  // Eq.15: density reduced to tmp
            maint_cb, win_n3d);
        std::swap(storage.density, storage.density_tmp);

        // Reset counter before compaction
        reset_counter(rc, device, storage.ptcl_counter);

        // Particle compaction
        dispatch_raw(rc, storage.ptcl_compact_program,
            {{"ptcl_alive", storage.ptcl_alive},
             {"ptcl_pos", storage.ptcl_pos},
             {"ptcl_vel", storage.ptcl_vel},
             {"ptcl_color", storage.ptcl_color}},
            {{"ptcl_counter", storage.ptcl_counter},
             {"ptcl_pos_out", storage.ptcl_pos_b},
             {"ptcl_vel_out", storage.ptcl_vel_b},
             {"ptcl_color_out", storage.ptcl_color_b},
             {"ptcl_alive_out", storage.ptcl_alive_b}},
            maint_cb, max_ptcl);
        std::swap(storage.ptcl_pos, storage.ptcl_pos_b);
        std::swap(storage.ptcl_vel, storage.ptcl_vel_b);
        std::swap(storage.ptcl_color, storage.ptcl_color_b);
        std::swap(storage.ptcl_alive, storage.ptcl_alive_b);

        rc.destroy(maint_cb);
    }

    // === READBACK ===
    int readback_n = win_n3d;
    auto readback = [&](nvrhi::BufferHandle field) -> std::vector<float> {
        std::vector<float> data(readback_n);
        auto rb = rc.create(nvrhi::BufferDesc{}
            .setByteSize(readback_n * sizeof(float))
            .setCpuAccess(nvrhi::CpuAccessMode::Read)
            .setDebugName("readback"));
        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        cmd->copyBuffer(rb, 0, field, 0, readback_n * sizeof(float));
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        void* mapped = device->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
        memcpy(data.data(), mapped, readback_n * sizeof(float));
        device->unmapBuffer(rb);
        rc.destroy(rb);
        rc.destroy(cmd);
        return data;
    };

    auto density_cpu = readback(storage.density);
    auto cr_cpu = readback(storage.color_r);
    auto cy_cpu = readback(storage.color_y);
    auto cb_cpu = readback(storage.color_b);

    // === FIDELITY STATISTICS ===
    // Aggregate GPU state into the debug output ports declared above, so the
    // Python fidelity tests can assert on physical correctness (divergence
    // should fall after pressure projection, paint mass should be conserved,
    // particle counts should be bounded). See tests/test_brush_sim_fidelity.py.

    // -- Grid-side: divergence over the active window, density/color totals
    //    over the window. divergence_buf holds window-local values now (the
    //    buffer is window-sized), so indices are window-local [0,WIN)³.
    float max_div = 0.0;
    double div_sum = 0.0;
    int   div_count = 0;
    if (sim_dt > 1e-6f && storage.divergence_buf) {
        auto div_cpu = readback(storage.divergence_buf);
        for (int dz = 0; dz < WIN_Z; ++dz)
        for (int dy = 0; dy < WIN_XY; ++dy)
        for (int dx = 0; dx < WIN_XY; ++dx) {
            // Window-local linear index (matches grid_idx_3d with WIN strides).
            int gi = dz * WIN_XY * WIN_XY + dy * WIN_XY + dx;
            if (gi < 0 || gi >= readback_n) continue;
            float ad = std::fabs(div_cpu[gi]);
            max_div = std::max(max_div, ad);
            div_sum += ad;
            ++div_count;
        }
    }
    float mean_div = div_count > 0
        ? static_cast<float>(div_sum / static_cast<double>(div_count)) : 0.0f;

    double tot_density = 0.0, tot_r = 0.0, tot_y = 0.0, tot_b = 0.0;
    for (int i = 0; i < readback_n; ++i) {
        tot_density += density_cpu[i];
        tot_r += cr_cpu[i];
        tot_y += cy_cpu[i];
        tot_b += cb_cpu[i];
    }

    // -- Particle-side: alive count + total mass (color.w carries density).
    //    ptcl_counter is a 4-byte ByteAddressBuffer, so it needs its own
    //    tiny readback rather than the n3d-float lambda above. The color
    //    buffer is float4 × max_ptcl.
    int ptcl_count = 0;
    float ptcl_mass = 0.0f;
    if (storage.particles_initialized && storage.ptcl_counter) {
        // Read the 4-byte counter.
        {
            uint32_t cnt = 0;
            auto rb = rc.create(nvrhi::BufferDesc{}
                .setByteSize(sizeof(uint32_t))
                .setCpuAccess(nvrhi::CpuAccessMode::Read)
                .setDebugName("ptcl_counter_rb"));
            auto cmd = rc.create(CommandListDesc{});
            cmd->open();
            cmd->copyBuffer(rb, 0, storage.ptcl_counter, 0, sizeof(uint32_t));
            cmd->close();
            device->executeCommandList(cmd);
            device->waitForIdle();
            void* mapped = device->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
            memcpy(&cnt, mapped, sizeof(uint32_t));
            device->unmapBuffer(rb);
            rc.destroy(rb);
            rc.destroy(cmd);
            ptcl_count = static_cast<int>(cnt);
        }
        // Sum mass over the alive slots actually in use. color.w carries
        // the per-particle density; read raw float bytes (stride 16) so we
        // don't depend on a host-side float4 typedef.
        if (ptcl_count > 0 && storage.ptcl_color && storage.ptcl_alive) {
            int n = std::min(ptcl_count, max_ptcl);
            constexpr int STRIDE = 4;  // float4 = 16 bytes
            std::vector<float> cols(n * STRIDE);
            std::vector<uint32_t> alive(n);
            auto read_structured = [&](nvrhi::BufferHandle buf,
                                       int elem_bytes, void* dst) {
                auto rb = rc.create(nvrhi::BufferDesc{}
                    .setByteSize(static_cast<size_t>(n) * elem_bytes)
                    .setCpuAccess(nvrhi::CpuAccessMode::Read)
                    .setDebugName("ptcl_rb"));
                auto cmd = rc.create(CommandListDesc{});
                cmd->open();
                cmd->copyBuffer(rb, 0, buf, 0,
                                static_cast<size_t>(n) * elem_bytes);
                cmd->close();
                device->executeCommandList(cmd);
                device->waitForIdle();
                void* mapped = device->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
                memcpy(dst, mapped, static_cast<size_t>(n) * elem_bytes);
                device->unmapBuffer(rb);
                rc.destroy(rb);
                rc.destroy(cmd);
            };
            read_structured(storage.ptcl_color, sizeof(float) * STRIDE, cols.data());
            read_structured(storage.ptcl_alive, sizeof(uint32_t), alive.data());
            for (int i = 0; i < n; ++i)
                if (alive[i] != 0)
                    ptcl_mass += cols[i * STRIDE + 3];  // .w component
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

    // === FINAL COMMIT: flush live window into the 2D canvas layer ===
    // Every frame, bake the current 3D window paint into the persistent canvas
    // so the output reflects ALL painted regions (not just the live brush
    // window). The canvas accumulates; the window keeps its paint too (it will
    // be cleared on the next window move). Without this, only the brush's
    // current neighborhood would show.
    {
        SimConstants commit_cb = {};
        commit_cb.res = storage.grid_res;
        commit_cb.cell_size = cell_sz;
        commit_cb.paper_size = storage.grid_paper;
        commit_cb.res_z = rz;
        commit_cb.height_extent = storage.grid_height;
        commit_cb.grid_center_z = storage.grid_center_z;
        commit_cb.window_origin_x = storage.win_origin_x;
        commit_cb.window_origin_y = storage.win_origin_y;
        commit_cb.window_origin_z = 0;
        commit_cb.window_size_x = WIN_XY;
        commit_cb.window_size_y = WIN_XY;
        commit_cb.window_size_z = WIN_Z;

        nvrhi::BufferHandle commit_cb_buf;
        upload_constant_buffer(rc, device, &commit_cb, sizeof(SimConstants),
                               "final_commit_cb", commit_cb_buf);
        ProgramVars cv(rc, storage.canvas_commit_program);
        cv["cb"] = commit_cb_buf.Get();
        cv["density"]  = storage.density.Get();
        cv["color_r"]  = storage.color_r.Get();
        cv["color_y"]  = storage.color_y.Get();
        cv["color_b"]  = storage.color_b.Get();
        cv["wetness"]  = storage.wetness.Get();
        cv["canvas_density"] = storage.canvas_density.Get();
        cv["canvas_color_r"] = storage.canvas_color_r.Get();
        cv["canvas_color_y"] = storage.canvas_color_y.Get();
        cv["canvas_color_b"] = storage.canvas_color_b.Get();
        cv["canvas_wetness"] = storage.canvas_wetness.Get();
        cv.finish_setting_vars();
        ComputeContext cctx(rc, cv);
        cctx.finish_setting_pso();
        cctx.begin();
        cctx.dispatch({}, cv, WIN_XY * WIN_XY, 256);
        cctx.finish();
        rc.destroy(commit_cb_buf);
    }

    // === OUTPUT: read the 2D canvas layer and emit a point per painted cell ===
    auto [particles, pts] = make_particles();
    std::vector<glm::vec3> out_pts;
    std::vector<glm::vec3> out_colors;
    std::vector<float> out_widths;

    // Read back the 2D canvas layer (full grid XY).
    int n2d_read = storage.grid_res * storage.grid_res;
    auto readback_2d = [&](nvrhi::BufferHandle field) -> std::vector<float> {
        std::vector<float> data(n2d_read);
        auto rb = rc.create(nvrhi::BufferDesc{}
            .setByteSize(n2d_read * sizeof(float))
            .setCpuAccess(nvrhi::CpuAccessMode::Read)
            .setDebugName("canvas_rb"));
        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        cmd->copyBuffer(rb, 0, field, 0, n2d_read * sizeof(float));
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        void* mapped = device->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
        memcpy(data.data(), mapped, n2d_read * sizeof(float));
        device->unmapBuffer(rb);
        rc.destroy(rb);
        rc.destroy(cmd);
        return data;
    };
    auto cdensity = readback_2d(storage.canvas_density);
    auto ccr = readback_2d(storage.canvas_color_r);
    auto ccy = readback_2d(storage.canvas_color_y);
    auto ccb = readback_2d(storage.canvas_color_b);

    constexpr float threshold = 0.001f;
    constexpr float rgb_white_cutoff = 0.9f;
    // Canvas is 2D (height field). One point per painted cell, at the canvas
    // surface Z offset by the accumulated density (paint thickness). step_xy=1
    // gives full resolution — no downsampling, so the brush footprint renders
    // at its true size. Width = true cell size (no inflation).
    float canvas_floor_z = storage.grid_center_z - storage.grid_height * 0.5f;
    for (int y = 0; y < storage.grid_res; ++y) {
        for (int x = 0; x < storage.grid_res; ++x) {
            int gi = y * storage.grid_res + x;
            if (cdensity[gi] <= threshold) continue;
            // canvas_color stores density-weighted RYB sum; normalize by density
            // to recover the average pigment.
            float d = cdensity[gi];
            float r = ccr[gi] / (d + 1e-8f);
            float yy = ccy[gi] / (d + 1e-8f);
            float b = ccb[gi] / (d + 1e-8f);
            r = std::min(std::max(r, 0.0f), 1.0f);
            yy = std::min(std::max(yy, 0.0f), 1.0f);
            b = std::min(std::max(b, 0.0f), 1.0f);
            float rm = 1-r, ym = 1-yy, bm = 1-b;
            glm::vec3 rgb = rm*ym*bm*glm::vec3(1,1,1) + r*ym*bm*glm::vec3(1,0,0)
                + rm*yy*bm*glm::vec3(1,1,0) + rm*ym*b*glm::vec3(0.163f,0.373f,0.6f)
                + r*yy*bm*glm::vec3(1,0.5f,0) + r*ym*b*glm::vec3(0.5f,0,0.5f)
                + rm*yy*b*glm::vec3(0,0.66f,0.2f) + r*yy*b*glm::vec3(0.2f,0.094f,0.029f);
            if (std::min({rgb.r, rgb.g, rgb.b}) >= rgb_white_cutoff) continue;

            float gx = (x + 0.5f) * cell_sz - storage.grid_paper * 0.5f;
            float gy = (y + 0.5f) * cell_sz - storage.grid_paper * 0.5f;
            // Paint thickness in Z: scale density to a visible height, capped.
            float gz = canvas_floor_z + std::min(d * cell_sz * 0.5f, cell_sz * 2.0f);
            out_pts.push_back(glm::vec3(
                gx + storage.grid_center.x,
                gy + storage.grid_center.y,
                gz));
            out_colors.push_back(rgb);
            out_widths.push_back(cell_sz * std::min(d, 1.0f));
        }
    }

    pts->set_vertices(out_pts);
    pts->set_display_color(out_colors);
    pts->set_width(out_widths);

    spdlog::info("brush_paint_sim: {} new verts, {} substeps, {} particles",
                 new_count,
                 sim_dt > 1e-6f ?
                     std::min(16, std::max(1, static_cast<int>(
                         std::ceil(sim_dt / (2.0f / storage.grid_res))))) : 0,
                 out_pts.size());

    params.set_output("Paint Particles", std::move(particles));
    params.set_storage(storage);
    return true;
}

NODE_DECLARATION_UI(brush_paint_sim);

NODE_DEF_CLOSE_SCOPE
