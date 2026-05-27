// GPU brush paint simulation — Wetbrush-style Eulerian solver
// Based on: Chen et al., "Wetbrush: GPU-based 3D Painting Simulation
// at the Bristle Level", SIGGRAPH Asia 2015.
//
// Uses: Stable Fluids (Stam 1999) for advection/diffusion,
//       Fixed-point pressure projection (Algorithm 1),
//       Brightness-preserving RYB color mixing (Algorithm 2).

#include "RHI/shaderCompiler.h"
#include "GCore/Components/CurveComponent.h"
#include "GCore/Components/PointsComponent.h"
#include "GCore/GOP.h"
#include "GCore/algorithms/intersection.h"
#include "geom_node_base.h"
#include "GPUContext/compute_context.hpp"
#include "RHI/ResourceManager/resource_allocator.hpp"
#include "nvrhi/nvrhi.h"
#include "spdlog/spdlog.h"

#include <algorithm>
#include <cmath>
#include <vector>

// ============================================================
// Helpers (outside NODE_DEF_OPEN_SCOPE to avoid C-linkage issues)
// Types are in Ruzino namespace, so we must open it here too.
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

} // anonymous namespace
} // namespace Ruzino

NODE_DEF_OPEN_SCOPE

// Shader constants — must match common.slangh SimConstants
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
    int jacobi_mode;      // 0 = diffuse, 1 = pressure
    float jacobi_alpha;
};

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
    nvrhi::BufferHandle vertex_buf;   // float4: x, y, z, timestamp
    nvrhi::BufferHandle color_buf;    // float4: r, y, b, pressure

    // Shader programs
    ProgramHandle deposit_program;
    ProgramHandle advect_program;
    ProgramHandle jacobi_program;
    ProgramHandle divergence_program;
    ProgramHandle gradient_program;
    ProgramHandle damp_dry_program;

    // Grid state
    int grid_res = 0;
    int grid_alloc_res = 0;  // allocated resolution (for buffer sizes)
    float grid_paper = 0.0f;
    glm::vec2 grid_center = glm::vec2(0.0f);
    float grid_center_z = 0.0f;
    bool center_initialized = false;
    int deposited_count = 0;
    float last_sim_time = -1.0f;

    ~PaintSimStorage()
    {
        auto& rc = get_resource_allocator();
        auto destroy = [&](nvrhi::BufferHandle& h) {
            if (h) { rc.destroy(h); h = nullptr; }
        };
        destroy(density); destroy(density_tmp);
        destroy(color_r); destroy(color_y); destroy(color_b); destroy(color_tmp);
        destroy(vel_x); destroy(vel_x_tmp);
        destroy(vel_y); destroy(vel_y_tmp);
        destroy(wetness); destroy(wetness_tmp);
        destroy(height_field);
        destroy(pressure_a); destroy(pressure_b);
        destroy(divergence_buf);
        destroy(vertex_buf); destroy(color_buf);

        auto destroy_p = [&](ProgramHandle& h) {
            if (h) { rc.destroy(h); h = nullptr; }
        };
        destroy_p(deposit_program);
        destroy_p(advect_program);
        destroy_p(jacobi_program);
        destroy_p(divergence_program);
        destroy_p(gradient_program);
        destroy_p(damp_dry_program);
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

        // Zero-init all buffers
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
                          &storage.divergence_buf}) {
            cmd->writeBuffer(*buf, zeros.data(), n * sizeof(float));
        }
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        rc.destroy(cmd);
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

    if (!storage.deposit_program || !storage.advect_program ||
        !storage.jacobi_program || !storage.divergence_program ||
        !storage.gradient_program || !storage.damp_dry_program) {
        spdlog::error("brush_paint_sim: shader compilation failed");
        auto [geom, pts] = make_particles();
        params.set_output("Paint Particles", std::move(geom));
        params.set_storage(storage);
        return false;
    }

    // === DEPOSIT new vertices ===
    if (new_count > 0) {
        // Gather new vertex data
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

        // Create/upload vertex buffers
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

        // Dispatch deposit
        float cell_sz = storage.grid_paper / static_cast<float>(storage.grid_res);
        float eff_radius = std::max(brush_radius, cell_sz * 3.0f);

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

        auto cb_buf = rc.create(nvrhi::BufferDesc{}
            .setByteSize(sizeof(SimConstants))
            .setIsConstantBuffer(true)
            .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
            .setKeepInitialState(true)
            .setDebugName("sim_cb"));

        auto cb_upload = rc.create(CommandListDesc{});
        cb_upload->open();
        cb_upload->writeBuffer(cb_buf, &dep_cb, sizeof(SimConstants));
        cb_upload->close();
        device->executeCommandList(cb_upload);
        device->waitForIdle();
        rc.destroy(cb_upload);

        ProgramVars vars(rc, storage.deposit_program);
        vars["cb"] = cb_buf.Get();
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
        ctx.dispatch({}, vars, 1, 1);  // Single thread group
        ctx.finish();

        rc.destroy(cb_buf);
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
        // Sub-step to keep per-step displacement manageable
        float max_sub_dt = 2.0f / static_cast<float>(storage.grid_res);
        int substeps = std::max(1, static_cast<int>(std::ceil(sim_dt / max_sub_dt)));
        substeps = std::min(substeps, 16);
        float sub_dt = sim_dt / static_cast<float>(substeps);
        float cell_sz = storage.grid_paper / static_cast<float>(storage.grid_res);
        int total = storage.grid_res * storage.grid_res;

        for (int s = 0; s < substeps; s++) {
            // Create constant buffer for this substep
            SimConstants fluid_cb = {};
            fluid_cb.res = storage.grid_res;
            fluid_cb.cell_size = cell_sz;
            fluid_cb.paper_size = storage.grid_paper;
            fluid_cb.dt = sub_dt;
            fluid_cb.viscosity = viscosity;
            fluid_cb.diffusion = diffusion;
            fluid_cb.drying_rate = drying_rate;

            auto cb_buf = rc.create(nvrhi::BufferDesc{}
                .setByteSize(sizeof(SimConstants))
                .setIsConstantBuffer(true)
                .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
                .setKeepInitialState(true)
                .setDebugName("fluid_cb"));

            auto cb_up = rc.create(CommandListDesc{});
            cb_up->open();
            cb_up->writeBuffer(cb_buf, &fluid_cb, sizeof(SimConstants));
            cb_up->close();
            device->executeCommandList(cb_up);
            device->waitForIdle();
            rc.destroy(cb_up);

            // --- Velocity step ---
            // Diffuse velocity
            fluid_cb.jacobi_mode = 0;
            fluid_cb.jacobi_alpha = sub_dt * viscosity *
                static_cast<float>(storage.grid_res * storage.grid_res);
            {
                auto jcb = rc.create(nvrhi::BufferDesc{}
                    .setByteSize(sizeof(SimConstants)).setIsConstantBuffer(true)
                    .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
                    .setKeepInitialState(true).setDebugName("jacobi_cb"));
                auto jup = rc.create(CommandListDesc{});
                jup->open();
                jup->writeBuffer(jcb, &fluid_cb, sizeof(SimConstants));
                jup->close();
                device->executeCommandList(jup);
                device->waitForIdle();
                rc.destroy(jup);

                // Diffuse vel_x
                dispatch_field(rc, storage.jacobi_program,
                    {{"field_in", storage.vel_x}, {"rhs", storage.vel_x}},
                    {{"field_out", storage.vel_x_tmp}}, jcb, total);
                std::swap(storage.vel_x, storage.vel_x_tmp);

                // Diffuse vel_y
                dispatch_field(rc, storage.jacobi_program,
                    {{"field_in", storage.vel_y}, {"rhs", storage.vel_y}},
                    {{"field_out", storage.vel_y_tmp}}, jcb, total);
                std::swap(storage.vel_y, storage.vel_y_tmp);

                rc.destroy(jcb);
            }

            // Project (Fixed-point, Algorithm 1: L=3, 2 Jacobi per L)
            for (int fp = 0; fp < 3; fp++) {
                // Divergence
                dispatch_field(rc, storage.divergence_program,
                    {{"vel_x", storage.vel_x}, {"vel_y", storage.vel_y}},
                    {{"div_out", storage.divergence_buf}}, cb_buf, total);

                // 2 Jacobi iterations on pressure (ping-pong)
                fluid_cb.jacobi_mode = 1;
                auto pcb = rc.create(nvrhi::BufferDesc{}
                    .setByteSize(sizeof(SimConstants)).setIsConstantBuffer(true)
                    .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
                    .setKeepInitialState(true).setDebugName("press_cb"));
                auto pup = rc.create(CommandListDesc{});
                pup->open();
                pup->writeBuffer(pcb, &fluid_cb, sizeof(SimConstants));
                pup->close();
                device->executeCommandList(pup);
                device->waitForIdle();
                rc.destroy(pup);

                for (int ji = 0; ji < 2; ji++) {
                    dispatch_field(rc, storage.jacobi_program,
                        {{"field_in", storage.pressure_a},
                         {"rhs", storage.divergence_buf}},
                        {{"field_out", storage.pressure_b}}, pcb, total);
                    std::swap(storage.pressure_a, storage.pressure_b);
                }
                rc.destroy(pcb);

                // Gradient subtraction
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
                auto pcb2 = rc.create(nvrhi::BufferDesc{}
                    .setByteSize(sizeof(SimConstants)).setIsConstantBuffer(true)
                    .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
                    .setKeepInitialState(true).setDebugName("press2_cb"));
                auto p2up = rc.create(CommandListDesc{});
                p2up->open();
                p2up->writeBuffer(pcb2, &fluid_cb, sizeof(SimConstants));
                p2up->close();
                device->executeCommandList(p2up);
                device->waitForIdle();
                rc.destroy(p2up);

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
            // Advect density, color, wetness
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

            // Diffuse density
            fluid_cb.jacobi_mode = 0;
            fluid_cb.jacobi_alpha = sub_dt * diffusion *
                static_cast<float>(storage.grid_res * storage.grid_res);
            {
                auto dcb = rc.create(nvrhi::BufferDesc{}
                    .setByteSize(sizeof(SimConstants)).setIsConstantBuffer(true)
                    .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
                    .setKeepInitialState(true).setDebugName("diff_cb"));
                auto dup = rc.create(CommandListDesc{});
                dup->open();
                dup->writeBuffer(dcb, &fluid_cb, sizeof(SimConstants));
                dup->close();
                device->executeCommandList(dup);
                device->waitForIdle();
                rc.destroy(dup);

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

            rc.destroy(cb_buf);
        }
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

    // Convert grid → particles (CPU subsample)
    auto [particles, pts] = make_particles();
    std::vector<glm::vec3> out_pts;
    std::vector<glm::vec3> out_colors;
    std::vector<float> out_widths;

    constexpr float threshold = 0.001f;
    int step = std::max(1, storage.grid_res / 64);
    float cell_sz = storage.grid_paper / static_cast<float>(storage.grid_res);

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

                float rgb_r, rgb_g, rgb_b;
                // RYB→RGB inline (Gossett & Chen)
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
