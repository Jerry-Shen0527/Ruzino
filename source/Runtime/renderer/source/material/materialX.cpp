#include "materialX.h"

#include <pxr/imaging/hdMtlx/hdMtlx.h>

#include <algorithm>
#include <fstream>

#include "../../../../Core/RHI/source/shaderCompiler.h"
#include "MaterialX/SlangShaderGenerator.h"
#include "MaterialXCore/Document.h"
#include "MaterialXFormat/Util.h"
#include "MaterialXGenShader/Shader.h"
#include "MaterialXGenShader/Util.h"
#include "RHI/Hgi/format_conversion.hpp"
#include "bindlessContext.h"
#include "hdMtlxFast.h"
#include "materialFilter.h"
#include "spdlog/spdlog.h"
RUZINO_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(_tokens, (file)(sourceColorSpace)(raw)(srgb));

namespace mx = MaterialX;

MaterialX::GenContextPtr Hd_RUZINO_MaterialX::shader_gen_context_ =
    std::make_shared<mx::GenContext>(mx::SlangShaderGenerator::create());
MaterialX::DocumentPtr Hd_RUZINO_MaterialX::libraries = mx::createDocument();
MaterialX::DocumentPtr Hd_RUZINO_MaterialX::shared_document = nullptr;

std::mutex Hd_RUZINO_MaterialX::shadergen_mutex;
std::mutex Hd_RUZINO_MaterialX::document_mutex;
std::once_flag Hd_RUZINO_MaterialX::shader_gen_initialized_;
std::unordered_map<std::string, MaterialX::NodeDefPtr>
    Hd_RUZINO_MaterialX::nodedef_cache_;
std::mutex Hd_RUZINO_MaterialX::nodedef_cache_mutex_;

Hd_RUZINO_MaterialX::Hd_RUZINO_MaterialX(SdfPath const& id)
    : Hd_RUZINO_Material(id)
{
    std::call_once(shader_gen_initialized_, []() {
        mx::FileSearchPath searchPath = mx::getDefaultDataSearchPath();

        // Add current working directory to search path for libraries
        searchPath.append(
            mx::FilePath(std::filesystem::current_path().string()));

        searchPath.append(mx::FileSearchPath("usd/hd_RUZINO/resources"));

        loadLibraries({ "libraries" }, searchPath, libraries);
        mx::loadLibraries(
            { "usd/hd_RUZINO/resources/libraries" }, searchPath, libraries);
        shader_gen_context_->registerSourceCodeSearchPath(searchPath);

        shader_gen_context_->pushUserData(
            mx::HW::USER_DATA_BINDING_CONTEXT, BindlessContext::create());

        // Create shared document with libraries pre-imported (all materials use
        // this)
        shared_document = mx::createDocument();
        shared_document->importLibrary(libraries);
        spdlog::info(
            "MaterialX: Shared document created - all materials will be added "
            "to this document");
    });
}

void Hd_RUZINO_MaterialX::Sync(
    HdSceneDelegate* sceneDelegate,
    HdRenderParam* renderParam,
    HdDirtyBits* dirtyBits)
{
    Hd_RUZINO_Material::Sync(sceneDelegate, renderParam, dirtyBits);
    spdlog::info("MaterialX::Sync called for material '{}'", GetId().GetText());

    auto param = static_cast<Hd_RUZINO_RenderParam*>(renderParam);

    ensure_material_data_handle(param);

    // First check if this material has a custom shader_path
    const SdfPath& id = GetId();
    VtValue customParamValue = sceneDelegate->Get(id, TfToken("shader_path"));
    if (!customParamValue.IsEmpty()) {
        if (customParamValue.IsHolding<std::string>()) {
            shader_path = customParamValue.UncheckedGet<std::string>();
        }
        else if (customParamValue.IsHolding<SdfAssetPath>()) {
            shader_path =
                customParamValue.UncheckedGet<SdfAssetPath>().GetAssetPath();
        }

        // Validate shader path
        if (!shader_path.empty()) {
            std::filesystem::path shader_file_path(shader_path);
            if (!shader_file_path.is_absolute()) {
                shader_file_path = std::filesystem::path(
                                       SlangShaderCompiler::get_shader_dir(
                                           ShaderDirType::Renderer)) /
                                   shader_path;
            }

            if (std::filesystem::exists(shader_file_path) &&
                std::filesystem::is_regular_file(shader_file_path)) {
                this->has_valid_shader = true;
                this->shader_path = shader_file_path.string();
                spdlog::info(
                    "Material {}: Using custom eval shader '{}' instead of "
                    "MaterialX",
                    id.GetText(),
                    shader_file_path.string());

                // Extract material name from file path for the callable
                // function name
                material_name = shader_file_path.stem().string();
                std::replace(
                    material_name.begin(), material_name.end(), '-', '_');
                std::replace(
                    material_name.begin(), material_name.end(), '.', '_');

                // The shader is already a complete eval callable at the file
                // Just store the path, no need to load source here

                shader_ready = true;
                shader_generation++;
                *dirtyBits = HdChangeTracker::Clean;
                return;
            }
        }
    }

    HdMaterialNetwork2 hdNetwork;
    SdfPath materialPath;

    SdfPath surfTerminalPath;
    HdMaterialNode2 const* surfTerminal;
    HdMaterialNetwork2Interface netInterface = FetchMaterialNetwork(
        sceneDelegate, hdNetwork, materialPath, surfTerminalPath, surfTerminal);

    spdlog::info(
        "MaterialX: MaterialPath = '{}', SurfTerminalPath = '{}'",
        materialPath.GetText(),
        surfTerminalPath.GetText());

    if (!surfTerminal) {
        spdlog::warn(
            "MaterialX: No surface terminal found for material '{}'. "
            "This material has no connected shader network.",
            GetId().GetText());
        *dirtyBits = HdChangeTracker::Clean;
        return;
    }

    HdMtlxTexturePrimvarData hdMtlxData;

    // Compute network structure hash to detect if only parameters changed
    size_t current_network_hash = compute_network_hash(netInterface);
    bool network_structure_changed =
        (current_network_hash != last_network_hash);

    if (!network_structure_changed && can_use_incremental_update) {
        // Only parameters changed, no need to regenerate shader
        spdlog::info(
            "MaterialX: Only parameters changed for material '{}', doing "
            "incremental update",
            GetId().GetText());

        update_parameters_incremental(netInterface, hdMtlxData);
        BuildGPUTextures(param);

        *dirtyBits = HdChangeTracker::Clean;
        return;
    }

    // Network structure changed or first time, do full shader generation
    MaterialX::ElementPtr mtlx_element =
        HdMtlxCreateMtlxDocumentFromHdNetworkFast(
            hdNetwork,
            *surfTerminal,
            surfTerminalPath,
            materialPath,
            shared_document,
            document_mutex,
            &hdMtlxData);

    if (!mtlx_element) {
        spdlog::error(
            "MaterialX: Failed to add material to shared document for '{}'",
            GetId().GetText());
        *dirtyBits = HdChangeTracker::Clean;
        return;
    }

    spdlog::info("MaterialX: Added material to shared document successfully");

    CollectTextures(netInterface, hdMtlxData);

    // Full shader generation with network structure changed
    MtlxGenerateShader(mtlx_element, netInterface, hdMtlxData);

    // Update hash after successful shader generation
    last_network_hash = current_network_hash;
    can_use_incremental_update = true;

    BuildGPUTextures(param);

    *dirtyBits = HdChangeTracker::Clean;
}

void Hd_RUZINO_MaterialX::ensure_shader_ready(const ShaderFactory& factory)
{
    if (shader_ready) {
        return;
    }

    // If we have a custom shader path, mark as ready (path-based, not
    // source-based)
    if (has_valid_shader) {
        spdlog::info(
            "MaterialX: Custom eval shader path ready for material '{}': {}",
            material_name,
            shader_path);
        shader_ready = true;
        return;
    }

    // Otherwise, process MaterialX shader generation
    // Call base class but temporarily disable shader_ready flag
    // because MaterialX needs additional processing
    Hd_RUZINO_Material::ensure_shader_ready(factory);
    shader_ready = false;  // Reset since MaterialX needs more work

    if (!eval_shader_source.empty()) {
        spdlog::info(
            "MaterialX: Processing shader source ({} bytes)",
            eval_shader_source.size());

        // Replace all data loading placeholders with actual data code
        constexpr char DATA_PLACEHOLDER[] = "$BindlessDataLoading";
        size_t pos = 0;
        while ((pos = eval_shader_source.find(DATA_PLACEHOLDER, pos)) !=
               std::string::npos) {
            eval_shader_source.replace(
                pos, strlen(DATA_PLACEHOLDER), get_data_code);
            pos += get_data_code.length();
        }

        // Extract texture sampling code from fetch_shader_data for opacity
        // function
        constexpr char TEXTURE_SAMPLING_PLACEHOLDER[] =
            "$TextureSamplingForOpacity";
        size_t texture_sampling_pos =
            eval_shader_source.find(TEXTURE_SAMPLING_PLACEHOLDER);

        if (texture_sampling_pos != std::string::npos) {
            // Find fetch_shader_data function
            size_t data_func_pos =
                eval_shader_source.find("void fetch_shader_data(");
            if (data_func_pos != std::string::npos) {
                size_t data_func_start =
                    eval_shader_source.find("{", data_func_pos);
                size_t data_func_end =
                    eval_shader_source.find("\nvoid ", data_func_start);

                if (data_func_end == std::string::npos) {
                    data_func_end = eval_shader_source.length();
                }

                std::string data_func_body = eval_shader_source.substr(
                    data_func_start, data_func_end - data_func_start);

                // Find the standalone ";" separator that marks end of bindless
                // loading
                size_t separator_pos = std::string::npos;
                size_t search_start = 0;

                while (true) {
                    size_t semicolon = data_func_body.find(";\n", search_start);
                    if (semicolon == std::string::npos)
                        break;

                    size_t line_start = data_func_body.rfind('\n', semicolon);
                    if (line_start == std::string::npos)
                        line_start = 0;
                    else
                        line_start++;

                    bool only_whitespace = true;
                    for (size_t i = line_start; i < semicolon; i++) {
                        if (data_func_body[i] != ' ' &&
                            data_func_body[i] != '\t') {
                            only_whitespace = false;
                            break;
                        }
                    }

                    if (only_whitespace) {
                        separator_pos = semicolon + 2;
                        break;
                    }

                    search_start = semicolon + 1;
                }

                if (separator_pos != std::string::npos) {
                    // Find the start of material params assignment (e.g.,
                    // "PackedStandardSurfaceMaterialParams params")
                    size_t params_start = data_func_body.find(
                        "PackedStandardSurfaceMaterialParams params");
                    if (params_start == std::string::npos) {
                        params_start = data_func_body.find(
                            "PreviewSurfaceMaterialParams params");
                    }

                    if (params_start != std::string::npos) {
                        // Extract texture sampling code between separator and
                        // params assignment
                        size_t texture_code_end =
                            data_func_body.rfind('\n', params_start);
                        std::string texture_sampling_code =
                            data_func_body.substr(
                                separator_pos,
                                texture_code_end - separator_pos);

                        // Replace the placeholder
                        eval_shader_source.replace(
                            texture_sampling_pos,
                            strlen(TEXTURE_SAMPLING_PLACEHOLDER),
                            texture_sampling_code);
                    }
                }
            }
        }

        // Extract opacity computation from fetch_shader_data and inject into
        // fetch_shader_opacity
        constexpr char OPACITY_PLACEHOLDER[] = "$OpacityComputation";
        size_t opacity_placeholder_pos =
            eval_shader_source.find(OPACITY_PLACEHOLDER);

        if (opacity_placeholder_pos != std::string::npos) {
            // Find fetch_shader_data function
            size_t data_func_pos =
                eval_shader_source.find("void fetch_shader_data(");
            if (data_func_pos != std::string::npos) {
                size_t data_func_start =
                    eval_shader_source.find("{", data_func_pos);
                size_t data_func_end =
                    eval_shader_source.find("\nvoid ", data_func_start);

                if (data_func_end == std::string::npos) {
                    data_func_end = eval_shader_source.length();
                }

                std::string data_func_body = eval_shader_source.substr(
                    data_func_start, data_func_end - data_func_start);

                std::string opacity_computation;

                // Check if this is standard_surface (uses
                // packStandardSurfaceMaterialParams)
                size_t pack_func_pos =
                    data_func_body.find("packStandardSurfaceMaterialParams(");

                if (pack_func_pos != std::string::npos) {
                    // Standard surface - find opacity parameter in function
                    // call Count parameters to find the opacity one (39th
                    // parameter, index 38)
                    size_t param_start =
                        pack_func_pos +
                        strlen("packStandardSurfaceMaterialParams(");
                    int param_count = 0;
                    int paren_depth = 1;
                    size_t opacity_param_start = param_start;
                    size_t opacity_param_end = param_start;
                    bool found_opacity = false;

                    for (size_t i = param_start;
                         i < data_func_body.length() && paren_depth > 0;
                         ++i) {
                        char c = data_func_body[i];
                        if (c == '(') {
                            paren_depth++;
                        }
                        else if (c == ')') {
                            paren_depth--;
                            if (paren_depth == 0) {
                                // Last parameter
                                if (param_count == 38) {
                                    opacity_param_end = i;
                                    found_opacity = true;
                                }
                                break;
                            }
                        }
                        else if (c == ',' && paren_depth == 1) {
                            if (param_count == 38) {
                                opacity_param_end = i;
                                found_opacity = true;
                                break;
                            }
                            param_count++;
                            if (param_count == 38) {
                                opacity_param_start = i + 1;
                            }
                        }
                    }

                    if (found_opacity) {
                        // Extract opacity parameter expression
                        std::string opacity_expr = data_func_body.substr(
                            opacity_param_start,
                            opacity_param_end - opacity_param_start);

                        // Trim whitespace
                        size_t first_non_ws =
                            opacity_expr.find_first_not_of(" \t\n");
                        size_t last_non_ws =
                            opacity_expr.find_last_not_of(" \t\n");
                        if (first_non_ws != std::string::npos) {
                            opacity_expr = opacity_expr.substr(
                                first_non_ws, last_non_ws - first_non_ws + 1);
                        }

                        // Check if opacity_expr is a simple variable or complex
                        // expression Simple variable: just alphanumeric +
                        // underscore, no operators or function calls
                        bool is_simple_var = true;
                        for (char c : opacity_expr) {
                            if (!std::isalnum(c) && c != '_') {
                                is_simple_var = false;
                                break;
                            }
                        }

                        if (is_simple_var) {
                            // Direct parameter variable - need to convert
                            // float3 to float Standard surface opacity is
                            // always float3
                            opacity_computation =
                                "    float opacity_value = (" + opacity_expr +
                                ".r + " + opacity_expr + ".g + " +
                                opacity_expr + ".b) / 3.0;\n";
                        }
                        else {
                            // Complex expression - assume it's already computed
                            // to appropriate type
                            opacity_computation =
                                "    float opacity_value = " + opacity_expr +
                                ";\n";
                        }
                    }
                    else {
                        opacity_computation =
                            "    float opacity_value = 1.0;  // Failed to find "
                            "opacity parameter\n";
                    }
                }
                else {
                    // UsdPreviewSurface - look for params.opacity assignment
                    size_t opacity_assignment =
                        data_func_body.find("params.opacity = ");

                    if (opacity_assignment != std::string::npos) {
                        // Find the standalone ";" separator line
                        size_t separator_pos = std::string::npos;
                        size_t search_start = 0;

                        while (true) {
                            size_t semicolon =
                                data_func_body.find(";\n", search_start);
                            if (semicolon == std::string::npos ||
                                semicolon >= opacity_assignment)
                                break;

                            size_t line_start =
                                data_func_body.rfind('\n', semicolon);
                            if (line_start == std::string::npos)
                                line_start = 0;
                            else
                                line_start++;

                            bool only_whitespace = true;
                            for (size_t i = line_start; i < semicolon; i++) {
                                if (data_func_body[i] != ' ' &&
                                    data_func_body[i] != '\t') {
                                    only_whitespace = false;
                                    break;
                                }
                            }

                            if (only_whitespace) {
                                separator_pos = semicolon + 2;
                                break;
                            }

                            search_start = semicolon + 1;
                        }

                        // Extract just the opacity assignment line
                        size_t opacity_line_end =
                            data_func_body.find(";", opacity_assignment);
                        std::string opacity_line = data_func_body.substr(
                            opacity_assignment,
                            opacity_line_end - opacity_assignment);
                        size_t eq_pos = opacity_line.find("= ");
                        std::string opacity_expr =
                            opacity_line.substr(eq_pos + 2);

                        // For UsdPreviewSurface, opacity value is already
                        // computed in the texture sampling section (if from
                        // texture) or is a direct parameter We just need to
                        // extract the final value
                        opacity_computation =
                            "    float opacity_value = " + opacity_expr + ";\n";
                    }
                    else {
                        opacity_computation =
                            "    float opacity_value = 1.0;  // No opacity in "
                            "material graph\n";
                    }
                }

                eval_shader_source.replace(
                    opacity_placeholder_pos,
                    strlen(OPACITY_PLACEHOLDER),
                    opacity_computation);
            }
        }

        try {
            std::filesystem::create_directories("generated_shaders");
            std::ofstream out("generated_shaders/" + material_name + ".slang");
            if (out.is_open()) {
                out << eval_shader_source;
                out.close();
                spdlog::info(
                    "MaterialX: Saved shader to generated_shaders/{}.slang",
                    material_name);
            }
        }
        catch (const std::exception& e) {
            TF_WARN("Failed to save generated shader: %s", e.what());
        }
        final_shader_source = eval_shader_source + slang_source_code_main;
    }
    else {
        if (!GetId().GetString().empty())
            spdlog::warn(
                "MaterialX: eval_shader_source is empty for material '{}'",
                GetId().GetString());
    }

    // Combine shader parts into final source

    // ProgramDesc program_desc;
    // program_desc.add_source_code(final_shader_source);
    // program_desc.set_shader_type(nvrhi::ShaderType::Callable);
    // program_desc.set_entry_name(material_name);

    // spdlog::info("MaterialX: Creating shader program for '{}'",
    // material_name); final_program = factory.createProgram(program_desc);

    // if (!final_program) {
    //     spdlog::error(
    //         "MaterialX: Failed to create shader program for '{}'",
    //         material_name);
    // }
    // else {
    //     spdlog::info(
    //         "MaterialX: Shader program created successfully for '{}'",
    //         material_name);
    // }

    // assert(final_program);

    shader_ready = true;
    // Note: shader_generation was already incremented by base class
}

void Hd_RUZINO_MaterialX::BuildGPUTextures(Hd_RUZINO_RenderParam* render_param)
{
    auto descriptor_table =
        render_param->InstanceCollection->get_texture_descriptor_table();

    for (auto& texture_resource : textureResources) {
        // Create a thread for asynchronous processing
        std::thread texture_thread([&texture_resource,
                                    this,
                                    descriptor_table]() {
            auto device = RHI::get_device();

            auto image = texture_resource.second.image;

            nvrhi::TextureDesc desc;
            desc.width = image->GetWidth();
            desc.height = image->GetHeight();
            desc.format = RHI::ConvertFromHioFormat(image->GetFormat());

            // Force linear format for non-sRGB textures (like normal maps)
            if (!texture_resource.second.isSRGB) {
                if (desc.format == nvrhi::Format::SRGBA8_UNORM) {
                    desc.format = nvrhi::Format::RGBA8_UNORM;
                }
            }

            desc.initialState = nvrhi::ResourceStates::ShaderResource;
            desc.isRenderTarget = false;
            desc.keepInitialState = true;

            texture_resource.second.texture = device->createTexture(desc);

            auto texture_name = std::filesystem::path(texture_resource.first)
                                    .filename()
                                    .string();

            auto storage_byte_size = image->GetBytesPerPixel();

            std::vector<uint8_t> data(
                image->GetWidth() * image->GetHeight() * storage_byte_size, 0);

            HioImage::StorageSpec storageSpec;
            storageSpec.width = image->GetWidth();
            storageSpec.height = image->GetHeight();
            storageSpec.format = image->GetFormat();
            storageSpec.flipped = true;
            storageSpec.data = data.data();

            // Read the image data asynchronously
            texture_resource.second.image->Read(storageSpec);

            {
                std::lock_guard lock(texture_mutex);
                if (image->GetFormat() == HioFormatUNorm8Vec3srgb) {
                    // rearrange the data to be RGBA
                    std::vector<uint8_t> rgba_data(
                        image->GetWidth() * image->GetHeight() * 4, 0);
                    for (size_t i = 0; i < data.size() / 3; i++) {
                        rgba_data[i * 4] = data[i * 3];
                        rgba_data[i * 4 + 1] = data[i * 3 + 1];
                        rgba_data[i * 4 + 2] = data[i * 3 + 2];
                        rgba_data[i * 4 + 3] = 255;
                    }
                    data = std::move(rgba_data);
                }

                auto [gpu_texture, staging] =
                    RHI::load_texture(desc, data.data());

                texture_resource.second.texture = gpu_texture;
            }

            texture_resource.second.descriptor =
                descriptor_table->CreateDescriptorHandle(
                    nvrhi::BindingSetItem::Texture_SRV(
                        0, texture_resource.second.texture, desc.format));

            if (texture_resource.second.texture) {
                auto texture_id = texture_resource.second.descriptor.Get();

                spdlog::info(
                    "BuildGPUTextures: Looking for texture key '{}' with ID {}",
                    texture_resource.first,
                    texture_id);

                // Find the data location for this texture's ID
                auto it = texture_id_locations.find(texture_resource.first);
                if (it != texture_id_locations.end()) {
                    unsigned int location = it->second;
                    // Write texture ID directly to the data buffer
                    memcpy(
                        &material_data.data[location],
                        &texture_id,
                        sizeof(unsigned int));

                    // Mark data as dirty so it will be uploaded
                    material_data_dirty = true;

                    spdlog::info(
                        "Texture '{}' ID {} written to data location {}",
                        texture_resource.first,
                        texture_id,
                        location);
                }
                else {
                    spdlog::warn(
                        "Texture '{}' not found in texture_id_locations map",
                        texture_resource.first);
                }
            }
        });

        // Add the thread to the render_param for tracking
        render_param->texture_loading_threads.push_back(
            std::move(texture_thread));
    }
}

void Hd_RUZINO_MaterialX::CollectTextures(
    HdMaterialNetwork2Interface netInterface,
    HdMtlxTexturePrimvarData hdMtlxData)
{
    // Collect texture names and paths into a vector.
    for (const SdfPath& texturePath : hdMtlxData.hdTextureNodes) {
        TfToken textureNodeName = texturePath.GetToken();
        // Get the file parameter from the node.
        VtValue vFile =
            netInterface.GetNodeParameterValue(textureNodeName, _tokens->file);
        std::string path;
        if (vFile.IsHolding<SdfAssetPath>()) {
            path = vFile.Get<SdfAssetPath>().GetResolvedPath();
            if (path.empty()) {
                path = vFile.Get<SdfAssetPath>().GetAssetPath();
            }
        }
        else if (vFile.IsHolding<std::string>()) {
            path = vFile.Get<std::string>();
        }

        VtValue sourceColorSpace = netInterface.GetNodeParameterValue(
            textureNodeName, _tokens->sourceColorSpace);

        bool isSRGB = false;
        if (sourceColorSpace.IsHolding<std::string>()) {
            std::string colorSpace = sourceColorSpace.Get<std::string>();
            if (colorSpace == "srgb_texture") {
                isSRGB = true;
            }
        }

        texturePaths[textureNodeName.GetString()] = path;

        // Extract the base name for MaterialX node (last component of path)
        // For example:
        // "/mesh_0/mtl/brickwall_01_usd/brickwall_01_Metalness/brickwall_01_Metalness"
        // -> "brickwall_01_Metalness"
        std::string mxNodeName = texturePath.GetName();

        spdlog::info(
            "CollectTextures: Full path='{}', extracted name='{}', file "
            "path='{}'",
            textureNodeName.GetString(),
            mxNodeName,
            path);

        // Load the texture immediately
        if (!pxr::HioImage::IsSupportedImageFile(path)) {
            TF_WARN(
                "Texture '%s': unsupported file format '%s'.",
                textureNodeName.GetString().c_str(),
                path.c_str());
            continue;
        }

        HioImageSharedPtr image = pxr::HioImage::OpenForReading(path);
        if (!image) {
            TF_WARN(
                "Texture '%s': failed to load image from file '%s'.",
                textureNodeName.GetString().c_str(),
                path.c_str());
            continue;
        }

        // Use MaterialX node name (last path component) as key to match
        // bindlessContext
        textureResources[mxNodeName].filePath = path;
        textureResources[mxNodeName].image = image;
        textureResources[mxNodeName].isSRGB = isSRGB;
    }
}

HdMaterialNetwork2Interface Hd_RUZINO_MaterialX::FetchMaterialNetwork(
    HdSceneDelegate* sceneDelegate,
    HdMaterialNetwork2& hdNetwork,
    SdfPath& materialPath,
    SdfPath& surfTerminalPath,
    HdMaterialNode2 const*& surfTerminal)
{
    HdMaterialNetwork2Interface netInterface =
        FetchNetInterface(sceneDelegate, hdNetwork, materialPath);
    _FixNodeTypes(&netInterface);
    _FixNodeValues(&netInterface);

    const TfToken& terminalNodeName = HdMaterialTerminalTokens->surface;

    surfTerminal =
        _GetTerminalNode(hdNetwork, terminalNodeName, &surfTerminalPath);
    return netInterface;
}

void Hd_RUZINO_MaterialX::MtlxGenerateShader(
    MaterialX::ElementPtr mtlx_element,
    HdMaterialNetwork2Interface netInterface,
    HdMtlxTexturePrimvarData& hdMtlxData)
{
    if (!mtlx_element) {
        TF_RUNTIME_ERROR(
            "MaterialX: Null element passed to MtlxGenerateShader");
        return;
    }

    mx::DocumentPtr mtlx_document = mtlx_element->getDocument();

    // Lock when modifying shared document (for all document modifications)
    {
        std::lock_guard<std::mutex> lock(document_mutex);

        _UpdateTextureNodes(
            &netInterface, hdMtlxData.hdTextureNodes, mtlx_document);

        // The element passed in is already the material element, get its shader
        // node
        mx::NodePtr mxMaterialNode = mtlx_element->asA<mx::Node>();
        if (!mxMaterialNode) {
            TF_RUNTIME_ERROR("MaterialX: Element is not a material node");
            return;
        }

        // Get the shader node connected to this material
        mx::NodePtr mxShaderNode = nullptr;
        for (auto input : mxMaterialNode->getInputs()) {
            if (input->hasNodeName()) {
                mxShaderNode = input->getConnectedNode();
                break;
            }
        }

        if (!mxShaderNode) {
            TF_RUNTIME_ERROR("MaterialX: No shader node found for material");
            return;
        }

        // Use a vector with single element for compatibility with
        // _FixOmittedConnections
        std::vector<mx::TypedElementPtr> renderable = { mxShaderNode };

        _FixOmittedConnections(mtlx_document, renderable);

        // Fix geompropvalue nodes that don't have 'geomprop' input
        for (auto nodeGraph : mtlx_document->getNodeGraphs()) {
            for (auto node : nodeGraph->getNodes("geompropvalue")) {
                auto geompropInput = node->getInput("geomprop");

                if (geompropInput) {
                    std::string interfaceName =
                        geompropInput->getInterfaceName();

                    if (!interfaceName.empty()) {
                        auto ngInput = nodeGraph->getInput(interfaceName);
                        if (ngInput && ngInput->hasValueString()) {
                            geompropInput->setInterfaceName("");
                            geompropInput->setValue(
                                ngInput->getValueString(), "string");
                        }
                        else {
                            geompropInput->setInterfaceName("");
                            geompropInput->setValue("st", "string");
                        }
                    }
                    else if (!geompropInput->hasValueString()) {
                        geompropInput->setValue("st", "string");
                    }
                }
                else {
                    node->setInputValue("geomprop", "st", "string");
                }
            }
        }
    }  // Release lock after all document modifications

    using namespace mx;

    // Get shader node again (outside lock for shader generation)
    mx::NodePtr mxMaterialNode = mtlx_element->asA<mx::Node>();
    mx::NodePtr mxShaderNode = nullptr;
    for (auto input : mxMaterialNode->getInputs()) {
        if (input->hasNodeName()) {
            mxShaderNode = input->getConnectedNode();
            break;
        }
    }

    // Use the shader node directly (not from renderable array)
    TypedElementPtr element = mxShaderNode;

    std::string elementName(element->getNamePath());

    // Use material ID path to create unique names to avoid conflicts
    // when multiple instances use the same MaterialX but different paths
    std::string materialIdStr = GetId().GetString();
    // Replace path separators with underscores to make valid shader name
    std::replace(materialIdStr.begin(), materialIdStr.end(), '/', '_');
    // Remove leading underscore
    if (!materialIdStr.empty() && materialIdStr[0] == '_') {
        materialIdStr = materialIdStr.substr(1);
    }
    material_name = mx::createValidName(materialIdStr);

    spdlog::info(
        "MaterialX: Generating shader for material '{}' from element '{}'",
        material_name,
        elementName);

    ShaderGenerator& shader_generator_ =
        shader_gen_context_->getShaderGenerator();

    // Clear parameter mappings before generating new shader
    // (BindlessContext is shared globally, needs to be reset for each material)
    BindlessContextPtr context =
        shader_gen_context_->getUserData<BindlessContext>(
            mx::HW::USER_DATA_BINDING_CONTEXT);
    if (context) {
        context->clear_parameter_mappings();
    }

    {
        std::lock_guard lock(shadergen_mutex);
        try {
            auto shader = shader_generator_.generate(
                material_name, element, *shader_gen_context_);

            if (!shader) {
                TF_RUNTIME_ERROR(
                    "MaterialX: Shader generation failed for material '%s'",
                    material_name.c_str());
                return;
            }

            eval_shader_source = shader->getSourceCode(mx::Stage::PIXEL);

            if (eval_shader_source.empty()) {
                TF_RUNTIME_ERROR(
                    "MaterialX: Empty shader source generated for material "
                    "'%s'",
                    material_name.c_str());
                return;
            }

            spdlog::info(
                "MaterialX: Generated {} bytes of shader code",
                eval_shader_source.size());

            BindlessContextPtr context =
                shader_gen_context_->getUserData<BindlessContext>(
                    mx::HW::USER_DATA_BINDING_CONTEXT);

            if (!context) {
                TF_RUNTIME_ERROR("MaterialX: Failed to get BindlessContext");
                return;
            }

            get_data_code = context->get_data_code();
            material_data = context->get_material_data();
            texture_id_locations = context->get_texture_id_locations();

            // Cache parameter mappings for incremental updates
            cached_parameter_mappings = context->get_parameter_mappings();

            spdlog::info(
                "MaterialX: Cached {} parameter mappings for incremental "
                "updates",
                cached_parameter_mappings.size());

            material_data_handle->write_data(&material_data);

            spdlog::info(
                "MaterialX: Shader generation complete for '{}'",
                material_name);
        }
        catch (const std::exception& e) {
            TF_RUNTIME_ERROR(
                "MaterialX: Exception during shader generation for '%s': %s",
                material_name.c_str(),
                e.what());
            return;
        }
    }
}

void Hd_RUZINO_MaterialX::upload_material_data()
{
    if (material_data_dirty && material_data_handle) {
        material_data_handle->write_data(&material_data);
        material_data_dirty = false;
    }
}

size_t Hd_RUZINO_MaterialX::compute_network_hash(
    const HdMaterialNetwork2Interface& netInterface)
{
    // Compute hash of network structure (node types, connections) but not
    // parameter values
    std::hash<std::string> hasher;
    size_t hash_value = 0;

    // Hash all node types
    TfTokenVector nodeNames = netInterface.GetNodeNames();
    for (const auto& nodeName : nodeNames) {
        TfToken nodeType = netInterface.GetNodeType(nodeName);
        hash_value ^= hasher(nodeType.GetString()) + 0x9e3779b9 +
                      (hash_value << 6) + (hash_value >> 2);
    }

    // Hash all connections (structure) - just check node existence and types
    // since HdMaterialNetwork2Interface doesn't provide direct connection
    // queries
    for (const auto& nodeName : nodeNames) {
        TfTokenVector paramNames =
            netInterface.GetAuthoredNodeParameterNames(nodeName);
        for (const auto& paramName : paramNames) {
            // Hash parameter names that are present (not values)
            std::string paramStr =
                nodeName.GetString() + ":" + paramName.GetString();
            hash_value ^= hasher(paramStr) + 0x9e3779b9 + (hash_value << 6) +
                          (hash_value >> 2);
        }
    }

    return hash_value;
}

void Hd_RUZINO_MaterialX::update_parameters_incremental(
    const HdMaterialNetwork2Interface& netInterface,
    HdMtlxTexturePrimvarData& hdMtlxData)
{
    if (cached_parameter_mappings.empty()) {
        spdlog::warn(
            "MaterialX: No cached parameter mappings for material '{}', cannot "
            "do incremental update",
            GetId().GetText());
        return;
    }

    spdlog::info(
        "MaterialX: Updating {} parameter mappings for material '{}'",
        cached_parameter_mappings.size(),
        GetId().GetText());

    // Get all nodes for parameter queries
    TfTokenVector nodeNames = netInterface.GetNodeNames();

    int updated_count = 0;
    for (const auto& nodeName : nodeNames) {
        // Get all parameters for this node
        TfTokenVector paramNames =
            netInterface.GetAuthoredNodeParameterNames(nodeName);

        for (const auto& paramName : paramNames) {
            // O(1) lookup in cached mappings
            auto it = cached_parameter_mappings.find(paramName.GetString());
            if (it == cached_parameter_mappings.end()) {
                continue;  // Not a parameter we're tracking
            }

            const ParameterMapping& mapping = it->second;
            VtValue paramValue =
                netInterface.GetNodeParameterValue(nodeName, paramName);

            if (paramValue.IsEmpty()) {
                continue;
            }

            // Update material_data based on parameter type
            using namespace MaterialX;
            if (mapping.parameterType == Type::FLOAT) {
                float val = paramValue.Get<float>();
                memcpy(
                    &material_data.data[mapping.dataLocation],
                    &val,
                    sizeof(float));
                updated_count++;
            }
            else if (
                mapping.parameterType == Type::INTEGER ||
                mapping.parameterType == Type::BOOLEAN) {
                int val = paramValue.Get<int>();
                memcpy(
                    &material_data.data[mapping.dataLocation],
                    &val,
                    sizeof(int));
                updated_count++;
            }
            else if (mapping.parameterType == Type::VECTOR2) {
                GfVec2f val = paramValue.Get<GfVec2f>();
                memcpy(
                    &material_data.data[mapping.dataLocation],
                    val.data(),
                    sizeof(float) * 2);
                updated_count++;
            }
            else if (mapping.parameterType == Type::VECTOR3) {
                GfVec3f val = paramValue.Get<GfVec3f>();
                memcpy(
                    &material_data.data[mapping.dataLocation],
                    val.data(),
                    sizeof(float) * 3);
                updated_count++;
            }
            else if (mapping.parameterType == Type::VECTOR4) {
                GfVec4f val = paramValue.Get<GfVec4f>();
                memcpy(
                    &material_data.data[mapping.dataLocation],
                    val.data(),
                    sizeof(float) * 4);
                updated_count++;
            }
            else if (mapping.parameterType == Type::COLOR3) {
                GfVec3f val = paramValue.Get<GfVec3f>();
                memcpy(
                    &material_data.data[mapping.dataLocation],
                    val.data(),
                    sizeof(float) * 3);
                updated_count++;
            }
            else if (mapping.parameterType == Type::COLOR4) {
                GfVec4f val = paramValue.Get<GfVec4f>();
                memcpy(
                    &material_data.data[mapping.dataLocation],
                    val.data(),
                    sizeof(float) * 4);
                updated_count++;
            }
            else if (mapping.parameterType == Type::MATRIX44) {
                GfMatrix4f val = paramValue.Get<GfMatrix4f>();
                memcpy(
                    &material_data.data[mapping.dataLocation],
                    val.data(),
                    sizeof(float) * 16);
                updated_count++;
            }
        }
    }

    // Upload updated data to GPU
    if (updated_count > 0) {
        material_data_handle->write_data(&material_data);
        spdlog::info(
            "MaterialX: Incremental update wrote {} parameters to GPU for "
            "material '{}'",
            updated_count,
            GetId().GetText());
    }
    else {
        spdlog::info(
            "MaterialX: Incremental parameter update completed - {} parameters "
            "checked for material '{}'",
            updated_count,
            GetId().GetText());
    }
}

RUZINO_NAMESPACE_CLOSE_SCOPE
