#include "RHI/shaderCompiler.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "RHI/api.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <linux/limits.h>
#include <unistd.h>
#endif

#include <filesystem>
#include <fstream>

#include "slang.h"

RUZINO_NAMESPACE_OPEN_SCOPE

static std::filesystem::path get_executable_dir()
{
#ifdef _WIN32
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
#else
    char buffer[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len != -1) {
        buffer[len] = '\0';
        return std::filesystem::path(buffer).parent_path();
    }
    return std::filesystem::current_path();
#endif
}

std::filesystem::path SlangShaderCompiler::get_exe_dir()
{
    return get_executable_dir();
}

static std::filesystem::path find_sdk_root()
{
    std::filesystem::path marker = "SDK/slang/include/slang-cuda-prelude.h";

    std::vector<std::filesystem::path> search_paths;

    const char* env_path = std::getenv("RUZINO_SDK_PATH");
    if (env_path && strlen(env_path) > 0) {
        search_paths.push_back(std::filesystem::path(env_path));
    }

    auto exe_dir = get_executable_dir();
    search_paths.push_back(exe_dir);
    search_paths.push_back(exe_dir.parent_path());
    search_paths.push_back(std::filesystem::current_path());

    for (const auto& base : search_paths) {
        if (std::filesystem::exists(base / marker)) {
            return std::filesystem::canonical(base);
        }
    }

    auto current = std::filesystem::absolute(std::filesystem::current_path());
    while (current.has_parent_path()) {
        if (std::filesystem::exists(current / marker)) {
            return std::filesystem::canonical(current);
        }
        current = current.parent_path();
    }

    throw std::runtime_error(
        "SDK root not found. Set RUZINO_SDK_PATH or ensure SDK is installed "
        "correctly.");
}

std::filesystem::path SlangShaderCompiler::find_root(
    const std::filesystem::path& ul)
{
    static std::filesystem::path cached_root;
    if (cached_root.empty()) {
        cached_root = find_sdk_root();
    }
    return cached_root;
}

std::filesystem::path SlangShaderCompiler::get_shader_dir(ShaderDirType type)
{
    static bool is_installed = false;
    static std::filesystem::path cached_base_dir;
    static std::filesystem::path cached_shader_dir;

    if (cached_shader_dir.empty()) {
        auto exe_dir = get_executable_dir();

        std::vector<std::filesystem::path> shader_search_paths = {
            exe_dir / "shaders",
            exe_dir.parent_path() / "shaders",
        };

        for (const auto& candidate : shader_search_paths) {
            if (std::filesystem::exists(candidate) &&
                std::filesystem::exists(candidate / "renderer")) {
                is_installed = true;
                cached_shader_dir = candidate;
                break;
            }
        }

        if (!is_installed) {
            auto root = find_root(".");

            std::vector<std::filesystem::path> possible_paths = {
                root / "source" / "Runtime" / "renderer" / "nodes" / "shaders",
                root / "source" / "Runtime" / "renderer" / "source",
                root / "source" / "Editor" / "geometry_nodes"
            };

            for (const auto& path : possible_paths) {
                if (std::filesystem::exists(path)) {
                    cached_base_dir = root / "source";
                    break;
                }
            }

            if (cached_base_dir.empty()) {
                cached_base_dir = std::filesystem::current_path();
            }
        }
    }

    if (is_installed) {
        switch (type) {
            case ShaderDirType::Renderer: return cached_shader_dir / "renderer";
            case ShaderDirType::GPUAssembler:
                return cached_shader_dir / "gpu_assembler";
            case ShaderDirType::GeomNodes:
                return cached_shader_dir / "geom_nodes";
            case ShaderDirType::GeomCompute:
                return cached_shader_dir / "geom_compute";
            default: return cached_shader_dir;
        }
    }
    else {
        switch (type) {
            case ShaderDirType::Renderer:
                return cached_base_dir / "Runtime" / "renderer" / "nodes" /
                       "shaders";
            case ShaderDirType::GPUAssembler:
                return cached_base_dir / "Runtime" / "renderer" / "source" /
                       "shaders";
            case ShaderDirType::GeomNodes:
                return cached_base_dir / "Editor" / "geometry_nodes";
            case ShaderDirType::GeomCompute:
                return cached_base_dir / "Editor" / "geometry" / "source" /
                       "algorithms" / "CS";
            default: return cached_base_dir;
        }
    }
}

SlangResult SlangShaderCompiler::addHLSLPrelude(slang::IGlobalSession* session)
{
    std::filesystem::path includePath = ".";

    auto root = find_root(includePath);

    auto prelude_name = "/SDK/slang/include/slang-hlsl-prelude.h";

    std::ostringstream prelude;
    prelude << "#include \"" << root.generic_string() + prelude_name
            << "\"\n\n";

    // std::cerr << prelude.str() << std::endl;
    session->setLanguagePrelude(
        SLANG_SOURCE_LANGUAGE_HLSL, prelude.str().c_str());
    return SLANG_OK;
}

SlangResult SlangShaderCompiler::addCPPPrelude(slang::IGlobalSession* session)
{
    std::filesystem::path includePath = ".";

    auto root = find_root(includePath);

    auto prelude_name = "/SDK/slang/include/slang-cpp-prelude.h";

    std::ostringstream prelude;
    prelude << "#include \"" << root.generic_string() + prelude_name
            << "\"\n\n";

    // std::cerr << prelude.str() << std::endl;
    session->setLanguagePrelude(
        SLANG_SOURCE_LANGUAGE_CPP, prelude.str().c_str());
    return SLANG_OK;
}

SlangResult SlangShaderCompiler::addCPPHeaderInclude(
    SlangCompileRequest* slangRequest)
{
    auto prelude_path = find_root(".") / "SDK" / "slang" / "include";

    auto prelude_command = "-I" + prelude_path.generic_string();

    // Inclusion in prelude should be passed to down stream compilers.....
    const char* args[] = { "-Xgenericcpp...", prelude_command.c_str(), "-X." };
    return slangRequest->processCommandLineArguments(
        args, sizeof(args) / sizeof(const char*));
}

SlangResult SlangShaderCompiler::addHLSLHeaderInclude(
    SlangCompileRequest* slangRequest)
{
    auto hlsl_path = find_root(".") / "SDK" / "slang" / "include";

    auto hlsl_path_name = "-I" + hlsl_path.generic_string();

    // Inclusion in prelude should be passed to down stream compilers.....
    const char* args[] = { "-Xdxc...", hlsl_path_name.c_str(), "-X." };
    return slangRequest->processCommandLineArguments(
        args, sizeof(args) / sizeof(const char*));
}

SlangResult SlangShaderCompiler::addHLSLSupportPreDefine(
    SlangCompileRequest* slangRequest)
{
    // However, this predefine remains to dxc...
    slangRequest->addPreprocessorDefine("SLANG_HLSL_ENABLE_NVAPI", "1");
    slangRequest->addPreprocessorDefine(
        "NV_SHADER_EXTN_REGISTER_SPACE", "space0");
    slangRequest->addPreprocessorDefine("NV_SHADER_EXTN_SLOT", "u127");
    return SLANG_OK;
}
#if RUZINO_WITH_CUDA

SlangResult SlangShaderCompiler::addCUDAPrelude(slang::IGlobalSession* session)
{
    std::filesystem::path includePath = ".";

    auto root = find_root(includePath);

    auto prelude_name = "/SDK/slang/include/slang-cuda-prelude.h";

    std::ostringstream prelude;
    prelude << "#include \"" << root.generic_string() + prelude_name
            << "\"\n\n";

    // std::cerr << prelude.str() << std::endl;
    session->setLanguagePrelude(
        SLANG_SOURCE_LANGUAGE_CUDA, prelude.str().c_str());
    return SLANG_OK;
}

SlangResult SlangShaderCompiler::addOptiXHeaderInclude(
    SlangCompileRequest* slangRequest)
{
    auto optix_path = find_root(".") / "optix";
    auto optix_path_name = "-I" + optix_path.generic_string();

    // Inclusion in prelude should be passed to down stream compilers.....
    const char* args[] = { "-Xnvrtc...", optix_path_name.c_str(), "-X." };
    return slangRequest->processCommandLineArguments(
        args, sizeof(args) / sizeof(const char*));
}

SlangResult SlangShaderCompiler::addOptiXSupportPreDefine(
    SlangCompileRequest* slangRequest)
{
    // However, this predefine remains to nvrtc...
    slangRequest->addPreprocessorDefine("SLANG_CUDA_ENABLE_OPTIX", "1");
    return SLANG_OK;
}

SlangResult SlangShaderCompiler::addOptiXSupport(
    SlangCompileRequest* slangRequest)
{
    addOptiXSupportPreDefine(slangRequest);
    return addOptiXHeaderInclude(slangRequest);
}

#endif

void SlangShaderCompiler::save_file(
    const std::string& filename,
    const char* data)
{
    std::ofstream file(filename);

    file << std::string(data);
    file.close();
}

RUZINO_NAMESPACE_CLOSE_SCOPE
