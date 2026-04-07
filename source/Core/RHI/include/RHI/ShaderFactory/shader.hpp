#pragma once
#include <nvrhi/nvrhi.h>

#include "RHI/api.h"
#include "RHI/internal/resources.hpp"
#include "RHI/rhi.hpp"
#include "shader_reflection.hpp"

namespace Ruzino {
class ResourceAllocator;
}

RUZINO_NAMESPACE_OPEN_SCOPE
class RHI_API ShaderFactory {
   public:
    ShaderFactory() : device(RHI::get_device()), resource_allocator(nullptr)
    {
    }

    ShaderFactory(ResourceAllocator* resource_allocator)
        : device(RHI::get_device()),
          resource_allocator(resource_allocator)
    {
    }

    ShaderHandle compile_shader(
        const std::string& entryName,
        nvrhi::ShaderType shader_type,
        const std::string& shader_path,
        ShaderReflectionInfo& reflection_info,
        std::string& error_string,
        const std::vector<ShaderMacro>& macro_defines = {},
        const std::string& source_code = {}) const;

    ProgramHandle compile_cpu_executable(
        const std::string& entryName,
        nvrhi::ShaderType shader_type,
        const std::string& shader_path,
        ShaderReflectionInfo& reflection_info,
        std::string& error_string,
        const std::vector<ShaderMacro>& macro_defines = {},
        const std::string& source_code = {});

    ProgramHandle createProgram(const ProgramDesc& desc) const;

    static void set_search_path(const std::string& string)
    {
        shader_search_path = string;
    }

    void add_search_path(const std::string& string)
    {
        search_paths.push_back(string);
    }

    static void set_cache_enabled(bool enabled)
    {
        cache_enabled = enabled;
    }

    static std::string get_cache_directory()
    {
        return "./shader_cache";
    }

   private:
    std::vector<std::string> search_paths;

    // Cache management
    static bool cache_enabled;
    bool try_load_from_cache(
        const ProgramDesc& desc,
        Slang::ComPtr<ISlangBlob>& blob,
        ShaderReflectionInfo& reflection_info,
        SlangCompileTarget target) const;
    void save_to_cache(
        const ProgramDesc& desc,
        const Slang::ComPtr<ISlangBlob>& blob,
        const ShaderReflectionInfo& reflection_info,
        SlangCompileTarget target) const;
    std::string get_cache_filename(
        const ProgramDesc& desc,
        SlangCompileTarget target) const;

    void SlangCompile(
        const std::vector<std::string>& paths,
        const std::vector<std::string>& sourceCodes,
        const char* entryPoint,
        nvrhi::ShaderType shaderType,
        const char* profile,
        const std::vector<ShaderMacro>& defines,
        ShaderReflectionInfo& shader_reflection,
        Slang::ComPtr<ISlangBlob>& ppResultBlob,
        Slang::ComPtr<ISlangSharedLibrary>& ppSharedLirary,
        std::string& error_string,
        SlangCompileTarget target,
        Slang::ComPtr<slang::IComponentType>* linkedProgram1 = nullptr,
        bool nvapi_support = false

    ) const;

    static void populate_vk_options(
        std::vector<slang::CompilerOptionEntry>& vk_compiler_options);
    static void populate_dxc_options(
        std::vector<slang::CompilerOptionEntry>& dxc_compiler_options,
        const char* nvapi_include_arg = nullptr);
    void modify_vulkan_binding_shift(nvrhi::BindingLayoutItem& item) const;
    ShaderReflectionInfo shader_reflect(
        slang::IComponentType* component,
        nvrhi::ShaderType shader_type,
        slang::IBlob** diagnostic) const;

    static constexpr int SRV_OFFSET = 0;
    static constexpr int SAMPLER_OFFSET = 128;
    static constexpr int CONSTANT_BUFFER_OFFSET = 256;
    static constexpr int UAV_OFFSET = 384;

    static std::string shader_search_path;
    nvrhi::IDevice* device;
    ResourceAllocator* resource_allocator;
    friend struct ProgramDesc;
};

RUZINO_NAMESPACE_CLOSE_SCOPE
