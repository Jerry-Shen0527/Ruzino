// Shared GPU plumbing for the terrain erode nodes: the TerrainConstants
// constant-buffer mirror (MUST match TerrainGen/shaders/terrain_common.slangh
// field-for-field — all scalars, so host and slang offsets agree), plus the
// rent/return buffer factories, slang compile helper, and dispatch/readback
// patterns proven by the Wetbrush chain (brush_sim_common.hpp).
//
// All helpers are `inline` so both node .cpp files can include this header.

#pragma once

#include <string>
#include <vector>

#include "GCore/algorithms/gpu_geometry.h"  // is_gpu_alive()
#include "GPUContext/compute_context.hpp"
#include "RHI/ResourceManager/resource_allocator.hpp"
#include "RHI/shaderCompiler.h"
#include "nvrhi/nvrhi.h"
#include "spdlog/spdlog.h"

namespace Ruzino {
namespace terrain_gpu {

    // Mirror of TerrainConstants in terrain_common.slangh. Scalar-only: no vec4
    // fields, so the host/std140 alignment trap (Wetbrush 2026-08-27) cannot
    // apply.
    struct TerrainConstantsCB {
        int res;
        float dt;
        float inv_cell;
        float max_delta;

        float rain_rate;
        float evaporation;
        float sediment_capacity;
        float erosion_rate;

        float deposition_rate;
        float pipe_area;
        float gravity;
        float talus;

        float strength;
        int pad0;
        int pad1;
        int pad2;
    };

    inline std::string terrain_shader_dir()
    {
        return SlangShaderCompiler::get_shader_dir(ShaderDirType::GeomNodes)
                   .string() +
               "/TerrainGen/shaders/";
    }

    inline nvrhi::BufferHandle
    create_field_buffer(ResourceAllocator& rc, size_t n, const char* debug_name)
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

    inline ProgramHandle compile_shader(
        ResourceAllocator& rc,
        const std::string& filename)
    {
        ProgramDesc desc;
        desc.shaderType = nvrhi::ShaderType::Compute;
        desc.set_path(terrain_shader_dir() + filename);
        desc.set_entry_name("main");
        auto prog = rc.create(desc);
        if (!prog->get_error_string().empty()) {
            spdlog::error(
                "[terrain] failed to compile {}: {}",
                filename,
                prog->get_error_string());
            rc.destroy(prog);
            return nullptr;
        }
        return prog;
    }

    inline void dispatch_shader(
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

    inline void upload_constant_buffer(
        ResourceAllocator& rc,
        nvrhi::IDevice* device,
        const void* data,
        size_t size_bytes,
        nvrhi::BufferHandle& out_buf)
    {
        if (!out_buf || out_buf->getDesc().byteSize < size_bytes) {
            if (out_buf)
                rc.destroy(out_buf);
            out_buf = rc.create(
                nvrhi::BufferDesc{}
                    .setByteSize(size_bytes)
                    .setIsConstantBuffer(true)
                    .setInitialState(nvrhi::ResourceStates::ConstantBuffer)
                    .setKeepInitialState(true)
                    .setDebugName("terrain_cb"));
        }
        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        cmd->writeBuffer(out_buf, data, size_bytes);
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        rc.destroy(cmd);
    }

    // Upload a full float array into an existing device buffer.
    inline void upload_floats(
        ResourceAllocator& rc,
        nvrhi::IDevice* device,
        nvrhi::BufferHandle dst,
        const float* data,
        size_t n)
    {
        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        cmd->writeBuffer(dst, data, n * sizeof(float));
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        rc.destroy(cmd);
    }

    // Staging-copy a device buffer back to host memory.
    inline void readback_floats(
        ResourceAllocator& rc,
        nvrhi::IDevice* device,
        nvrhi::BufferHandle src,
        float* dst,
        size_t n)
    {
        auto rb = rc.create(
            nvrhi::BufferDesc{}
                .setByteSize(n * sizeof(float))
                .setCpuAccess(nvrhi::CpuAccessMode::Read)
                .setDebugName("terrain_readback"));
        auto cmd = rc.create(CommandListDesc{});
        cmd->open();
        cmd->copyBuffer(rb, 0, src, 0, n * sizeof(float));
        cmd->close();
        device->executeCommandList(cmd);
        device->waitForIdle();
        void* mapped = device->mapBuffer(rb, nvrhi::CpuAccessMode::Read);
        memcpy(dst, mapped, n * sizeof(float));
        device->unmapBuffer(rb);
        rc.destroy(rb);
        rc.destroy(cmd);
    }

}  // namespace terrain_gpu
}  // namespace Ruzino
