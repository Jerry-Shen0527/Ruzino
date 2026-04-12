#include "RHI/ShaderFactory/shader.hpp"

#include <spdlog/spdlog.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

#include "RHI/ResourceManager/resource_allocator.hpp"
#include "RHI/internal/resources.hpp"
#include "shaderCompiler.h"
#include "slang-com-ptr.h"
#include "slang.h"

RUZINO_NAMESPACE_OPEN_SCOPE

// Custom Blob implementation to hold shader binary data loaded from cache
class CustomBlob : public ISlangBlob {
   private:
    std::vector<char> data;
    std::atomic<uint32_t> refCount;

   public:
    explicit CustomBlob(std::vector<char>&& buffer)
        : data(std::move(buffer)),
          refCount(1)
    {
    }

    // ISlangUnknown interface
    virtual SLANG_NO_THROW SlangResult SLANG_MCALL
    queryInterface(SlangUUID const& uuid, void** outObject) override
    {
        if (uuid == ISlangUnknown::getTypeGuid() ||
            uuid == ISlangBlob::getTypeGuid()) {
            *outObject = static_cast<ISlangBlob*>(this);
            addRef();
            return SLANG_OK;
        }
        return SLANG_E_NO_INTERFACE;
    }

    virtual SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override
    {
        return ++refCount;
    }

    virtual SLANG_NO_THROW uint32_t SLANG_MCALL release() override
    {
        uint32_t newCount = --refCount;
        if (newCount == 0) {
            delete this;
        }
        return newCount;
    }

    // ISlangBlob interface
    virtual SLANG_NO_THROW void const* SLANG_MCALL getBufferPointer() override
    {
        return data.data();
    }

    virtual SLANG_NO_THROW size_t SLANG_MCALL getBufferSize() override
    {
        return data.size();
    }
};

std::string ShaderFactory::shader_search_path = "";
bool ShaderFactory::cache_enabled = true;

ProgramDesc Program::get_desc() const
{
    return desc;
}

nvrhi::ShaderDesc Program::get_shader_desc() const
{
    ShaderDesc desc;

    desc.shaderType = this->desc.shaderType;
    desc.entryName = this->desc.entry_name;
    desc.debugName =
        std::to_string(reinterpret_cast<long long>(getBufferPointer()));
    return desc;
}

void const* Program::getBufferPointer() const
{
    if (!blob) return nullptr;
    return blob->getBufferPointer();
}

size_t Program::getBufferSize() const
{
    if (!blob) return 0;
    return blob->getBufferSize();
}

const ShaderReflectionInfo& Program::get_reflection_info() const
{
    return reflection_info;
}

ProgramDesc& ProgramDesc::set_path(const std::string& path)
{
    this->paths.clear();
    this->paths.push_back(path);
#ifdef _DEBUG
    update_last_write_time(path);
#endif
    return *this;
}

ProgramDesc& ProgramDesc::add_path(const std::string& path)
{
    this->paths.push_back(path);
#ifdef _DEBUG
    update_last_write_time(path);
#endif
    return *this;
}

ProgramDesc& ProgramDesc::set_shader_type(nvrhi::ShaderType shaderType)
{
    this->shaderType = shaderType;
    return *this;
}

ProgramDesc& ProgramDesc::set_entry_name(const std::string& entry_name)
{
    this->entry_name = entry_name;
#ifdef _DEBUG
    // update_last_write_time(path);
#endif

    return *this;
}
namespace fs = std::filesystem;

bool ProgramDesc::check_shader_updated() const
{
    for (const auto& path : paths) {
        auto full_path =
            std::filesystem::path(ShaderFactory::shader_search_path) / path;
        if (fs::exists(full_path)) {
            auto possibly_newer_lastWriteTime = fs::last_write_time(full_path);
            auto current_time =
                possibly_newer_lastWriteTime.time_since_epoch().count();

            if (lastWriteTime == 0) {
                return false;
            }

            if (current_time > lastWriteTime) {
                return true;
            }
        }
    }
    return false;
}
void ProgramDesc::update_last_write_time(const std::string& path)
{
    auto full_path =
        std::filesystem::path(ShaderFactory::shader_search_path) / path;
    if (fs::exists(full_path)) {
        auto possibly_newer_lastWriteTime = fs::last_write_time(full_path);
        if (possibly_newer_lastWriteTime.time_since_epoch().count() >
            lastWriteTime) {
            lastWriteTime =
                possibly_newer_lastWriteTime.time_since_epoch().count();
        }
    }
    else {
        lastWriteTime = 0;
    }
}

size_t ProgramDesc::calculate_hash() const
{
    size_t hash = 0;

    // Hash shader type
    hash ^= std::hash<int>{}(static_cast<int>(shaderType)) + 0x9e3779b9 +
            (hash << 6) + (hash >> 2);

    // Hash entry name
    hash ^= std::hash<std::string>{}(entry_name) + 0x9e3779b9 + (hash << 6) +
            (hash >> 2);

    // Hash all paths (sorted for consistency)
    if (!paths.empty()) {
        std::vector<std::string> sorted_paths = paths;
        std::sort(sorted_paths.begin(), sorted_paths.end());
        for (const auto& path : sorted_paths) {
            hash ^= std::hash<std::string>{}(path) + 0x9e3779b9 + (hash << 6) +
                    (hash >> 2);
        }
    }

    // Hash all source codes (sorted for consistency)
    if (!source_code.empty()) {
        std::vector<std::string> sorted_codes = source_code;
        std::sort(sorted_codes.begin(), sorted_codes.end());
        for (const auto& code : sorted_codes) {
            hash ^= std::hash<std::string>{}(code) + 0x9e3779b9 + (hash << 6) +
                    (hash >> 2);
        }
    }

    // Hash macros (sorted by name for consistency)
    if (!macros.empty()) {
        std::vector<ShaderMacro> sorted_macros = macros;
        std::sort(
            sorted_macros.begin(),
            sorted_macros.end(),
            [](const ShaderMacro& a, const ShaderMacro& b) {
                return a.name < b.name;
            });
        for (const auto& macro : sorted_macros) {
            hash ^= std::hash<std::string>{}(macro.name) + 0x9e3779b9 +
                    (hash << 6) + (hash >> 2);
            hash ^= std::hash<std::string>{}(macro.definition) + 0x9e3779b9 +
                    (hash << 6) + (hash >> 2);
        }
    }

    return hash;
}

std::string ProgramDesc::get_profile() const
{
    switch (shaderType) {
        case nvrhi::ShaderType::None: break;
        case nvrhi::ShaderType::Compute: return "cs_6_6";
        case nvrhi::ShaderType::Vertex: return "vs_6_6";
        case nvrhi::ShaderType::Hull: return "hs_6_6";
        case nvrhi::ShaderType::Domain: return "ds_6_6";
        case nvrhi::ShaderType::Geometry: return "gs_6_6";
        case nvrhi::ShaderType::Pixel: return "ps_6_6";
        case nvrhi::ShaderType::Amplification: return "as_6_6";
        case nvrhi::ShaderType::Mesh: return "ms_6_6";
        case nvrhi::ShaderType::AllGraphics: return "lib_6_6";
        case nvrhi::ShaderType::RayGeneration: return "rg_6_6";
        case nvrhi::ShaderType::AnyHit: return "ah_6_6";
        case nvrhi::ShaderType::ClosestHit: return "ch_6_6";
        case nvrhi::ShaderType::Miss: return "ms_6_6";
        case nvrhi::ShaderType::Intersection: return "is_6_6";
        case nvrhi::ShaderType::Callable: return "lib_6_6";
        case nvrhi::ShaderType::AllRayTracing: return "lib_6_6";
        case nvrhi::ShaderType::All: return "lib_6_6";
    }

    // Default return value for cases not handled explicitly
    return "lib_6_6";
}
class GlobalSessionPool {
   private:
    std::mutex sessionsMutex;
    std::condition_variable sessionAvailable;
    size_t maxSessions;
    std::unordered_map<std::thread::id, Slang::ComPtr<slang::IGlobalSession>>
        activeThreadSessions;
    std::queue<Slang::ComPtr<slang::IGlobalSession>> cachedSessions;
    size_t totalSessionCount = 0;  // Tracks both active and cached sessions

    Slang::ComPtr<slang::IGlobalSession> createNewSession()
    {
        Slang::ComPtr<slang::IGlobalSession> session;
        slang::createGlobalSession(session.writeRef());
        SlangShaderCompiler::addHLSLPrelude(session);
        SlangShaderCompiler::addCPPPrelude(session);
        return session;
    }

   public:
    GlobalSessionPool() : maxSessions(16)
    {
    }

    Slang::ComPtr<slang::IGlobalSession> getSession()
    {
        std::thread::id threadId = std::this_thread::get_id();
        std::unique_lock<std::mutex> lock(sessionsMutex);

        // Check if this thread already has a session
        auto it = activeThreadSessions.find(threadId);
        if (it != activeThreadSessions.end()) {
            return it->second;
        }

        // Wait until we can create or reuse a session
        while (totalSessionCount >= maxSessions && cachedSessions.empty()) {
            sessionAvailable.wait(lock);
        }

        Slang::ComPtr<slang::IGlobalSession> session;

        // Reuse a cached session if available
        if (!cachedSessions.empty()) {
            session = cachedSessions.front();
            cachedSessions.pop();
        }
        else {
            // Create a new session
            session = createNewSession();
            totalSessionCount++;
        }

        // Associate session with this thread
        activeThreadSessions[threadId] = session;

        return session;
    }

    void releaseSession(std::thread::id threadId)
    {
        std::lock_guard<std::mutex> lock(sessionsMutex);

        auto it = activeThreadSessions.find(threadId);
        if (it != activeThreadSessions.end()) {
            // Move session to cache
            cachedSessions.push(it->second);
            activeThreadSessions.erase(it);

            // Notify waiting threads
            sessionAvailable.notify_one();
        }
    }

    ~GlobalSessionPool()
    {
        // Clean up all sessions
        std::lock_guard<std::mutex> lock(sessionsMutex);
        activeThreadSessions.clear();
        while (!cachedSessions.empty()) {
            cachedSessions.pop();
        }
        totalSessionCount = 0;
    }
};

GlobalSessionPool globalSessionPool;

Slang::ComPtr<slang::IGlobalSession> getGlobalSession()
{
    return globalSessionPool.getSession();
}

void releaseGlobalSession()
{
    globalSessionPool.releaseSession(std::this_thread::get_id());
}

static nvrhi::ResourceType convertBindingTypeToResourceType(
    slang::BindingType bindingType,
    SlangResourceShape resource_shape)
{
    using namespace nvrhi;
    using namespace slang;

    auto ret = ResourceType::None;
    switch (bindingType) {
        case BindingType::Sampler: ret = ResourceType::Sampler; break;
        case BindingType::Texture:
        case BindingType::CombinedTextureSampler:
        case BindingType::InputRenderTarget:
            ret = ResourceType::Texture_SRV;
            break;
        case BindingType::MutableTexture:
            ret = ResourceType::Texture_UAV;
            break;
        case BindingType::TypedBuffer:
        case BindingType::MutableTypedBuffer:
            ret = ResourceType::TypedBuffer_SRV;
            break;
        case BindingType::RawBuffer: ret = ResourceType::RawBuffer_SRV; break;
        case BindingType::MutableRawBuffer:
            ret = ResourceType::RawBuffer_UAV;
            break;
        case BindingType::ConstantBuffer:
        case BindingType::ParameterBlock:
            ret = ResourceType::ConstantBuffer;
            break;
        case BindingType::RayTracingAccelerationStructure:
            ret = ResourceType::RayTracingAccelStruct;
            break;
        case BindingType::PushConstant:
            ret = ResourceType::PushConstants;
            break;
    }

    if (resource_shape == SLANG_STRUCTURED_BUFFER) {
        if (ret == ResourceType::RawBuffer_SRV) {
            ret = ResourceType::StructuredBuffer_SRV;
        }
        else if (ret == ResourceType::RawBuffer_UAV) {
            ret = ResourceType::StructuredBuffer_UAV;
        }
    }

    return ret;
}

void ShaderFactory::modify_vulkan_binding_shift(
    nvrhi::BindingLayoutItem& item) const
{
    switch (item.type) {
        case nvrhi::ResourceType::None: break;
        case nvrhi::ResourceType::Texture_SRV: item.slot -= SRV_OFFSET; break;
        case nvrhi::ResourceType::Texture_UAV: item.slot -= UAV_OFFSET; break;
        case nvrhi::ResourceType::TypedBuffer_SRV:
            item.slot -= SRV_OFFSET;
            break;
        case nvrhi::ResourceType::TypedBuffer_UAV:
            item.slot -= UAV_OFFSET;
            break;
        case nvrhi::ResourceType::StructuredBuffer_SRV:
            item.slot -= SRV_OFFSET;
            break;
        case nvrhi::ResourceType::StructuredBuffer_UAV:
            item.slot -= UAV_OFFSET;
            break;
        case nvrhi::ResourceType::RawBuffer_SRV: item.slot -= SRV_OFFSET; break;
        case nvrhi::ResourceType::RawBuffer_UAV: item.slot -= UAV_OFFSET; break;
        case nvrhi::ResourceType::ConstantBuffer:
            item.slot -= CONSTANT_BUFFER_OFFSET;
            break;
        case nvrhi::ResourceType::VolatileConstantBuffer:
            item.slot -= CONSTANT_BUFFER_OFFSET;
            break;
        case nvrhi::ResourceType::Sampler: item.slot -= SAMPLER_OFFSET; break;
        case nvrhi::ResourceType::RayTracingAccelStruct:
            item.slot -= SRV_OFFSET;
            break;
    }
}
ShaderReflectionInfo ShaderFactory::shader_reflect(
    slang::IComponentType* component,
    nvrhi::ShaderType shader_type,
    slang::IBlob** diagnostic) const
{
    ShaderReflectionInfo ret;
    auto& binding_locations = ret.binding_locations;

    slang::ShaderReflection* programReflection =
        component->getLayout(0, diagnostic);

    // slang::EntryPointReflection* entryPoint =
    //     programReflection->findEntryPointByName(entryPointName);
    auto parameterCount = programReflection->getParameterCount();
    auto g_layout = programReflection->getGlobalParamsTypeLayout();
    auto binding_set_count = g_layout->getDescriptorSetCount();
    // auto parameterCount = entryPoint->getParameterCount();
    nvrhi::BindingLayoutDescVector& layout_vector = ret.binding_spaces;

    std::vector<unsigned> indices;

    for (int pp = 0; pp < parameterCount; ++pp) {
        slang::VariableLayoutReflection* parameter =
            programReflection->getParameterByIndex(pp);
        slang::TypeLayoutReflection* typeLayout = parameter->getTypeLayout();
        slang::TypeReflection* type_reflection = parameter->getType();
        SlangResourceShape resource_shape = type_reflection->getResourceShape();
        auto d_set_count = typeLayout->getDescriptorSetCount();

        auto element_count = typeLayout->getElementCount();
        bool is_array = element_count > 0;

        slang::ParameterCategory category = parameter->getCategory();
        std::string name = parameter->getName();

        auto index = parameter->getBindingIndex();
        auto space = parameter->getBindingSpace() +
                     parameter->getOffset(
                         SLANG_PARAMETER_CATEGORY_SUB_ELEMENT_REGISTER_SPACE);

        auto bindingRangeCount = typeLayout->getBindingRangeCount();
        assert(bindingRangeCount == 1);
        slang::BindingType type = typeLayout->getBindingRangeType(0);

        if (RHI::get_backend() == nvrhi::GraphicsAPI::VULKAN) {
            if (layout_vector.size() < space + 1) {
                layout_vector.resize(space + 1);
                indices.resize(space + 1, 0);
            }
        }

        // Handle both arrays and single elements
        nvrhi::BindingLayoutItem item;
        item.type = convertBindingTypeToResourceType(type, resource_shape);
        item.slot = index;

        // Set the size field: for arrays use element_count, for single elements
        // use 1
        if (is_array && element_count > 0) {
            item.size = static_cast<uint16_t>(element_count);
        }
        else {
            item.size = 1;
        }

        if (RHI::get_backend() == nvrhi::GraphicsAPI::VULKAN) {
            modify_vulkan_binding_shift(item);
        }

        if (layout_vector.size() < space + 1) {
            layout_vector.resize(space + 1);
            indices.resize(space + 1, 0);
        }

        // Store the binding location with the base name
        binding_locations[name] =
            std::make_tuple(static_cast<unsigned int>(space), indices[space]++);
        layout_vector[space].addItem(item);

        layout_vector[space].visibility = shader_type;
        layout_vector[space].registerSpaceIsDescriptorSet = true;
        layout_vector[space].registerSpace = space;
    }

    return ret;
}

// Function to convert ShaderType to SlangStage
SlangStage ConvertShaderTypeToSlangStage(nvrhi::ShaderType shaderType)
{
    using namespace nvrhi;
    switch (shaderType) {
        case ShaderType::Vertex: return SLANG_STAGE_VERTEX;
        case ShaderType::Hull: return SLANG_STAGE_HULL;
        case ShaderType::Domain: return SLANG_STAGE_DOMAIN;
        case ShaderType::Geometry: return SLANG_STAGE_GEOMETRY;
        case ShaderType::Pixel:
            return SLANG_STAGE_FRAGMENT;  // alias for SLANG_STAGE_PIXEL
        case ShaderType::Amplification: return SLANG_STAGE_AMPLIFICATION;
        case ShaderType::Mesh: return SLANG_STAGE_MESH;
        case ShaderType::Compute: return SLANG_STAGE_COMPUTE;
        case ShaderType::RayGeneration: return SLANG_STAGE_RAY_GENERATION;
        case ShaderType::AnyHit: return SLANG_STAGE_ANY_HIT;
        case ShaderType::ClosestHit: return SLANG_STAGE_CLOSEST_HIT;
        case ShaderType::Miss: return SLANG_STAGE_MISS;
        case ShaderType::Intersection: return SLANG_STAGE_INTERSECTION;
        case ShaderType::Callable: return SLANG_STAGE_CALLABLE;
        default: return SLANG_STAGE_NONE;
    }
}

nvrhi::ShaderHandle ShaderFactory::compile_shader(
    const std::string& entryName,
    nvrhi::ShaderType shader_type,
    const std::string& shader_path,
    ShaderReflectionInfo& reflection_info,
    std::string& error_string,
    const std::vector<ShaderMacro>& macro_defines,
    const std::string& source_code) const
{
    ProgramDesc program_desc;
    program_desc.set_entry_name(entryName);

    if (shader_path != "") {
        program_desc.set_path(shader_path);
    }
    for (const auto& macro_define : macro_defines) {
        program_desc.define(macro_define.name, macro_define.definition);
    }
    program_desc.shaderType = shader_type;
    if (!source_code.empty()) {
        program_desc.source_code = { source_code };
    }

    ProgramHandle shader_compiled;

    if (resource_allocator) {
        shader_compiled = resource_allocator->create(program_desc);
    }
    else {
        shader_compiled = createProgram(program_desc);
    }

    if (!shader_compiled->get_error_string().empty()) {
        error_string = shader_compiled->get_error_string();
        shader_compiled = nullptr;
        return nullptr;
    }

    nvrhi::ShaderDesc desc = shader_compiled->get_shader_desc();

    reflection_info = shader_compiled->get_reflection_info();

    ShaderHandle shader;
    if (resource_allocator) {
        shader = resource_allocator->create(
            desc,
            shader_compiled->getBufferPointer(),
            shader_compiled->getBufferSize());
    }
    else {
        shader = device->createShader(
            desc,
            shader_compiled->getBufferPointer(),
            shader_compiled->getBufferSize());
    }

    if (resource_allocator) {
        resource_allocator->destroy(shader_compiled);
    }
    else {
        shader_compiled = nullptr;
    }

    return shader;
}

ProgramHandle ShaderFactory::compile_cpu_executable(
    const std::string& entryName,
    nvrhi::ShaderType shader_type,
    const std::string& shader_path,
    ShaderReflectionInfo& reflection_info,
    std::string& error_string,
    const std::vector<ShaderMacro>& macro_defines,
    const std::string& source_code)
{
    ProgramDesc desc;

    if (shader_path != "") {
        desc.set_path(shader_path);
    }
    desc.set_entry_name(entryName);

    for (const auto& macro_define : macro_defines) {
        desc.define(macro_define.name, macro_define.definition);
    }
    desc.shaderType = shader_type;
    desc.source_code = { source_code };

    ProgramHandle program_handle;
    program_handle = ProgramHandle::Create(new Program());

    program_handle->desc = desc;

    SlangCompileTarget target = SLANG_SHADER_HOST_CALLABLE;

    SlangCompile(
        desc.paths,
        desc.source_code,
        desc.entry_name.c_str(),
        desc.shaderType,
        desc.get_profile().c_str(),
        desc.macros,
        program_handle->reflection_info,
        program_handle->blob,
        program_handle->library,
        program_handle->error_string,
        target);

    reflection_info = program_handle->get_reflection_info();
    error_string = program_handle->get_error_string();

    return program_handle;
}

void ShaderFactory::populate_vk_options(
    std::vector<slang::CompilerOptionEntry>& vk_compiler_options)
{
    vk_compiler_options.push_back(
        { slang::CompilerOptionName::VulkanUseEntryPointName,
          slang::CompilerOptionValue{ slang::CompilerOptionValueKind::Int,
                                      1 } });
    vk_compiler_options.push_back(
        { slang::CompilerOptionName::VulkanBindShiftAll,
          slang::CompilerOptionValue{
              slang::CompilerOptionValueKind::Int, 2, SRV_OFFSET } });
    vk_compiler_options.push_back(
        { slang::CompilerOptionName::VulkanBindShiftAll,
          slang::CompilerOptionValue{
              slang::CompilerOptionValueKind::Int, 1, SAMPLER_OFFSET } });
    vk_compiler_options.push_back(
        { slang::CompilerOptionName::VulkanBindShiftAll,
          slang::CompilerOptionValue{ slang::CompilerOptionValueKind::Int,
                                      3,
                                      CONSTANT_BUFFER_OFFSET } });
    vk_compiler_options.push_back(
        { slang::CompilerOptionName::VulkanBindShiftAll,
          slang::CompilerOptionValue{
              slang::CompilerOptionValueKind::Int, 0, UAV_OFFSET } });
}

void ShaderFactory::populate_dxc_options(
    std::vector<slang::CompilerOptionEntry>& dxc_compiler_options,
    const char* nvapi_include_arg)
{
    // Use DXC as the downstream compiler for DXIL generation
    dxc_compiler_options.push_back(
        { slang::CompilerOptionName::DefaultDownstreamCompiler,
          slang::CompilerOptionValue{ slang::CompilerOptionValueKind::Int,
                                      SLANG_PASS_THROUGH_DXC } });

    // Enable optimizations in DXC
    dxc_compiler_options.push_back(
        { slang::CompilerOptionName::Optimization,
          slang::CompilerOptionValue{ slang::CompilerOptionValueKind::Int,
                                      3 } });  // O3 optimization level

    // Enable debug info for shader profiling and debugging
    // This adds the -Zi flag to DXC, providing full debug info including
    // linetable
    dxc_compiler_options.push_back(
        { slang::CompilerOptionName::DebugInformation,
          slang::CompilerOptionValue{
              slang::CompilerOptionValueKind::Int,
              2 } });  // Full debug info (equivalent to -Zi)

    // Use IEEE strict mode for better precision
    dxc_compiler_options.push_back(
        { slang::CompilerOptionName::FloatingPointMode,
          slang::CompilerOptionValue{ slang::CompilerOptionValueKind::Int,
                                      1 } });  // IEEE strict

    // Add NVAPI include path for DXC if provided
    if (nvapi_include_arg != nullptr) {
        // Use DownstreamArgs to pass -I flag to DXC
        // stringValue0: downstream compiler name ("dxc" or empty for default)
        // stringValue1: argument list
        dxc_compiler_options.push_back(
            { slang::CompilerOptionName::DownstreamArgs,
              slang::CompilerOptionValue{
                  slang::CompilerOptionValueKind::String,
                  0,
                  0,
                  "",  // Empty string for default compiler (DXC)
                  nvapi_include_arg } });
    }
}

#define CHECK_REPORTED_ERROR()                                           \
    if (SLANG_FAILED(result)) {                                          \
        if (diagnostics) {                                               \
            error_string = (const char*)diagnostics->getBufferPointer(); \
        }                                                                \
        return;                                                          \
    }                                                                    \
    else if (diagnostics) {                                              \
        spdlog::warn((const char*)diagnostics->getBufferPointer());      \
    }

void ShaderFactory::SlangCompile(
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
    Slang::ComPtr<slang::IComponentType>* linkedProgram1,
    bool nvapi_support) const
{
    auto stage = ConvertShaderTypeToSlangStage(shaderType);

    auto global_session = getGlobalSession();

    auto profile_id = global_session->findProfile(profile);

    std::vector<slang::CompilerOptionEntry> vk_compiler_options;
    std::vector<slang::CompilerOptionEntry> dxc_compiler_options;

    // Calculate NVAPI path if needed
    std::string nvapi_include_path;
    std::string
        nvapi_include_arg;  // Must be kept alive for the entire compilation
    if (nvapi_support && target == SLANG_DXIL) {
        spdlog::info(
            "Looking for NVAPI headers, shader_search_path = {}",
            shader_search_path);

        // Try multiple potential locations for nvapi headers
        std::vector<std::filesystem::path> potential_nvapi_paths = {
            // Runtime renderer resources
            // shader_search_path is: .../renderer/nodes/shaders/
            // we need: .../renderer/resources/nvapi
            std::filesystem::path(shader_search_path)
                    .parent_path()
                    .parent_path()
                    .parent_path() /
                "resources" / "nvapi",
            // External folder
            // shader_search_path is: .../source/Runtime/renderer/nodes/shaders/
            // we need: .../external/nvapi
            std::filesystem::path(shader_search_path)
                    .parent_path()
                    .parent_path()
                    .parent_path()
                    .parent_path()
                    .parent_path()
                    .parent_path() /
                "external" / "nvapi"
        };

        for (const auto& nvapi_path : potential_nvapi_paths) {
            spdlog::info("Checking nvapi path: {}", nvapi_path.string());
            if (std::filesystem::exists(nvapi_path / "nvHLSLExtns.h")) {
                nvapi_include_path = nvapi_path.generic_string();
                nvapi_include_arg = "-I" + nvapi_include_path;
                spdlog::info("Found nvapi at {} for DXC", nvapi_path.string());
                break;
            }
        }

        if (nvapi_include_path.empty()) {
            spdlog::warn(
                "NVAPI support requested but nvHLSLExtns.h not found in "
                "expected locations");
        }
    }

    if (target == SLANG_SPIRV) {
        populate_vk_options(vk_compiler_options);
    }
    else if (target == SLANG_DXIL) {
        populate_dxc_options(
            dxc_compiler_options,
            nvapi_include_arg.empty() ? nullptr : nvapi_include_arg.c_str());
    }

    slang::TargetDesc desc;
    desc.format = target;
    desc.profile = profile_id;
    if (target == SLANG_SPIRV)
    {
        desc.flags = SLANG_TARGET_FLAG_GENERATE_WHOLE_PROGRAM |
                     SLANG_TARGET_FLAG_GENERATE_SPIRV_DIRECTLY;
        desc.forceGLSLScalarBufferLayout = true;
    }
    else if (target == SLANG_DXIL) {
        // Pass through to DXC for better optimization and compatibility
        desc.flags = SLANG_TARGET_FLAG_GENERATE_WHOLE_PROGRAM;
        // Note: Slang will automatically use DXC as the downstream compiler
        // when targeting DXIL with the appropriate compiler options
    }
    std::vector<slang::PreprocessorMacroDesc> macros;

    for (const auto& define : defines) {
        macros.push_back({ define.name.c_str(), define.definition.c_str() });
    }

    Slang::ComPtr<slang::ISession> p_compile_session;

    slang::SessionDesc compile_session_desc;
    compile_session_desc.targets = &desc;
    compile_session_desc.targetCount = 1;

    compile_session_desc.preprocessorMacros = macros.data();
    compile_session_desc.preprocessorMacroCount =
        static_cast<SlangInt>(macros.size());

    std::vector<std::string> searchPaths = { shader_search_path };
    searchPaths.push_back("./");
    searchPaths.push_back(shader_search_path + "/shaders/");

    for (auto& search_path : search_paths) {
        searchPaths.push_back(search_path);
    }

    std::vector<const char*> slangSearchPaths;
    for (auto& path : searchPaths) {
        slangSearchPaths.push_back(path.data());
    }
    compile_session_desc.searchPaths = slangSearchPaths.data();
    compile_session_desc.searchPathCount = (SlangInt)slangSearchPaths.size();

    if (target == SLANG_SPIRV) {
        compile_session_desc.compilerOptionEntries = vk_compiler_options.data();
        compile_session_desc.compilerOptionEntryCount =
            static_cast<SlangInt>(vk_compiler_options.size());
    }
    else if (target == SLANG_DXIL) {
        compile_session_desc.compilerOptionEntries =
            dxc_compiler_options.data();
        compile_session_desc.compilerOptionEntryCount =
            static_cast<SlangInt>(dxc_compiler_options.size());
    }
    SlangResult result;

    result = global_session->createSession(
        compile_session_desc, p_compile_session.writeRef());

    assert(result == SLANG_OK);

    Slang::ComPtr<slang::IBlob> diagnostics;

    if (target == SLANG_HOST_EXECUTABLE) {
        // result = SlangShaderCompiler::addCPPHeaderInclude(slangRequest);
        assert(result == SLANG_OK);
    }

    static std::atomic<unsigned> shader_id = 0;

    auto load_module_from_source =
        [&](const std::string& sourceCode,
            slang::ISession* session,
            const std::string& name) -> slang::IModule* {
        auto id = shader_id++;
        return session->loadModuleFromSourceString(
            (std::to_string(id) + name).c_str(),
            (std::to_string(id) + name).c_str(),
            sourceCode.c_str(),
            diagnostics.writeRef());
    };

    auto load_module_from_path =
        [&](const std::filesystem::path& modulePath,
            slang::ISession* session) -> slang::IModule* {
        return session->loadModule(
            modulePath.generic_string().c_str(), diagnostics.writeRef());
    };

    std::vector<Slang::ComPtr<slang::IModule>> modules;
    bool loaded_successfully = false;

    std::string module_name = paths.empty() ? "unnamed" : paths[0];

    // Try to load from source code if provided
    if (!sourceCodes.empty()) {
        for (const auto& sourceCode : sourceCodes) {
            if (sourceCode.empty())
                continue;

            auto m = load_module_from_source(
                sourceCode, p_compile_session.get(), module_name);
            if (m) {
                modules.emplace_back(m);
                loaded_successfully = true;
            }

            else {
                if (diagnostics) {
                    error_string = (const char*)diagnostics->getBufferPointer();
                }
            }
        }
    }

    // Load all paths if provided
    for (const auto& path : paths) {
        if (path.empty())
            continue;

        auto m = load_module_from_path(path, p_compile_session.get());
        if (m) {
            modules.emplace_back(m);
            loaded_successfully = true;
        }
        else {
            if (diagnostics) {
                error_string = (const char*)diagnostics->getBufferPointer();
            }
        }
    }

    // Report error only if nothing could be loaded
    if (!loaded_successfully) {
        if (diagnostics) {
            error_string = (const char*)diagnostics->getBufferPointer();
        }
        return;
    }

    std::vector<slang::IComponentType*> components;

    for (auto& module : modules) {
        components.push_back(module.get());
    }

    Slang::ComPtr<slang::IEntryPoint> entry;

    if (!std::string(entryPoint).empty()) {
        CHECK_REPORTED_ERROR();

        result = modules[0]->findAndCheckEntryPoint(
            entryPoint, stage, entry.writeRef(), diagnostics.writeRef());
        CHECK_REPORTED_ERROR();
        components.push_back(entry.get());
    }

    Slang::ComPtr<slang::IComponentType> program;
    result = p_compile_session->createCompositeComponentType(
        components.data(),
        components.size(),
        program.writeRef(),
        diagnostics.writeRef());

    CHECK_REPORTED_ERROR();

    Slang::ComPtr<slang::IComponentType> linkedProgram;

    result = program->link(linkedProgram.writeRef(), diagnostics.writeRef());

    CHECK_REPORTED_ERROR();

    shader_reflection =
        shader_reflect(linkedProgram.get(), shaderType, diagnostics.writeRef());
    CHECK_REPORTED_ERROR();

    if (target == SLANG_SHADER_HOST_CALLABLE) {
        result = linkedProgram->getEntryPointHostCallable(
            0, 0, ppSharedLirary.writeRef(), diagnostics.writeRef());
        CHECK_REPORTED_ERROR();
        assert(result == SLANG_OK);
    }
    else {
        result = linkedProgram->getTargetCode(
            0, ppResultBlob.writeRef(), diagnostics.writeRef());
        CHECK_REPORTED_ERROR();
        assert(ppResultBlob);
        assert(result == SLANG_OK);
    }

    releaseGlobalSession();
}

ProgramHandle ShaderFactory::createProgram(const ProgramDesc& desc) const
{
    ProgramHandle ret;
    ret = ProgramHandle::Create(new Program());

    // Create a modifiable copy of the descriptor
    ProgramDesc modified_desc = desc;

    // Automatically add NVAPI extension defines if hlslExtensionsUAV is set
    if (modified_desc.hlslExtensionsUAV >= 0) {
        modified_desc.define("ENABLE_SER", "1");
        modified_desc.define(
            "NV_SHADER_EXTN_SLOT",
            "u" + std::to_string(modified_desc.hlslExtensionsUAV));
        modified_desc.define("NV_SHADER_EXTN_REGISTER_SPACE", "space0");
        // Enable NVAPI support in HLSL prelude for DXC
        modified_desc.define("SLANG_HLSL_ENABLE_NVAPI", "1");
    }

    ret->desc = modified_desc;

    SlangCompileTarget target =
        (RHI::get_backend() == nvrhi::GraphicsAPI::VULKAN) ? SLANG_SPIRV
                                                           : SLANG_DXIL;

    // Try to load from cache first
    if (try_load_from_cache(
            modified_desc, ret->blob, ret->reflection_info, target)) {
        // Successfully loaded from cache
        return ret;
    }

    // Cache miss - compile the shader
    SlangCompile(
        modified_desc.paths,
        modified_desc.source_code,
        modified_desc.entry_name.c_str(),
        modified_desc.shaderType,
        modified_desc.get_profile().c_str(),
        modified_desc.macros,
        ret->reflection_info,
        ret->blob,
        ret->library,
        ret->error_string,
        target,
        std::addressof(ret->linkedProgram),
        modified_desc.nvapi_support);

    // Add NVAPI extension binding to reflection if hlslExtensionsUAV is set
    if (modified_desc.hlslExtensionsUAV >= 0 && ret->error_string.empty()) {
        unsigned space = 0;  // space0
        unsigned slot = modified_desc.hlslExtensionsUAV;

        // Ensure space exists in binding layout
        if (ret->reflection_info.binding_spaces.size() <= space) {
            ret->reflection_info.binding_spaces.resize(space + 1);
        }

        // Add the NVAPI UAV binding
        nvrhi::BindingLayoutItem nvapi_item;
        nvapi_item.slot = slot;
        nvapi_item.type = nvrhi::ResourceType::StructuredBuffer_UAV;
        nvapi_item.size = 1;  // Array size

        ret->reflection_info.binding_spaces[space].addItem(nvapi_item);
        ret->reflection_info.binding_spaces[space].visibility =
            modified_desc.shaderType;
        ret->reflection_info.binding_spaces[space]
            .registerSpaceIsDescriptorSet = true;
        ret->reflection_info.binding_spaces[space].registerSpace = space;

        // Add to binding locations map
        ret->reflection_info.binding_locations["g_NvidiaExt"] = std::make_tuple(
            static_cast<unsigned int>(space),
            static_cast<unsigned int>(
                ret->reflection_info.binding_spaces[space].bindings.size() -
                1));
    }

    // Save to cache if compilation was successful
    if (ret->blob && ret->error_string.empty()) {
        save_to_cache(modified_desc, ret->blob, ret->reflection_info, target);
    }

    return ret;
}

std::string ShaderFactory::get_cache_filename(
    const ProgramDesc& desc,
    SlangCompileTarget target) const
{
    size_t hash = desc.calculate_hash();

    // Add target to hash
    hash ^= std::hash<int>{}(static_cast<int>(target)) + 0x9e3779b9 +
            (hash << 6) + (hash >> 2);

    // Add backend to hash
    auto backend = RHI::get_backend();
    hash ^= std::hash<int>{}(static_cast<int>(backend)) + 0x9e3779b9 +
            (hash << 6) + (hash >> 2);

    std::stringstream ss;
    ss << std::hex << hash;

    const char* extension = "";
    switch (target) {
        case SLANG_SPIRV: extension = ".spv"; break;
        case SLANG_DXIL: extension = ".dxil"; break;
        case SLANG_SHADER_HOST_CALLABLE: extension = ".host"; break;
        default: extension = ".bin"; break;
    }

    return ss.str() + extension;
}

bool ShaderFactory::try_load_from_cache(
    const ProgramDesc& desc,
    Slang::ComPtr<ISlangBlob>& blob,
    ShaderReflectionInfo& reflection_info,
    SlangCompileTarget target) const
{
    if (!cache_enabled) {
        return false;
    }

    auto cache_dir = get_cache_directory();
    auto cache_file = cache_dir + "/" + get_cache_filename(desc, target);
    auto meta_file = cache_file + ".meta";

    namespace fs = std::filesystem;

    if (!fs::exists(cache_file) || !fs::exists(meta_file)) {
        return false;
    }

    try {
        // Read shader binary
        std::ifstream shader_stream(cache_file, std::ios::binary);
        if (!shader_stream) {
            return false;
        }

        shader_stream.seekg(0, std::ios::end);
        size_t size = shader_stream.tellg();
        shader_stream.seekg(0, std::ios::beg);

        std::vector<char> buffer(size);
        shader_stream.read(buffer.data(), size);
        shader_stream.close();

        if (!shader_stream.good() && !shader_stream.eof()) {
            return false;
        }

        // Read metadata (reflection info)
        std::ifstream meta_stream(meta_file, std::ios::binary);
        if (!meta_stream) {
            return false;
        }

        // Deserialize reflection info
        // This is a simplified version - you'd need proper serialization
        size_t space_count;
        meta_stream.read(
            reinterpret_cast<char*>(&space_count), sizeof(space_count));

        reflection_info.binding_spaces.resize(space_count);
        for (size_t i = 0; i < space_count; ++i) {
            size_t item_count;
            meta_stream.read(
                reinterpret_cast<char*>(&item_count), sizeof(item_count));

            for (size_t j = 0; j < item_count; ++j) {
                nvrhi::BindingLayoutItem item;
                uint32_t type_val, size_val;
                uint16_t slot_val;

                meta_stream.read(
                    reinterpret_cast<char*>(&type_val), sizeof(type_val));
                meta_stream.read(
                    reinterpret_cast<char*>(&slot_val), sizeof(slot_val));
                meta_stream.read(
                    reinterpret_cast<char*>(&size_val), sizeof(size_val));

                item.type = static_cast<nvrhi::ResourceType>(type_val);
                item.slot = slot_val;
                item.size = static_cast<uint16_t>(size_val);

                reflection_info.binding_spaces[i].addItem(item);
            }

            meta_stream.read(
                reinterpret_cast<char*>(
                    &reflection_info.binding_spaces[i].visibility),
                sizeof(reflection_info.binding_spaces[i].visibility));
            meta_stream.read(
                reinterpret_cast<char*>(
                    &reflection_info.binding_spaces[i].registerSpace),
                sizeof(reflection_info.binding_spaces[i].registerSpace));
        }

        // Read binding locations
        size_t binding_count;
        meta_stream.read(
            reinterpret_cast<char*>(&binding_count), sizeof(binding_count));

        for (size_t i = 0; i < binding_count; ++i) {
            size_t name_len;
            meta_stream.read(
                reinterpret_cast<char*>(&name_len), sizeof(name_len));

            std::string name(name_len, '\0');
            meta_stream.read(&name[0], name_len);

            unsigned space, index;
            meta_stream.read(reinterpret_cast<char*>(&space), sizeof(space));
            meta_stream.read(reinterpret_cast<char*>(&index), sizeof(index));

            reflection_info.binding_locations[name] =
                std::make_tuple(space, index);
        }

        meta_stream.close();

        // Create blob from buffer using our custom implementation
        blob = Slang::ComPtr<ISlangBlob>(new CustomBlob(std::move(buffer)));

        return true;
    }
    catch (const std::exception&) {
        return false;
    }
}

void ShaderFactory::save_to_cache(
    const ProgramDesc& desc,
    const Slang::ComPtr<ISlangBlob>& blob,
    const ShaderReflectionInfo& reflection_info,
    SlangCompileTarget target) const
{
    if (!cache_enabled || !blob) {
        return;
    }

    namespace fs = std::filesystem;

    auto cache_dir = get_cache_directory();

    // Create cache directory if it doesn't exist
    if (!fs::exists(cache_dir)) {
        fs::create_directories(cache_dir);
    }

    auto cache_file = cache_dir + "/" + get_cache_filename(desc, target);
    auto meta_file = cache_file + ".meta";

    try {
        // Write shader binary
        std::ofstream shader_stream(cache_file, std::ios::binary);
        if (!shader_stream) {
            return;
        }

        shader_stream.write(
            static_cast<const char*>(blob->getBufferPointer()),
            blob->getBufferSize());
        shader_stream.close();

        // Write metadata (reflection info)
        std::ofstream meta_stream(meta_file, std::ios::binary);
        if (!meta_stream) {
            return;
        }

        // Serialize reflection info
        size_t space_count = reflection_info.binding_spaces.size();
        meta_stream.write(
            reinterpret_cast<const char*>(&space_count), sizeof(space_count));

        for (const auto& space : reflection_info.binding_spaces) {
            size_t item_count = space.bindings.size();
            meta_stream.write(
                reinterpret_cast<const char*>(&item_count), sizeof(item_count));

            for (const auto& item : space.bindings) {
                uint32_t type_val = static_cast<uint32_t>(item.type);
                uint16_t slot_val = item.slot;
                uint32_t size_val = static_cast<uint32_t>(item.size);

                meta_stream.write(
                    reinterpret_cast<const char*>(&type_val), sizeof(type_val));
                meta_stream.write(
                    reinterpret_cast<const char*>(&slot_val), sizeof(slot_val));
                meta_stream.write(
                    reinterpret_cast<const char*>(&size_val), sizeof(size_val));
            }

            meta_stream.write(
                reinterpret_cast<const char*>(&space.visibility),
                sizeof(space.visibility));
            meta_stream.write(
                reinterpret_cast<const char*>(&space.registerSpace),
                sizeof(space.registerSpace));
        }

        // Write binding locations
        size_t binding_count = reflection_info.binding_locations.size();
        meta_stream.write(
            reinterpret_cast<const char*>(&binding_count),
            sizeof(binding_count));

        for (const auto& [name, location] : reflection_info.binding_locations) {
            size_t name_len = name.length();
            meta_stream.write(
                reinterpret_cast<const char*>(&name_len), sizeof(name_len));
            meta_stream.write(name.data(), name_len);

            unsigned space = std::get<0>(location);
            unsigned index = std::get<1>(location);
            meta_stream.write(
                reinterpret_cast<const char*>(&space), sizeof(space));
            meta_stream.write(
                reinterpret_cast<const char*>(&index), sizeof(index));
        }

        meta_stream.close();
    }
    catch (const std::exception&) {
        // Silently fail - caching is not critical
    }
}

RUZINO_NAMESPACE_CLOSE_SCOPE
