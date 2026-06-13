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
#include <vector>

// ============================================================
// Helpers (outside NODE_DEF_OPEN_SCOPE to avoid C-linkage issues)
// ============================================================

namespace Ruzino {
namespace {

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

    float viscosity;        // Viscosity coefficient
    float diffusion;        // Diffusion rate
    float drying_rate;      // Drying rate
    float brush_radius;     // Brush radius (world units)

    float ink_amount;       // Ink deposit amount
    int num_vertices;       // Number of NEW curve vertices to deposit
    float center_x;         // Grid center X (world space)
    float center_y;         // Grid center Y (world space)

    float center_z;         // Grid center Z (world space)
    float effective_radius; // max(brush_radius, cell_size * 3)
    int jacobi_mode;        // 0 = diffuse, 1 = pressure
    float jacobi_alpha;     // alpha for Jacobi

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
    float _pad0;
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
    nvrhi::BufferHandle height_field;
    nvrhi::BufferHandle pressure_a, pressure_b;
    nvrhi::BufferHandle divergence_buf;

    // Per-frame upload buffers
    nvrhi::BufferHandle vertex_buf;
    nvrhi::BufferHandle color_buf;

    // --- Bristle model (3D, §4.1) ---
    static constexpr int NUM_BRISTLES = 80;
    static constexpr int VERTS_PER_BRISTLE = 10;
    static constexpr int SAMPLES_PER_BRISTLE = 32;
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
    nvrhi::BufferHandle bristle_input_color_buf; // float4: user paint color RYB

    // --- FLIP/PIC particles ---
    static constexpr int MAX_PARTICLES = 16384;

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
    int deposited_count = 0;
    float last_sim_time = -1.0f;

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
            release(height_field);
            release(pressure_a); release(pressure_b);
            release(divergence_buf);
            release(vertex_buf); release(color_buf);
            release(bristle_data); release(sample_pos); release(sample_vel);
            release(sample_color); release(sample_frame); release(lambda_buf);
            release(sample_liquid); release(sample_liquid_b); release(bristle_input_color_buf);
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
        destroy_buf(height_field);
        destroy_buf(pressure_a); destroy_buf(pressure_b);
        destroy_buf(divergence_buf);
        destroy_buf(vertex_buf); destroy_buf(color_buf);
        destroy_buf(bristle_data); destroy_buf(sample_pos); destroy_buf(sample_vel);
            destroy_buf(sample_color); destroy_buf(sample_frame); destroy_buf(lambda_buf);
        destroy_buf(sample_liquid); destroy_buf(sample_liquid_b); destroy_buf(bristle_input_color_buf);
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
    }
};

// ============================================================
// Node declaration
// ============================================================

NODE_DECLARATION_FUNCTION(brush_paint_sim)
{
    b.add_input<Geometry>("Brush Strokes");
    b.add_input<int>("Resolution").default_val(512).min(64).max(2048);
    b.add_input<int>("Resolution Z").default_val(32).min(4).max(128);
    b.add_input<float>("Paper Size").default_val(1.0f).min(0.1f).max(10.0f);
    b.add_input<float>("Brush Radius").default_val(0.02f).min(0.001f).max(0.5f);
    b.add_input<float>("Ink Amount").default_val(0.8f).min(0.0f).max(2.0f);
    b.add_input<float>("Viscosity").default_val(0.5f).min(0.0f).max(10.0f);
    b.add_input<float>("Diffusion Rate")
        .default_val(0.0001f).min(0.0f).max(0.01f);
    b.add_input<float>("Pickup Rate").default_val(0.1f).min(0.0f).max(1.0f);
    b.add_input<float>("Drying Rate").default_val(0.1f).min(0.0f).max(2.0f);
    b.add_output<Geometry>("Paint Particles");
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
    float brush_radius = params.get_input<float>("Brush Radius");
    float ink_amount = params.get_input<float>("Ink Amount");
    float viscosity = params.get_input<float>("Viscosity");
    float diffusion = params.get_input<float>("Diffusion Rate");
    float drying_rate = params.get_input<float>("Drying Rate");

    auto make_particles = [&]() -> std::pair<Geometry, PointsComponent*> {
        Geometry geom;
        auto pts = std::make_shared<PointsComponent>(&geom);
        geom.attach_component(pts);
        return {std::move(geom), pts.get()};
    };

    auto curve = brush_strokes.get_component<CurveComponent>();
    if (!curve || curve->get_vertices().empty()) {
        auto [geom, pts] = make_particles();
        params.set_output("Paint Particles", std::move(geom));
        params.set_storage(storage);
        return true;
    }

    auto vertices = curve->get_vertices();
    auto colors = curve->get_display_color();

    // Detect stroke reset
    if (static_cast<int>(vertices.size()) < storage.deposited_count) {
        storage.deposited_count = 0;
        storage.last_sim_time = -1.0f;
        storage.center_initialized = false;
        storage.bristles_initialized = false;
    }

    int already_deposited = storage.deposited_count;
    int new_count = static_cast<int>(vertices.size()) - already_deposited;

    // Re-anchor if all new vertices are outside current grid
    if (storage.center_initialized && new_count > 0) {
        bool any_inside = false;
        glm::vec2 c = storage.grid_center;
        float half = storage.grid_paper * 0.5f;
        float cs = storage.grid_paper / static_cast<float>(storage.grid_res);
        for (size_t si = 0; si < vertices.size(); si++) {
            if (static_cast<int>(si) < already_deposited) continue;
            float gx = (vertices[si].x - c.x + half) / cs;
            float gy = (vertices[si].y - c.y + half) / cs;
            if (gx >= 0 && gx < storage.grid_res &&
                gy >= 0 && gy < storage.grid_res) {
                any_inside = true;
                break;
            }
        }
        if (!any_inside) {
            spdlog::info("brush_paint_sim: re-anchoring grid");
            storage.center_initialized = false;
            storage.deposited_count = 0;
            already_deposited = 0;
            new_count = static_cast<int>(vertices.size());
            storage.bristles_initialized = false;
        }
    }

    // Initialize grid from bounding box
    if (!storage.center_initialized && !vertices.empty()) {
        glm::vec2 bmin(std::numeric_limits<float>::max());
        glm::vec2 bmax(std::numeric_limits<float>::lowest());
        float z_sum = 0.0f;
        for (const auto& v : vertices) {
            bmin = glm::min(bmin, glm::vec2(v.x, v.y));
            bmax = glm::max(bmax, glm::vec2(v.x, v.y));
            z_sum += v.z;
        }
        storage.grid_center = (bmin + bmax) * 0.5f;
        storage.grid_center_z = z_sum / static_cast<float>(vertices.size());

        glm::vec2 extent = bmax - bmin;
        float margin = std::max(brush_radius * 8, 0.5f);
        storage.grid_paper = std::max({
            paper_size,
            extent.x + margin * 2.0f,
            extent.y + margin * 2.0f});
        storage.grid_res = resolution;
        storage.grid_res_z = resolution_z;
        storage.grid_height = storage.grid_paper;  // Z extent matches XY paper size for cubic voxels
        storage.center_initialized = true;

        spdlog::info(
            "brush_paint_sim: grid {}x{}x{}, paper={:.3f}, height={:.3f}, cell={:.5f}",
            resolution, resolution, resolution_z, storage.grid_paper, storage.grid_height,
            storage.grid_paper / static_cast<float>(resolution));
    }

    // Helper: safely destroy a buffer via resource allocator before recreation
    auto safe_destroy_buf = [&](nvrhi::BufferHandle& h) {
        if (h) { rc.destroy(h); h = nullptr; }
    };

    // Create or resize GPU buffers
    int n  = storage.grid_res * storage.grid_res;
    int rz = storage.grid_res_z > 0 ? storage.grid_res_z : resolution_z;
    int n3d = storage.grid_res * storage.grid_res * rz;
    if (storage.grid_alloc_res != storage.grid_res || storage.grid_alloc_res_z != rz) {
        storage.grid_alloc_res = storage.grid_res;
        storage.grid_alloc_res_z = rz;
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

        auto make_buf = [&](const char* name) -> nvrhi::BufferHandle {
            return create_field_buffer(rc, n3d, name);
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
        storage.vel_y        = make_buf("vel_y");
        storage.vel_y_tmp    = make_buf("vel_y_tmp");
        storage.vel_z        = make_buf("vel_z");
        storage.vel_z_tmp    = make_buf("vel_z_tmp");
        storage.wetness      = make_buf("wetness");
        storage.wetness_tmp  = make_buf("wetness_tmp");
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

        // Zero-init all field buffers
        std::vector<float> zeros(n3d, 0.0f);
        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        for (auto* buf : {&storage.density, &storage.density_tmp,
                          &storage.color_r, &storage.color_y, &storage.color_b,
                          &storage.color_tmp, &storage.vel_x, &storage.vel_x_tmp,
                          &storage.vel_y, &storage.vel_y_tmp,
                          &storage.vel_z, &storage.vel_z_tmp,
                          &storage.wetness, &storage.wetness_tmp,
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
            cmd->writeBuffer(*buf, zeros.data(), n3d * sizeof(float));
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
        cmd->writeBuffer(storage.sample_frame, zeros_sample.data(),
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
        params.set_storage(storage);
        return false;
    }

    // === Derive brush transform from latest vertex ===
    float cell_sz = storage.grid_paper / static_cast<float>(storage.grid_res);
    float eff_radius = std::max(brush_radius, cell_sz * 3.0f);

    glm::vec3 brush_pos_3d(0.0f);
    glm::vec3 brush_vel_3d(0.0f);
    float brush_rotation = 0.0f;
    // Brush rotates about the canvas normal (Z axis); the stroke is planar so
    // ω is dominantly z. x/y components stay 0 unless the canvas tilts.
    glm::vec3 brush_angular_vel(0.0f);

    if (!vertices.empty()) {
        int last = static_cast<int>(vertices.size()) - 1;
        brush_pos_3d = vertices[last];
        brush_pos_3d.x -= storage.grid_center.x;
        brush_pos_3d.y -= storage.grid_center.y;

        if (last > 0) {
            brush_vel_3d = vertices[last] - vertices[last - 1];
            // Heading angle in the XY plane
            brush_rotation = atan2(brush_vel_3d.y, brush_vel_3d.x);
            if (last > 1) {
                float prev_rot = atan2(
                    vertices[last-1].y - vertices[last-2].y,
                    vertices[last-1].x - vertices[last-2].x);
                // Angular velocity about canvas normal (Z)
                brush_angular_vel.z = brush_rotation - prev_rot;
            }
        }
    }

    // === BRISTLE SIMULATION ===
    {
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
            {{"field", storage.bristle_density}}, nullptr, n3d);
        dispatch_field(rc, storage.field_clear_program,
            {},
            {{"field", storage.bristle_vel_x}}, nullptr, n3d);
        dispatch_field(rc, storage.field_clear_program,
            {},
            {{"field", storage.bristle_vel_y}}, nullptr, n3d);
        dispatch_field(rc, storage.field_clear_program,
            {},
            {{"field", storage.bristle_vel_z}}, nullptr, n3d);
        dispatch_field(rc, storage.field_clear_program,
            {},
            {{"field", storage.bristle_color_r}}, nullptr, n3d);
        dispatch_field(rc, storage.field_clear_program,
            {},
            {{"field", storage.bristle_color_y}}, nullptr, n3d);
        dispatch_field(rc, storage.field_clear_program,
            {},
            {{"field", storage.bristle_color_b}}, nullptr, n3d);

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
             {"wetness", storage.wetness}},
            merge_cb, n3d);

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
                 {"grid_density", storage.density}},
                {{"sample_liquid_out", storage.sample_liquid_b}},
                liquid_cb, Nb * S);
            std::swap(storage.sample_liquid, storage.sample_liquid_b);

            // Pass 1: EMIT (sample → particles, hemisphere pattern)
            // Now sample_liquid has ABSORB result, write to _b
            reset_counter(rc, device, storage.ptcl_counter);
            dispatch_raw(rc, storage.bri_liquid_emit_program,
                {{"sample_pos", storage.sample_pos},
                 {"sample_color", storage.sample_color},
                 {"sample_liquid_in", storage.sample_liquid},
                 {"bristle_psi", storage.bristle_density},
                 {"grid_density", storage.density}},
                {{"sample_liquid_out", storage.sample_liquid_b},
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
        pc.friction_delta = brush_radius;
        pc.flip_gamma = 0.8f;
        pc.grid_res = storage.grid_res;
        pc.grid_res_z = rz;
        pc.height_extent = storage.grid_height;
        pc.grid_center_z = storage.grid_center_z;
        pc.cell_size = cell_sz;
        pc.paper_size = storage.grid_paper;
        pc.grid_center_x = storage.grid_center.x;
        pc.grid_center_y = storage.grid_center.y;
        pc.brush_pos_x = brush_pos_3d.x;
        pc.brush_pos_y = brush_pos_3d.y;
        pc.brush_pos_z = brush_pos_3d.z;
        pc.brush_radius = brush_radius;
        pc.D1 = brush_radius * 0.5f;
        pc.num_bristles = Nb;
        pc.samples_per_bristle = S;
        pc._pad0 = 0.0f;

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
            emit1_cb, n3d);
        rc.destroy(emit1_cb);

        // Update particles (ping-pong)
        dispatch_raw(rc, storage.ptcl_update_program,
            {{"ptcl_pos", storage.ptcl_pos},
             {"ptcl_vel", storage.ptcl_vel},
             {"ptcl_color", storage.ptcl_color},
             {"ptcl_alive", storage.ptcl_alive},
             {"sample_pos", storage.sample_pos},
             {"sample_frame", storage.sample_frame}},
            {{"ptcl_pos_out", storage.ptcl_pos_b},
             {"ptcl_vel_out", storage.ptcl_vel_b},
             {"ptcl_alive_out", storage.ptcl_alive_b}},
            ptcl_cb, max_ptcl);
        std::swap(storage.ptcl_pos, storage.ptcl_pos_b);
        std::swap(storage.ptcl_vel, storage.ptcl_vel_b);
        std::swap(storage.ptcl_alive, storage.ptcl_alive_b);

        // Clear particle accum grids (density + velocity + color)
        dispatch_field(rc, storage.field_clear_program,
            {}, {{"field", storage.ptcl_density}}, nullptr, n3d);
        dispatch_field(rc, storage.field_clear_program,
            {}, {{"field", storage.ptcl_vel_x}}, nullptr, n3d);
        dispatch_field(rc, storage.field_clear_program,
            {}, {{"field", storage.ptcl_vel_y}}, nullptr, n3d);
        dispatch_field(rc, storage.field_clear_program,
            {}, {{"field", storage.ptcl_vel_z}}, nullptr, n3d);
        dispatch_field(rc, storage.field_clear_program,
            {}, {{"field", storage.ptcl_rast_r}}, nullptr, n3d);
        dispatch_field(rc, storage.field_clear_program,
            {}, {{"field", storage.ptcl_rast_y}}, nullptr, n3d);
        dispatch_field(rc, storage.field_clear_program,
            {}, {{"field", storage.ptcl_rast_b}}, nullptr, n3d);

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
             {"wetness", storage.wetness}},
            merge_cb, n3d);
        rc.destroy(merge_cb);

        rc.destroy(ptcl_cb);
    }

    // === DEPOSIT remaining vertices (fallback for non-bristle) ===
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
        vars["height"] = storage.height_field.Get();
        vars.finish_setting_vars();

        ComputeContext ctx(rc, vars);
        ctx.finish_setting_pso();
        ctx.begin();
        ctx.dispatch({}, vars, 1, 1);
        ctx.finish();

        rc.destroy(dep_cb_buf);
    }

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

    if (sim_dt > 1e-6f) {
        float max_sub_dt = 2.0f / static_cast<float>(storage.grid_res);
        int substeps = std::max(1, static_cast<int>(std::ceil(sim_dt / max_sub_dt)));
        substeps = std::min(substeps, 16);
        float sub_dt = sim_dt / static_cast<float>(substeps);

        // --- Active window (Wetbrush §4.2) ---
        // Restrict fluid sim to a brush-centered sub-volume. Window size is
        // capped at 128×128×res_z (paper's value); smaller grids use the
        // full extent. Origin is centered on the brush and clamped to grid.
        const int WIN_XY = std::min(128, storage.grid_res);
        const int WIN_Z  = rz;  // simulate the full paint height
        float half_p = storage.grid_paper * 0.5f;
        // Brush position in grid cell coordinates (centered at origin).
        float bgx = (brush_pos_3d.x - storage.grid_center.x + half_p) / cell_sz;
        float bgy = (brush_pos_3d.y - storage.grid_center.y + half_p) / cell_sz;
        int wox = static_cast<int>(bgx) - WIN_XY / 2;
        int woy = static_cast<int>(bgy) - WIN_XY / 2;
        wox = std::max(0, std::min(wox, storage.grid_res - WIN_XY));
        woy = std::max(0, std::min(woy, storage.grid_res - WIN_XY));
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
                snap_cmd->copyBuffer(storage.vel_x_old, 0, storage.vel_x, 0, n3d * sizeof(float));
                snap_cmd->copyBuffer(storage.vel_y_old, 0, storage.vel_y, 0, n3d * sizeof(float));
                snap_cmd->copyBuffer(storage.vel_z_old, 0, storage.vel_z, 0, n3d * sizeof(float));
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
                rc.destroy(dcb);
            }

            // Damp + dry
            dispatch_field(rc, storage.damp_dry_program,
                {},
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
            maint_cb, n3d);
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
    int readback_n = n3d;
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

    auto [particles, pts] = make_particles();
    std::vector<glm::vec3> out_pts;
    std::vector<glm::vec3> out_colors;
    std::vector<float> out_widths;

    constexpr float threshold = 0.001f;
    int step_xy = std::max(1, storage.grid_res / 64);
    int step_z  = std::max(1, rz / 16);
    float cell_sz_z = storage.grid_height / static_cast<float>(rz);

    // Collapse 3D grid into 2D visualization by accumulating along Z
    for (int z = 0; z < rz; z += step_z) {
        for (int y = 0; y < storage.grid_res; y += step_xy) {
            for (int x = 0; x < storage.grid_res; x += step_xy) {
                int gi = z * storage.grid_res * storage.grid_res
                       + y * storage.grid_res + x;
                if (density_cpu[gi] > threshold) {
                    float gx = (x + 0.5f) * cell_sz - storage.grid_paper * 0.5f;
                    float gy = (y + 0.5f) * cell_sz - storage.grid_paper * 0.5f;
                    float gz = (z + 0.5f) * cell_sz_z - storage.grid_height * 0.5f;
                    out_pts.push_back(glm::vec3(
                        gx + storage.grid_center.x,
                        gy + storage.grid_center.y,
                        gz + storage.grid_center_z));

                    float r = cr_cpu[gi], yy = cy_cpu[gi], b = cb_cpu[gi];
                    float rm = 1-r, ym = 1-yy, bm = 1-b;
                    glm::vec3 rgb = rm*ym*bm*glm::vec3(1,1,1) + r*ym*bm*glm::vec3(1,0,0)
                        + rm*yy*bm*glm::vec3(1,1,0) + rm*ym*b*glm::vec3(0.163f,0.373f,0.6f)
                        + r*yy*bm*glm::vec3(1,0.5f,0) + r*ym*b*glm::vec3(0.5f,0,0.5f)
                        + rm*yy*b*glm::vec3(0,0.66f,0.2f) + r*yy*b*glm::vec3(0.2f,0.094f,0.029f);
                    out_colors.push_back(rgb);
                    out_widths.push_back(
                        cell_sz * step_xy * std::min(density_cpu[gi], 1.0f));
                }
            }
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
