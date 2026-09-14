// terrain_erode_thermal: slope-limited (talus) relaxation for the terrain
// family. GPU path dispatches the same pass as the CPU fallback
// (TerrainGen/src/Erosion.cpp) in a ping-pong loop; every cell's net change
// follows only from the old heights, so the pass is race-free.

#include <cmath>
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

    bool solve_thermal_gpu(
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
        if (res < 3 || hf.height.size() != n)
            return false;

        nvrhi::BufferHandle b[2];
        std::vector<nvrhi::BufferHandle> leased;
        leased.reserve(3);
        const auto lease_field = [&](const char* name) {
            nvrhi::BufferHandle h =
                terrain_gpu::create_field_buffer(rc, n, name);
            leased.push_back(h);
            return h;
        };
        b[0] = lease_field("terrain_th_b0");
        b[1] = lease_field("terrain_th_b1");

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

        ProgramHandle prog =
            terrain_gpu::compile_shader(rc, "terrain_thermal.slang");
        if (!prog)
            return false;
        struct ProgGuard {
            ResourceAllocator& rc;
            ProgramHandle& p;
            ~ProgGuard()
            {
                if (p)
                    rc.destroy(p);
            }
        } prog_guard{ rc, prog };

        {
            auto cmd = rc.create(CommandListDesc{});
            cmd->open();
            cmd->writeBuffer(b[0], hf.height.data(), n * sizeof(float));
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
                .setDebugName("terrain_th_cb"));
        leased.push_back(cb_buf);
        terrain_gpu::upload_constant_buffer(
            rc, device, &cb_init, sizeof(cb_init), cb_buf);

        const int threads = static_cast<int>(n);
        for (int iter = 0; iter < iterations; ++iter) {
            const int cur = iter & 1, nxt = cur ^ 1;
            terrain_gpu::dispatch_shader(
                rc,
                prog,
                { { "height", b[cur] } },
                { { "height_out", b[nxt] } },
                cb_buf,
                threads);
        }

        terrain_gpu::readback_floats(
            rc, device, b[iterations & 1], hf.height.data(), n);
        return true;
    }

}  // namespace
}  // namespace Ruzino

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(terrain_erode_thermal)
{
    b.add_input<Geometry>("Height Field");
    b.add_input<int>("Iterations").min(1).max(500).default_val(30);
    b.add_input<float>("Talus Angle")
        .min(0.0f)
        .max(60.0f)
        .default_val(35.0f);  // degrees
    b.add_input<float>("Strength").min(0.0f).max(1.0f).default_val(0.5f);

    b.add_output<Geometry>("Height Field");
}

NODE_EXECUTION_FUNCTION(terrain_erode_thermal)
{
    if (!params.has_input("Height Field")) {
        spdlog::warn("terrain_erode_thermal: no Height Field input");
        return false;
    }
    Geometry input = params.get_input<Geometry>("Height Field");

    Heightfield hf;
    std::string err;
    if (!terrain_carry::heightfield_from_mesh(input, hf, err)) {
        spdlog::warn("terrain_erode_thermal: {}", err);
        params.set_output("Height Field", input);
        return true;
    }

    ThermalErosionParams tp;
    tp.iterations = params.get_input<int>("Iterations");
    tp.talus_angle_deg = params.get_input<float>("Talus Angle");
    tp.strength = params.get_input<float>("Strength");

    const double mean_before = hf.mean_height();

    TerrainConstantsCB cb{};
    const float cell = hf.cell_size();
    cb.res = hf.res;
    cb.dt = 0.0f;
    cb.inv_cell = 1.0f / cell;
    cb.max_delta = 0.0f;
    cb.rain_rate = 0.0f;
    cb.evaporation = 0.0f;
    cb.sediment_capacity = 0.0f;
    cb.erosion_rate = 0.0f;
    cb.deposition_rate = 0.0f;
    cb.pipe_area = 0.0f;
    cb.gravity = 0.0f;
    cb.talus = std::tan(tp.talus_angle_deg * 3.14159265358979f / 180.0f) * cell;
    cb.strength = tp.strength;

    // solve_thermal_gpu lazily brings the device up and falls back to the
    // CPU implementation when unavailable.
    const bool gpu_done = solve_thermal_gpu(hf, cb, tp.iterations);
    if (!gpu_done) {
        spdlog::warn("terrain_erode_thermal: GPU path unavailable, using CPU");
        thermal_erosion(hf, tp);
    }

    spdlog::info(
        "[terrain] thermal erode: res={} iters={} mean {} -> {} device={}",
        hf.res,
        tp.iterations,
        mean_before,
        hf.mean_height(),
        gpu_done ? "GPU" : "CPU");

    params.set_output("Height Field", terrain_carry::mesh_from_heightfield(hf));
    return true;
}

NODE_DEF_CLOSE_SCOPE
