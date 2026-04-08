#include "shaderCompiler.h"

#include <stdexcept>

#include "RHI/api.h"

#ifdef _WIN32
#include <codecvt>
#endif

#include <limits.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "slang.h"

RUZINO_NAMESPACE_OPEN_SCOPE

std::filesystem::path SlangShaderCompiler::find_root(
    const std::filesystem::path& ul)
{
    std::filesystem::path executable_path;
#ifdef _WIN32
    char p[MAX_PATH];
    GetModuleFileNameA(NULL, p, MAX_PATH);
    executable_path = std::filesystem::path(p).parent_path();
#else
    char p[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", p, PATH_MAX);
    if (count != -1) {
        p[count] = '\0';
        executable_path = std::filesystem::path(p).parent_path();
    }
    else {
        throw std::runtime_error("Failed to get executable path.");
    }
#endif
    return executable_path / "rti";
}

SlangResult SlangShaderCompiler::addHLSLPrelude(slang::IGlobalSession* session)
{
    std::filesystem::path includePath = ".";

    auto root = find_root(includePath);

    auto prelude_name = "/slang/slang-hlsl-prelude.h";

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

    auto prelude_name = "/slang/slang-cpp-prelude.h";

    std::ostringstream prelude;
    prelude << "#include \"" << root.generic_string() + prelude_name
            << "\"\n\n";

    // std::cerr << prelude.str() << std::endl;
    session->setLanguagePrelude(
        SLANG_SOURCE_LANGUAGE_HLSL, prelude.str().c_str());
    return SLANG_OK;
}

SlangResult SlangShaderCompiler::addCPPHeaderInclude(
    SlangCompileRequest* slangRequest)
{
    auto prelude_path = find_root(".") / "slang";

    auto prelude_command = "-I" + prelude_path.generic_string();

    // Inclusion in prelude should be passed to down stream compilers.....
    const char* args[] = { "-Xgenericcpp...", prelude_command.c_str(), "-X." };
    return slangRequest->processCommandLineArguments(
        args, sizeof(args) / sizeof(const char*));
}

SlangResult SlangShaderCompiler::addHLSLHeaderInclude(
    SlangCompileRequest* slangRequest)
{
    auto hlsl_path = find_root(".") / "nvapi";

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

    auto prelude_name = "/slang/slang-cuda-prelude.h";

    std::ostringstream prelude;
    prelude << "#include \"" << root.generic_string() + prelude_name
            << "\"\n\n";

    // std::cerr << prelude.str() << std::endl;
    session->setLanguagePrelude(
        SLANG_SOURCE_LANGUAGE_HLSL, prelude.str().c_str());
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
