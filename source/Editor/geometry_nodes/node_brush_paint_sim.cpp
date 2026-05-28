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

} // anonymous namespace
} // namespace Ruzino

NODE_DEF_OPEN_SCOPE

// Shader constants — must match common.slangh structs
struct SimConstants {
    int res;
    float cell_size;
    float paper_size;
    float dt;

    float viscosity;
    float diffusion;
    float drying_rate;
    float brush_radius;

    float ink_amount;
    int num_vertices;
    float center_x;
    float center_y;

    float center_z;
    float effective_radius;
    int jacobi_mode;
    float jacobi_alpha;
};

struct BristleConstants {
    int num_bristles;
    int verts_per_bristle;
    int samples_per_bristle;
    float beta_B;

    float dt;
    float brush_pos_x, brush_pos_y, brush_pos_z;
    float brush_vel_x, brush_vel_y, brush_vel_z;
    float brush_angular_vel;
    float brush_rotation;

    float brush_radius;
    float spring_k;
    float damping;
    int grid_res;
    float cell_size;
    float paper_size;
    float grid_center_x, grid_center_y;
};

struct ParticleConstants {
    int max_particles;
    float dt;
    float D0;
    float friction_delta;

    float flip_gamma;
    int grid_res;
    float cell_size;
    float paper_size;
    float grid_center_x, grid_center_y;
    float brush_pos_x, brush_pos_y;
    float brush_radius;
    int emit_mode;
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

    // Grid field buffers
    nvrhi::BufferHandle density, density_tmp;
    nvrhi::BufferHandle color_r, color_y, color_b, color_tmp;
    nvrhi::BufferHandle vel_x, vel_x_tmp;
    nvrhi::BufferHandle vel_y, vel_y_tmp;
    nvrhi::BufferHandle wetness, wetness_tmp;
    nvrhi::BufferHandle height_field;
    nvrhi::BufferHandle pressure_a, pressure_b;
    nvrhi::BufferHandle divergence_buf;

    // Per-frame upload buffers
    nvrhi::BufferHandle vertex_buf;
    nvrhi::BufferHandle color_buf;

    // --- Bristle model ---
    static constexpr int NUM_BRISTLES = 80;
    static constexpr int VERTS_PER_BRISTLE = 10;
    static constexpr int SAMPLES_PER_BRISTLE = 32;

    nvrhi::BufferHandle bristle_data;   // {float2 pos, vel} * Nb*M (structured)
    nvrhi::BufferHandle sample_pos;     // float2 * Nb*S
    nvrhi::BufferHandle sample_vel;     // float2 * Nb*S
    nvrhi::BufferHandle sample_color;   // float4 * Nb*S
    nvrhi::BufferHandle bristle_density; // N*N (accumulation)
    nvrhi::BufferHandle bristle_vel_x;  // N*N
    nvrhi::BufferHandle bristle_vel_y;  // N*N
    nvrhi::BufferHandle bristle_color_r; // N*N
    nvrhi::BufferHandle bristle_color_y; // N*N
    nvrhi::BufferHandle bristle_color_b; // N*N

    // --- FLIP/PIC particles ---
    static constexpr int MAX_PARTICLES = 16384;

    nvrhi::BufferHandle ptcl_pos;
    nvrhi::BufferHandle ptcl_vel;
    nvrhi::BufferHandle ptcl_color;
    nvrhi::BufferHandle ptcl_alive;
    nvrhi::BufferHandle ptcl_counter;   // ByteAddressBuffer, 4 bytes
    nvrhi::BufferHandle ptcl_density;   // N*N
    nvrhi::BufferHandle ptcl_vel_x;     // N*N
    nvrhi::BufferHandle ptcl_vel_y;     // N*N
    nvrhi::BufferHandle vel_x_old;      // N*N (snapshot for FLIP)
    nvrhi::BufferHandle vel_y_old;      // N*N
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
    int grid_alloc_res = 0;
    float grid_paper = 0.0f;
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
            release(wetness); release(wetness_tmp);
            release(height_field);
            release(pressure_a); release(pressure_b);
            release(divergence_buf);
            release(vertex_buf); release(color_buf);
            release(bristle_data); release(sample_pos); release(sample_vel);
            release(sample_color); release(lambda_buf);
            release(bristle_density); release(bristle_vel_x); release(bristle_vel_y);
            release(bristle_color_r); release(bristle_color_y); release(bristle_color_b);
            release(ptcl_pos); release(ptcl_vel); release(ptcl_color);
            release(ptcl_alive); release(ptcl_counter);
            release(ptcl_density); release(ptcl_vel_x); release(ptcl_vel_y);
            release(vel_x_old); release(vel_y_old);
            release(ptcl_pos_b); release(ptcl_vel_b); release(ptcl_color_b); release(ptcl_alive_b);
            release(deposit_program); release(advect_program);
            release(jacobi_program); release(divergence_program);
            release(gradient_program); release(damp_dry_program);
            release(bristle_sim_program); release(bristle_raster_program);
            release(bristle_merge_program); release(field_clear_program);
            release(bristle_density_constraint_program);
            release(bristle_resample_program);
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
        destroy_buf(wetness); destroy_buf(wetness_tmp);
        destroy_buf(height_field);
        destroy_buf(pressure_a); destroy_buf(pressure_b);
        destroy_buf(divergence_buf);
        destroy_buf(vertex_buf); destroy_buf(color_buf);
        destroy_buf(bristle_data); destroy_buf(sample_pos); destroy_buf(sample_vel);
        destroy_buf(sample_color); destroy_buf(lambda_buf);
        destroy_buf(bristle_density); destroy_buf(bristle_vel_x); destroy_buf(bristle_vel_y);
        destroy_buf(bristle_color_r); destroy_buf(bristle_color_y); destroy_buf(bristle_color_b);
        destroy_buf(ptcl_pos); destroy_buf(ptcl_vel); destroy_buf(ptcl_color);
        destroy_buf(ptcl_alive); destroy_buf(ptcl_counter);
        destroy_buf(ptcl_density); destroy_buf(ptcl_vel_x); destroy_buf(ptcl_vel_y);
        destroy_buf(vel_x_old); destroy_buf(vel_y_old);
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
        storage.center_initialized = true;

        spdlog::info(
            "brush_paint_sim: grid {}x{}, paper={:.3f}, cell={:.5f}",
            resolution, resolution, storage.grid_paper,
            storage.grid_paper / static_cast<float>(resolution));
    }

    // Create or resize GPU buffers
    int n = storage.grid_res * storage.grid_res;
    if (storage.grid_alloc_res != storage.grid_res) {
        storage.grid_alloc_res = storage.grid_res;
        storage.deposited_count = 0;
        storage.last_sim_time = -1.0f;
        already_deposited = 0;
        new_count = static_cast<int>(vertices.size());
        storage.bristles_initialized = false;
        storage.particles_initialized = false;

        auto make_buf = [&](const char* name) -> nvrhi::BufferHandle {
            return create_field_buffer(rc, n, name);
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
        storage.bristle_color_r = make_buf("bristle_color_r");
        storage.bristle_color_y = make_buf("bristle_color_y");
        storage.bristle_color_b = make_buf("bristle_color_b");

        // Particle accumulation grids + FLIP snapshot
        storage.ptcl_density = make_buf("ptcl_density");
        storage.ptcl_vel_x   = make_buf("ptcl_vel_x");
        storage.ptcl_vel_y   = make_buf("ptcl_vel_y");
        storage.vel_x_old    = make_buf("vel_x_old");
        storage.vel_y_old    = make_buf("vel_y_old");

        // Zero-init all field buffers
        std::vector<float> zeros(n, 0.0f);
        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        for (auto* buf : {&storage.density, &storage.density_tmp,
                          &storage.color_r, &storage.color_y, &storage.color_b,
                          &storage.color_tmp, &storage.vel_x, &storage.vel_x_tmp,
                          &storage.vel_y, &storage.vel_y_tmp,
                          &storage.wetness, &storage.wetness_tmp,
                          &storage.height_field,
                          &storage.pressure_a, &storage.pressure_b,
                          &storage.divergence_buf,
                          &storage.bristle_density, &storage.bristle_vel_x,
                          &storage.bristle_vel_y,
                          &storage.bristle_color_r, &storage.bristle_color_y,
                          &storage.bristle_color_b,
                          &storage.ptcl_density, &storage.ptcl_vel_x,
                          &storage.ptcl_vel_y,
                          &storage.vel_x_old, &storage.vel_y_old}) {
            cmd->writeBuffer(*buf, zeros.data(), n * sizeof(float));
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
        storage.bristle_data  = create_typed_buffer(
            rc, Nb * M, sizeof(float) * 4, "bristle_data"); // float2 pos, float2 vel
        storage.lambda_buf    = create_typed_buffer(
            rc, Nb * M, sizeof(float), "lambda_buf");
        storage.sample_pos   = create_typed_buffer(
            rc, Nb * S, sizeof(float) * 2, "sample_pos");
        storage.sample_vel   = create_typed_buffer(
            rc, Nb * S, sizeof(float) * 2, "sample_vel");
        storage.sample_color = create_typed_buffer(
            rc, Nb * S, sizeof(float) * 4, "sample_color");

        // Zero-init
        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        std::vector<float> zeros_bristle(Nb * M * 4, 0.0f);
        cmd->writeBuffer(storage.bristle_data, zeros_bristle.data(),
                         zeros_bristle.size() * sizeof(float));
        std::vector<float> zeros_sample(Nb * S * 4, 0.0f);
        cmd->writeBuffer(storage.sample_pos, zeros_sample.data(),
                         Nb * S * sizeof(float) * 2);
        cmd->writeBuffer(storage.sample_vel, zeros_sample.data(),
                         Nb * S * sizeof(float) * 2);
        cmd->writeBuffer(storage.sample_color, zeros_sample.data(),
                         Nb * S * sizeof(float) * 4);
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        rc.destroy(cmd);

        storage.bristles_initialized = true;
    }

    // Initialize particle buffers
    int max_ptcl = PaintSimStorage::MAX_PARTICLES;
    if (!storage.particles_initialized) {
        storage.ptcl_pos    = create_typed_buffer(rc, max_ptcl, sizeof(float) * 2, "ptcl_pos");
        storage.ptcl_vel    = create_typed_buffer(rc, max_ptcl, sizeof(float) * 2, "ptcl_vel");
        storage.ptcl_color  = create_typed_buffer(rc, max_ptcl, sizeof(float) * 4, "ptcl_color");
        storage.ptcl_alive  = create_typed_buffer(rc, max_ptcl, sizeof(uint32_t), "ptcl_alive");
        storage.ptcl_counter = create_byte_buffer(rc, sizeof(uint32_t), "ptcl_counter");
        storage.ptcl_pos_b   = create_typed_buffer(rc, max_ptcl, sizeof(float) * 2, "ptcl_pos_b");
        storage.ptcl_vel_b   = create_typed_buffer(rc, max_ptcl, sizeof(float) * 2, "ptcl_vel_b");
        storage.ptcl_color_b = create_typed_buffer(rc, max_ptcl, sizeof(float) * 4, "ptcl_color_b");
        storage.ptcl_alive_b = create_typed_buffer(rc, max_ptcl, sizeof(uint32_t), "ptcl_alive_b");

        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        std::vector<float> zeros_ptcl(max_ptcl * 4, 0.0f);
        cmd->writeBuffer(storage.ptcl_pos, zeros_ptcl.data(), max_ptcl * sizeof(float) * 2);
        cmd->writeBuffer(storage.ptcl_vel, zeros_ptcl.data(), max_ptcl * sizeof(float) * 2);
        cmd->writeBuffer(storage.ptcl_color, zeros_ptcl.data(), max_ptcl * sizeof(float) * 4);
        std::vector<uint32_t> zeros_u(max_ptcl, 0);
        cmd->writeBuffer(storage.ptcl_alive, zeros_u.data(), max_ptcl * sizeof(uint32_t));
        uint32_t zero_c = 0;
        cmd->writeBuffer(storage.ptcl_counter, &zero_c, sizeof(uint32_t));
        cmd->writeBuffer(storage.ptcl_pos_b, zeros_ptcl.data(), max_ptcl * sizeof(float) * 2);
        cmd->writeBuffer(storage.ptcl_vel_b, zeros_ptcl.data(), max_ptcl * sizeof(float) * 2);
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

    if (!storage.deposit_program || !storage.advect_program ||
        !storage.jacobi_program || !storage.divergence_program ||
        !storage.gradient_program || !storage.damp_dry_program ||
        !storage.bristle_sim_program || !storage.bristle_density_constraint_program ||
        !storage.bristle_resample_program || !storage.bristle_raster_program ||
        !storage.bristle_merge_program || !storage.field_clear_program ||
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
    float brush_angular_vel = 0.0f;

    if (!vertices.empty()) {
        int last = static_cast<int>(vertices.size()) - 1;
        brush_pos_3d = vertices[last];
        brush_pos_3d.x -= storage.grid_center.x;
        brush_pos_3d.y -= storage.grid_center.y;

        if (last > 0) {
            brush_vel_3d = vertices[last] - vertices[last - 1];
            // Estimate rotation from velocity direction
            brush_rotation = atan2(brush_vel_3d.y, brush_vel_3d.x);
            if (last > 1) {
                float prev_rot = atan2(
                    vertices[last-1].y - vertices[last-2].y,
                    vertices[last-1].x - vertices[last-2].x);
                brush_angular_vel = brush_rotation - prev_rot;
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
        bc.brush_angular_vel = brush_angular_vel;
        bc.brush_rotation = brush_rotation;
        bc.brush_radius = brush_radius;
        bc.spring_k = 50.0f;
        bc.damping = 5.0f;
        bc.grid_res = storage.grid_res;
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

        // Step 3: Resample bristle chains → samples
        dispatch_raw(rc, storage.bristle_resample_program,
            {{"bristle_data", storage.bristle_data}},
            {{"sample_pos", storage.sample_pos},
             {"sample_vel", storage.sample_vel},
             {"sample_color", storage.sample_color}},
            bristle_cb, Nb);

        // Step 4: Clear bristle accumulation grids
        dispatch_field(rc, storage.field_clear_program,
            {},
            {{"field", storage.bristle_density}}, nullptr, n);
        dispatch_field(rc, storage.field_clear_program,
            {},
            {{"field", storage.bristle_vel_x}}, nullptr, n);
        dispatch_field(rc, storage.field_clear_program,
            {},
            {{"field", storage.bristle_vel_y}}, nullptr, n);
        dispatch_field(rc, storage.field_clear_program,
            {},
            {{"field", storage.bristle_color_r}}, nullptr, n);
        dispatch_field(rc, storage.field_clear_program,
            {},
            {{"field", storage.bristle_color_y}}, nullptr, n);
        dispatch_field(rc, storage.field_clear_program,
            {},
            {{"field", storage.bristle_color_b}}, nullptr, n);

        // Step 5: Rasterize samples → accumulation grids
        dispatch_raw(rc, storage.bristle_raster_program,
            {{"sample_pos", storage.sample_pos},
             {"sample_color", storage.sample_color},
             {"sample_vel", storage.sample_vel}},
            {{"bristle_density", storage.bristle_density},
             {"bristle_vel_x", storage.bristle_vel_x},
             {"bristle_vel_y", storage.bristle_vel_y},
             {"bristle_color_r", storage.bristle_color_r},
             {"bristle_color_y", storage.bristle_color_y},
             {"bristle_color_b", storage.bristle_color_b}},
            bristle_cb, Nb * S);

        // Step 6: Merge bristle grids into main simulation grids
        nvrhi::BufferHandle merge_cb;
        SimConstants mc = {};
        mc.res = storage.grid_res;
        mc.cell_size = cell_sz;
        mc.paper_size = storage.grid_paper;
        mc.ink_amount = ink_amount;
        upload_constant_buffer(rc, device, &mc, sizeof(SimConstants),
                               "merge_cb", merge_cb);

        dispatch_raw(rc, storage.bristle_merge_program,
            {{"bristle_density", storage.bristle_density},
             {"bristle_vel_x", storage.bristle_vel_x},
             {"bristle_vel_y", storage.bristle_vel_y},
             {"bristle_color_r", storage.bristle_color_r},
             {"bristle_color_y", storage.bristle_color_y},
             {"bristle_color_b", storage.bristle_color_b}},
            {{"density", storage.density},
             {"color_r", storage.color_r},
             {"color_y", storage.color_y},
             {"color_b", storage.color_b},
             {"vel_x", storage.vel_x},
             {"vel_y", storage.vel_y},
             {"wetness", storage.wetness}},
            merge_cb, n);

        rc.destroy(bristle_cb);
        rc.destroy(merge_cb);
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
        pc.cell_size = cell_sz;
        pc.paper_size = storage.grid_paper;
        pc.grid_center_x = storage.grid_center.x;
        pc.grid_center_y = storage.grid_center.y;
        pc.brush_pos_x = brush_pos_3d.x;
        pc.brush_pos_y = brush_pos_3d.y;
        pc.brush_radius = brush_radius;

        nvrhi::BufferHandle ptcl_cb;
        upload_constant_buffer(rc, device, &pc, sizeof(ParticleConstants),
                               "ptcl_cb", ptcl_cb);

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
            emit1_cb, n);
        rc.destroy(emit1_cb);

        // Update particles (ping-pong)
        dispatch_raw(rc, storage.ptcl_update_program,
            {{"ptcl_pos", storage.ptcl_pos},
             {"ptcl_vel", storage.ptcl_vel},
             {"ptcl_color", storage.ptcl_color},
             {"ptcl_alive", storage.ptcl_alive}},
            {{"ptcl_pos_out", storage.ptcl_pos_b},
             {"ptcl_vel_out", storage.ptcl_vel_b},
             {"ptcl_alive_out", storage.ptcl_alive_b}},
            ptcl_cb, max_ptcl);
        std::swap(storage.ptcl_pos, storage.ptcl_pos_b);
        std::swap(storage.ptcl_vel, storage.ptcl_vel_b);
        std::swap(storage.ptcl_alive, storage.ptcl_alive_b);

        // Clear particle accum grids
        dispatch_field(rc, storage.field_clear_program,
            {}, {{"field", storage.ptcl_density}}, nullptr, n);
        dispatch_field(rc, storage.field_clear_program,
            {}, {{"field", storage.ptcl_vel_x}}, nullptr, n);
        dispatch_field(rc, storage.field_clear_program,
            {}, {{"field", storage.ptcl_vel_y}}, nullptr, n);

        // Rasterize particles
        dispatch_raw(rc, storage.ptcl_raster_program,
            {{"ptcl_pos", storage.ptcl_pos},
             {"ptcl_color", storage.ptcl_color},
             {"ptcl_vel", storage.ptcl_vel},
             {"ptcl_alive", storage.ptcl_alive}},
            {{"ptcl_density", storage.ptcl_density},
             {"ptcl_vel_x", storage.ptcl_vel_x},
             {"ptcl_vel_y", storage.ptcl_vel_y}},
            ptcl_cb, max_ptcl);

        // Merge particle grids into main grids (reuse bristle merge logic)
        // Simple additive merge for particle contribution
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
             {"bristle_color_r", storage.ptcl_density},  // reuse density as color placeholder
             {"bristle_color_y", storage.ptcl_density},
             {"bristle_color_b", storage.ptcl_density}},
            {{"density", storage.density},
             {"color_r", storage.color_r},
             {"color_y", storage.color_y},
             {"color_b", storage.color_b},
             {"vel_x", storage.vel_x},
             {"vel_y", storage.vel_y},
             {"wetness", storage.wetness}},
            merge_cb, n);
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
        int total = storage.grid_res * storage.grid_res;

        for (int s = 0; s < substeps; s++) {
            SimConstants fluid_cb = {};
            fluid_cb.res = storage.grid_res;
            fluid_cb.cell_size = cell_sz;
            fluid_cb.paper_size = storage.grid_paper;
            fluid_cb.dt = sub_dt;
            fluid_cb.viscosity = viscosity;
            fluid_cb.diffusion = diffusion;
            fluid_cb.drying_rate = drying_rate;

            nvrhi::BufferHandle cb_buf;
            upload_constant_buffer(rc, device, &fluid_cb, sizeof(SimConstants),
                                   "fluid_cb", cb_buf);

            // Snapshot velocity for FLIP
            {
                auto snap_cmd = rc.create(CommandListDesc{});
                snap_cmd->open();
                snap_cmd->copyBuffer(storage.vel_x_old, 0, storage.vel_x, 0, n * sizeof(float));
                snap_cmd->copyBuffer(storage.vel_y_old, 0, storage.vel_y, 0, n * sizeof(float));
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
                    {{"field_in", storage.vel_x}, {"rhs", storage.vel_x}},
                    {{"field_out", storage.vel_x_tmp}}, jcb, total);
                std::swap(storage.vel_x, storage.vel_x_tmp);

                dispatch_field(rc, storage.jacobi_program,
                    {{"field_in", storage.vel_y}, {"rhs", storage.vel_y}},
                    {{"field_out", storage.vel_y_tmp}}, jcb, total);
                std::swap(storage.vel_y, storage.vel_y_tmp);

                rc.destroy(jcb);
            }

            // Project (Fixed-point, Algorithm 1: L=3, 2 Jacobi per L)
            for (int fp = 0; fp < 3; fp++) {
                dispatch_field(rc, storage.divergence_program,
                    {{"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}},
                    {{"div_out", storage.divergence_buf}}, cb_buf, total);

                fluid_cb.jacobi_mode = 1;
                nvrhi::BufferHandle pcb;
                upload_constant_buffer(rc, device, &fluid_cb, sizeof(SimConstants),
                                       "press_cb", pcb);

                for (int ji = 0; ji < 2; ji++) {
                    dispatch_field(rc, storage.jacobi_program,
                        {{"field_in", storage.pressure_a},
                         {"rhs", storage.divergence_buf}},
                        {{"field_out", storage.pressure_b}}, pcb, total);
                    std::swap(storage.pressure_a, storage.pressure_b);
                }
                rc.destroy(pcb);

                dispatch_field(rc, storage.gradient_program,
                    {{"pressure", storage.pressure_a}},
                    {{"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}},
                    cb_buf, total);
            }

            // Advect velocity
            dispatch_field(rc, storage.advect_program,
                {{"field_in", storage.vel_x},
                 {"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}},
                {{"field_out", storage.vel_x_tmp}}, cb_buf, total);
            std::swap(storage.vel_x, storage.vel_x_tmp);

            dispatch_field(rc, storage.advect_program,
                {{"field_in", storage.vel_y},
                 {"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}},
                {{"field_out", storage.vel_y_tmp}}, cb_buf, total);
            std::swap(storage.vel_y, storage.vel_y_tmp);

            // Project again
            for (int fp = 0; fp < 3; fp++) {
                dispatch_field(rc, storage.divergence_program,
                    {{"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}},
                    {{"div_out", storage.divergence_buf}}, cb_buf, total);

                fluid_cb.jacobi_mode = 1;
                nvrhi::BufferHandle pcb2;
                upload_constant_buffer(rc, device, &fluid_cb, sizeof(SimConstants),
                                       "press2_cb", pcb2);

                for (int ji = 0; ji < 2; ji++) {
                    dispatch_field(rc, storage.jacobi_program,
                        {{"field_in", storage.pressure_a},
                         {"rhs", storage.divergence_buf}},
                        {{"field_out", storage.pressure_b}}, pcb2, total);
                    std::swap(storage.pressure_a, storage.pressure_b);
                }
                rc.destroy(pcb2);

                dispatch_field(rc, storage.gradient_program,
                    {{"pressure", storage.pressure_a}},
                    {{"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}},
                    cb_buf, total);
            }

            // --- Scalar step ---
            dispatch_field(rc, storage.advect_program,
                {{"field_in", storage.density},
                 {"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}},
                {{"field_out", storage.density_tmp}}, cb_buf, total);
            std::swap(storage.density, storage.density_tmp);

            dispatch_field(rc, storage.advect_program,
                {{"field_in", storage.color_r},
                 {"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}},
                {{"field_out", storage.color_tmp}}, cb_buf, total);
            std::swap(storage.color_r, storage.color_tmp);

            dispatch_field(rc, storage.advect_program,
                {{"field_in", storage.color_y},
                 {"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}},
                {{"field_out", storage.color_tmp}}, cb_buf, total);
            std::swap(storage.color_y, storage.color_tmp);

            dispatch_field(rc, storage.advect_program,
                {{"field_in", storage.color_b},
                 {"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}},
                {{"field_out", storage.color_tmp}}, cb_buf, total);
            std::swap(storage.color_b, storage.color_tmp);

            dispatch_field(rc, storage.advect_program,
                {{"field_in", storage.wetness},
                 {"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}},
                {{"field_out", storage.wetness_tmp}}, cb_buf, total);
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
                    {{"field_in", storage.density}, {"rhs", storage.density}},
                    {{"field_out", storage.density_tmp}}, dcb, total);
                std::swap(storage.density, storage.density_tmp);

                dispatch_field(rc, storage.jacobi_program,
                    {{"field_in", storage.wetness}, {"rhs", storage.wetness}},
                    {{"field_out", storage.wetness_tmp}}, dcb, total);
                std::swap(storage.wetness, storage.wetness_tmp);
                rc.destroy(dcb);
            }

            // Damp + dry
            dispatch_field(rc, storage.damp_dry_program,
                {},
                {{"vel_x", storage.vel_x}, {"vel_y", storage.vel_y},
                 {"wetness", storage.wetness}},
                cb_buf, total);

            // FLIP/PIC velocity update for particles
            if (storage.particles_initialized) { // re-enable for testing
                ParticleConstants pc = {};
                pc.max_particles = max_ptcl;
                pc.dt = sub_dt;
                pc.D0 = brush_radius * 3.0f;
                pc.flip_gamma = 0.8f;
                pc.grid_res = storage.grid_res;
                pc.cell_size = cell_sz;
                pc.paper_size = storage.grid_paper;
                pc.grid_center_x = storage.grid_center.x;
                pc.grid_center_y = storage.grid_center.y;
                pc.brush_pos_x = brush_pos_3d.x;
                pc.brush_pos_y = brush_pos_3d.y;
                pc.brush_radius = brush_radius;

                nvrhi::BufferHandle flip_cb;
                upload_constant_buffer(rc, device, &pc, sizeof(ParticleConstants),
                                       "flip_cb", flip_cb);

                dispatch_raw(rc, storage.ptcl_flip_pic_program,
                    {{"ptcl_pos", storage.ptcl_pos},
                     {"ptcl_alive", storage.ptcl_alive},
                     {"vel_x_old", storage.vel_x_old},
                     {"vel_y_old", storage.vel_y_old},
                     {"vel_x_new", storage.vel_x},
                     {"vel_y_new", storage.vel_y}},
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
        pc.cell_size = cell_sz;
        pc.paper_size = storage.grid_paper;
        pc.grid_center_x = storage.grid_center.x;
        pc.grid_center_y = storage.grid_center.y;
        pc.brush_pos_x = brush_pos_3d.x;
        pc.brush_pos_y = brush_pos_3d.y;
        pc.brush_radius = brush_radius;

        nvrhi::BufferHandle maint_cb;
        upload_constant_buffer(rc, device, &pc, sizeof(ParticleConstants),
                               "maint_cb", maint_cb);

        // Particle to grid (absorb distant particles)
        dispatch_raw(rc, storage.ptcl_to_grid_program,
            {{"ptcl_pos", storage.ptcl_pos},
             {"ptcl_color", storage.ptcl_color},
             {"ptcl_alive", storage.ptcl_alive}},
            {{"density", storage.density},
             {"color_r", storage.color_r},
             {"color_y", storage.color_y},
             {"color_b", storage.color_b},
             {"ptcl_alive_out", storage.ptcl_alive_b}},
            maint_cb, max_ptcl);
        std::swap(storage.ptcl_alive, storage.ptcl_alive_b);

        // Grid to particle (emit near brush)
        dispatch_raw(rc, storage.grid_to_ptcl_program,
            {{"density", storage.density},
             {"color_r", storage.color_r},
             {"color_y", storage.color_y},
             {"color_b", storage.color_b}},
            {{"ptcl_counter", storage.ptcl_counter},
             {"ptcl_pos", storage.ptcl_pos},
             {"ptcl_vel", storage.ptcl_vel},
             {"ptcl_color", storage.ptcl_color},
             {"ptcl_alive", storage.ptcl_alive}},
            maint_cb, n);

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
    auto readback = [&](nvrhi::BufferHandle field) -> std::vector<float> {
        std::vector<float> data(n);
        auto rb = rc.create(nvrhi::BufferDesc{}
            .setByteSize(n * sizeof(float))
            .setCpuAccess(nvrhi::CpuAccessMode::Read)
            .setDebugName("readback"));
        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        cmd->copyBuffer(rb, 0, field, 0, n * sizeof(float));
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        void* mapped = device->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
        memcpy(data.data(), mapped, n * sizeof(float));
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
    int step = std::max(1, storage.grid_res / 64);

    for (int y = 0; y < storage.grid_res; y += step) {
        for (int x = 0; x < storage.grid_res; x += step) {
            int gi = y * storage.grid_res + x;
            if (density_cpu[gi] > threshold) {
                float gx = (x + 0.5f) * cell_sz - storage.grid_paper * 0.5f;
                float gy = (y + 0.5f) * cell_sz - storage.grid_paper * 0.5f;
                out_pts.push_back(glm::vec3(
                    gx + storage.grid_center.x,
                    gy + storage.grid_center.y,
                    storage.grid_center_z));

                float r = cr_cpu[gi], yy = cy_cpu[gi], b = cb_cpu[gi];
                float rm = 1-r, ym = 1-yy, bm = 1-b;
                glm::vec3 rgb = rm*ym*bm*glm::vec3(1,1,1) + r*ym*bm*glm::vec3(1,0,0)
                    + rm*yy*bm*glm::vec3(1,1,0) + rm*ym*b*glm::vec3(0.163f,0.373f,0.6f)
                    + r*yy*bm*glm::vec3(1,0.5f,0) + r*ym*b*glm::vec3(0.5f,0,0.5f)
                    + rm*yy*b*glm::vec3(0,0.66f,0.2f) + r*yy*b*glm::vec3(0.2f,0.094f,0.029f);
                out_colors.push_back(rgb);
                out_widths.push_back(
                    cell_sz * step * std::min(density_cpu[gi], 1.0f));
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
