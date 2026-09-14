// terrain_erode_hydraulic: hydraulic erosion for the terrain family.
//
// Primary path — virtual pipes (Mei, Decaudin & Hu 2007) as slang compute:
// four dispatches per solver step (flux -> water/velocity ->
// erosion/deposition -> sediment advection), buffers stay resident on the
// GPU for the whole solve and only the field layers are read back once.
// Fallback — the CPU droplet implementation (TerrainGen/src/Erosion.cpp)
// when the user picks "Droplets (CPU)" or no RHI device is alive.
//
// Stateless per cook: the solve always starts from the input heightfield
// (Houdini HeightField semantics — tweak any upstream parameter and the
// erosion re-runs deterministically).

#include <algorithm>
#include <string>
#include <vector>

#include "GCore/Components/MeshComponent.h"
#include "GCore/GOP.h"
#include "TerrainGen/Erosion.h"
#include "TerrainGen/Heightfield.h"
#include "geom_node_base.h"
#include "terrain_carry.hpp"
#include "terrain_gpu_common.hpp"

using namespace TerrainGen;

// The GPU solve helper lives in Ruzino so unqualified lookup finds
// is_gpu_alive / RHI::get_device / TerrainConstantsCB.
namespace Ruzino {
namespace {
    using namespace terrain_gpu;

    // Returns false (caller falls back to CPU droplets) when no device is alive
    // or a shader fails to compile.
    bool solve_pipes_gpu(
        Heightfield& hf,
        const TerrainConstantsCB& cb_init,
        int iterations)
    {
        // Lazily bring up the RHI device + resource allocator (idempotent).
        Ruzino::init_gpu_geometry_algorithms();
        if (!is_gpu_alive())
            return false;
        nvrhi::IDevice* device = RHI::get_device();
        if (!device)
            return false;
        if (iterations < 1)
            return false;
        auto& rc = get_resource_allocator();

        const int res = hf.res;
        const size_t n = static_cast<size_t>(res) * res;
        if (res < 2 || hf.height.size() != n)
            return false;

        // ---- Rent solver buffers (released by the guards on every exit) ----
        nvrhi::BufferHandle b[2], d[2], s[3], flux[4][2], vel_u, vel_v, wear;
        std::vector<nvrhi::BufferHandle> leased;
        leased.reserve(18);
        const auto lease_field = [&](const char* name) {
            nvrhi::BufferHandle h =
                terrain_gpu::create_field_buffer(rc, n, name);
            leased.push_back(h);
            return h;
        };
        b[0] = lease_field("terrain_b0");
        b[1] = lease_field("terrain_b1");
        d[0] = lease_field("terrain_d0");
        d[1] = lease_field("terrain_d1");
        s[0] = lease_field("terrain_s0");
        s[1] = lease_field("terrain_s1");
        s[2] = lease_field("terrain_s2");
        for (int k = 0; k < 4; ++k) {
            flux[k][0] = lease_field("terrain_flux_cur");
            flux[k][1] = lease_field("terrain_flux_nxt");
        }
        vel_u = lease_field("terrain_vel_u");
        vel_v = lease_field("terrain_vel_v");
        wear = lease_field("terrain_wear");

        std::vector<ProgramHandle> programs;
        const auto lease_program = [&](const char* file) {
            ProgramHandle p = terrain_gpu::compile_shader(rc, file);
            if (p)
                programs.push_back(p);
            return p;
        };
        struct ProgGuard {
            ResourceAllocator& rc;
            std::vector<ProgramHandle>& v;
            ~ProgGuard()
            {
                for (auto& p : v)
                    if (p)
                        rc.destroy(p);
            }
        } prog_guard{ rc, programs };

        struct BufGuard {
            ResourceAllocator& rc;
            std::vector<nvrhi::BufferHandle>& v;
            ~BufGuard()
            {
                for (auto& h : v)
                    if (h)
                        rc.destroy(h);
            }
        } buf_guard{ rc, leased };

        auto flux_prog = lease_program("terrain_flux.slang");
        auto water_prog = lease_program("terrain_water.slang");
        auto erode_prog = lease_program("terrain_erode_deposit.slang");
        auto advect_prog = lease_program("terrain_advect_sediment.slang");
        if (!flux_prog || !water_prog || !erode_prog || !advect_prog)
            return false;

        // ---- Upload: height + zeroed water/suspended/flux/wear state ----
        std::vector<float> zeros(n, 0.0f);
        {
            auto cmd = rc.create(CommandListDesc{});
            cmd->open();
            cmd->writeBuffer(b[0], hf.height.data(), n * sizeof(float));
            cmd->writeBuffer(d[0], zeros.data(), n * sizeof(float));
            cmd->writeBuffer(s[0], zeros.data(), n * sizeof(float));
            cmd->writeBuffer(wear, zeros.data(), n * sizeof(float));
            for (int k = 0; k < 4; ++k)
                cmd->writeBuffer(flux[k][0], zeros.data(), n * sizeof(float));
            cmd->close();
            device->executeCommandList(cmd);
            device->waitForIdle();
            rc.destroy(cmd);
        }

        nvrhi::BufferHandle cb_buf = rc.create(
            nvrhi::BufferDesc{}
                .setByteSize(sizeof(TerrainConstantsCB))
                .setIsConstantBuffer(true)
                .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
                .setKeepInitialState(true)
                .setDebugName("terrain_cb"));
        leased.push_back(cb_buf);
        terrain_gpu::upload_constant_buffer(
            rc, device, &cb_init, sizeof(cb_init), cb_buf);

        const int threads = static_cast<int>(n);
        for (int iter = 0; iter < iterations; ++iter) {
            const int cur = iter & 1, nxt = cur ^ 1;

            // Pass 1: pipe flux update (b, d, flux[cur] -> flux[nxt]).
            terrain_gpu::dispatch_shader(
                rc,
                flux_prog,
                { { "height", b[cur] },
                  { "water", d[cur] },
                  { "flux_l", flux[0][cur] },
                  { "flux_r", flux[1][cur] },
                  { "flux_t", flux[2][cur] },
                  { "flux_b", flux[3][cur] } },
                { { "flux_l_out", flux[0][nxt] },
                  { "flux_r_out", flux[1][nxt] },
                  { "flux_t_out", flux[2][nxt] },
                  { "flux_b_out", flux[3][nxt] } },
                cb_buf,
                threads);

            // Pass 2: water surface + flow velocity (d[cur], flux[nxt] ->
            // d[nxt], u, v).
            terrain_gpu::dispatch_shader(
                rc,
                water_prog,
                { { "water", d[cur] },
                  { "flux_l", flux[0][nxt] },
                  { "flux_r", flux[1][nxt] },
                  { "flux_t", flux[2][nxt] },
                  { "flux_b", flux[3][nxt] } },
                { { "water_out", d[nxt] },
                  { "vel_u", vel_u },
                  { "vel_v", vel_v } },
                cb_buf,
                threads);

            // Pass 3: erosion / deposition (b[cur], d[nxt], s[cur], u, v ->
            // b[nxt], s_mid, wear).
            terrain_gpu::dispatch_shader(
                rc,
                erode_prog,
                { { "height", b[cur] },
                  { "water", d[nxt] },
                  { "sediment", s[0] },
                  { "vel_u", vel_u },
                  { "vel_v", vel_v } },
                { { "height_out", b[nxt] },
                  { "sediment_mid", s[1] },
                  { "wear", wear } },
                cb_buf,
                threads);

            // Pass 4: advect suspended sediment (s_mid, u, v -> s[nxt]).
            terrain_gpu::dispatch_shader(
                rc,
                advect_prog,
                { { "sediment", s[1] },
                  { "vel_u", vel_u },
                  { "vel_v", vel_v } },
                { { "sediment_out", s[2] } },
                cb_buf,
                threads);

            std::swap(s[0], s[2]);
        }

        // After the loop: height/water live in the slot written by the LAST
        // iteration (index N & 1), the suspended load in s[0] (swapped each
        // iteration), wear accumulated in place. water/sediment/wear start
        // out EMPTY when the input mesh carried no such quantities (a plain
        // grid from read_usd) — readback_floats memcpy's n floats into the
        // destination, so size them first or it writes past an empty
        // vector's storage.
        hf.water.assign(n, 0.0f);
        hf.sediment.assign(n, 0.0f);
        hf.wear.assign(n, 0.0f);
        terrain_gpu::readback_floats(
            rc, device, b[iterations & 1], hf.height.data(), n);
        terrain_gpu::readback_floats(
            rc, device, d[iterations & 1], hf.water.data(), n);
        terrain_gpu::readback_floats(rc, device, s[0], hf.sediment.data(), n);
        terrain_gpu::readback_floats(rc, device, wear, hf.wear.data(), n);
        hf.has_water = true;
        hf.has_sediment = true;
        hf.has_wear = true;
        return true;
    }

}  // namespace
}  // namespace Ruzino

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(terrain_erode_hydraulic)
{
    b.add_input<Geometry>("Height Field");

    b.add_input<std::string>("Method").default_val(
        "Virtual Pipes (GPU)");  // | Droplets (CPU)
    b.add_input<int>("Iterations").min(1).max(2000).default_val(150);

    // Virtual-pipes parameters (Mei et al. 2007).
    b.add_input<float>("Rain Rate").min(0.0f).max(1.0f).default_val(0.05f);
    b.add_input<float>("Evaporation").min(0.0f).max(0.2f).default_val(0.015f);
    b.add_input<float>("Sediment Capacity")
        .min(0.1f)
        .max(20.0f)
        .default_val(4.0f);
    b.add_input<float>("Erosion Rate").min(0.0f).max(1.0f).default_val(0.35f);
    b.add_input<float>("Deposition Rate").min(0.0f).max(1.0f).default_val(0.3f);
    b.add_input<float>("Pipe Cross Section")
        .min(1.0f)
        .max(100.0f)
        .default_val(20.0f);
    b.add_input<float>("Time Step").min(0.001f).max(0.2f).default_val(0.04f);

    // Droplet parameters (CPU fallback).
    b.add_input<float>("Inertia").min(0.0f).max(0.8f).default_val(0.05f);
    b.add_input<int>("Droplet Count").min(1000).max(500000).default_val(60000);
    b.add_input<int>("Erosion Radius").min(1).max(8).default_val(3);

    b.add_input<int>("Seed").min(0).max(1073741823).default_val(0);

    b.add_output<Geometry>("Height Field");
}

NODE_EXECUTION_FUNCTION(terrain_erode_hydraulic)
{
    if (!params.has_input("Height Field")) {
        spdlog::warn("terrain_erode_hydraulic: no Height Field input");
        return false;
    }
    Geometry input = params.get_input<Geometry>("Height Field");

    Heightfield hf;
    std::string err;
    if (!terrain_carry::heightfield_from_mesh(input, hf, err)) {
        spdlog::warn("terrain_erode_hydraulic: {}", err);
        params.set_output("Height Field", input);
        return true;
    }

    const std::string method = params.get_input<std::string>("Method");
    const double mean_before = hf.mean_height();

    const bool want_droplets = (method == "Droplets (CPU)");
    bool gpu_done = false;
    if (!want_droplets) {
        TerrainConstantsCB cb{};
        const float cell = hf.cell_size();
        cb.res = hf.res;
        cb.dt = params.get_input<float>("Time Step");
        cb.inv_cell = 1.0f / cell;
        cb.max_delta = 0.1f * cell;
        cb.rain_rate = params.get_input<float>("Rain Rate");
        cb.evaporation = params.get_input<float>("Evaporation");
        cb.sediment_capacity = params.get_input<float>("Sediment Capacity");
        cb.erosion_rate = params.get_input<float>("Erosion Rate");
        cb.deposition_rate = params.get_input<float>("Deposition Rate");
        cb.pipe_area =
            params.get_input<float>("Pipe Cross Section") * cell * cell;
        cb.gravity = 9.81f;
        cb.talus = 0.0f;
        cb.strength = 0.0f;

        gpu_done = solve_pipes_gpu(hf, cb, params.get_input<int>("Iterations"));
        if (!gpu_done)
            spdlog::warn(
                "terrain_erode_hydraulic: GPU path unavailable, falling "
                "back to CPU droplets");
    }

    if (!gpu_done) {
        DropletErosionParams dp;
        // The user-facing count is the dose per 256x256 field (the scale the
        // Beyer parameters are tuned at); scale with grid area so small
        // previews don't get 25x the per-cell dosage of a full field.
        const long long raw_count = params.get_input<int>("Droplet Count");
        const double area_scale =
            static_cast<double>(hf.res) * hf.res / (256.0 * 256.0);
        dp.droplet_count = static_cast<int>(
            std::max(500.0, static_cast<double>(raw_count) * area_scale));
        dp.inertia = params.get_input<float>("Inertia");
        dp.sediment_capacity = params.get_input<float>("Sediment Capacity");
        dp.erosion_rate = params.get_input<float>("Erosion Rate");
        dp.deposition_rate = params.get_input<float>("Deposition Rate");
        dp.brush_radius = params.get_input<int>("Erosion Radius");
        dp.seed = static_cast<uint32_t>(params.get_input<int>("Seed"));
        dp.max_lifetime = 30;
        // Beyer's own stabilizer: capacity is proportional to the water
        // volume, so an evaporation of 0.05/star step geometrically decays
        // late-life erosion (0.01 lets droplets etch unbounded slots).
        dp.evaporation = 0.05f;
        dp.gravity = 4.0f;
        hydraulic_droplets(hf, dp);
    }

    const double mean_after = hf.mean_height();
    float wear_max = 0.0f;
    for (float w : hf.wear)
        wear_max = std::max(wear_max, w);
    spdlog::info(
        "[terrain] hydraulic erode: res={} iters={} mean {} -> {} (delta "
        "{:+.4f}) wear_max={:.4f} device={}",
        hf.res,
        params.get_input<int>("Iterations"),
        mean_before,
        mean_after,
        mean_after - mean_before,
        wear_max,
        gpu_done ? "GPU" : "CPU-droplets");

    params.set_output("Height Field", terrain_carry::mesh_from_heightfield(hf));
    return true;
}

NODE_DEF_CLOSE_SCOPE
