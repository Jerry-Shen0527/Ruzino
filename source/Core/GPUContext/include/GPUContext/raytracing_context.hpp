#pragma once

#include "context.hpp"

RUZINO_NAMESPACE_OPEN_SCOPE

struct GPUCONTEXT_API RaytracingState { };

class GPUCONTEXT_API RaytracingContext : public GPUContext {
   public:
    explicit RaytracingContext(ResourceAllocator& r, ProgramVars& vars);
    ~RaytracingContext() override;
    using ProgramVarHandle = std::shared_ptr<ProgramVars>;

    void begin() override;
    void finish() override;

    void trace_rays(
        const RaytracingState& state,
        const ProgramVars& program_vars,
        uint32_t width,
        uint32_t height = 1,
        uint32_t depth = 1) const;

    void announce_raygeneration(
        const std::string& name,
        ProgramVarHandle handle = nullptr);
    void announce_hitgroup(
        const std::string& closesthit,
        const std::string& anyhit = "",
        const std::string& intercestion = "",
        unsigned position = 0,
        ProgramVarHandle handle_hg = nullptr);

    void announce_callable(
        const std::string& name,
        unsigned position = 0,
        ProgramVarHandle handle = nullptr);

    void announce_miss(
        const std::string& name,
        unsigned position = 0,
        ProgramVarHandle handle = nullptr);

    void finish_announcing_shader_names();

   private:
    // Shader names
    std::string raygeneration_name;
    std::vector<std::tuple<std::string, std::string, std::string>>
        hitgroup_names;
    std::vector<std::string> callable_names;
    std::vector<std::string> miss_names;

    // Solid programs
    ProgramVarHandle ray_generation_program;
    std::vector<ProgramVarHandle> hitgroup_programs;
    std::vector<ProgramVarHandle> callable_programs;
    std::vector<ProgramVarHandle> miss_programs;

    // Solid shaders
    nvrhi::ShaderHandle ray_generation_shader;
    std::vector<std::tuple<ShaderHandle, ShaderHandle, ShaderHandle>>
        hit_group_shaders;
    std::vector<nvrhi::ShaderHandle> callable_shaders;
    std::vector<nvrhi::ShaderHandle> miss_shaders;

    // Pipeline
    IProgram* program;
    nvrhi::rt::ShaderTableHandle sbt;
    nvrhi::rt::PipelineHandle raytracing_pipeline;
};

RUZINO_NAMESPACE_CLOSE_SCOPE
