#include "materialX.h"

#include <pxr/imaging/hdMtlx/hdMtlx.h>

#include <algorithm>
#include <fstream>

#include "MaterialX/SlangShaderGenerator.h"
#include "MaterialXCore/Document.h"
#include "MaterialXFormat/Util.h"
#include "MaterialXGenShader/Shader.h"
#include "MaterialXGenShader/Util.h"
#include "RHI/Hgi/format_conversion.hpp"
#include "RHI/shaderCompiler.h"
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

void Hd_RUZINO_MaterialX::reset_shared_state()
{
    // Rebuild shared_document from the (scene-independent) libraries so the
    // next render delegate starts with a clean document instead of one still
    // holding the previous scene's accumulated material/shader nodes. See the
    // header doc for why this matters. libraries is left untouched.
    std::lock_guard<std::mutex> doc_lock(document_mutex);
    std::lock_guard<std::mutex> cache_lock(nodedef_cache_mutex_);

    shared_document = mx::createDocument();
    shared_document->importLibrary(libraries);
    nodedef_cache_.clear();
    spdlog::info("MaterialX: shared document reset for next render delegate");
}

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

    const SdfPath& id = GetId();

    HdMaterialNetwork2 hdNetwork;
    SdfPath materialPath;

    SdfPath surfTerminalPath;
    HdMaterialNode2 const* surfTerminal;
    HdMaterialNetwork2Interface netInterface = FetchMaterialNetwork(
        sceneDelegate, hdNetwork, materialPath, surfTerminalPath, surfTerminal);

    // Read shader_path from config dict (populated by config:shader_path
    // attribute on the material prim). This uses USD's built-in forwarding
    // mechanism and works in both USD 25.05 and 26.x.
    VtValue customParamValue;
    auto configIt = hdNetwork.config.find("shader_path");
    if (configIt != hdNetwork.config.end()) {
        customParamValue = configIt->second;
    }

    spdlog::debug(
        "Material {}: shader_path lookup result: {}",
        id.GetText(),
        customParamValue.IsEmpty() ? "EMPTY" : "found");

    // If previously using a custom shader, force a full MaterialX shader
    // regeneration because the cached state is stale.
    bool was_custom_shader = has_valid_shader;
    has_valid_shader = false;
    shader_path.clear();
    if (was_custom_shader) {
        can_use_incremental_update = false;
        last_network_hash = 0;
    }

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

                shader_ready = true;
                shader_generation++;
                *dirtyBits = HdChangeTracker::Clean;
                return;
            }
        }
    }

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

    // Reset shader_ready so ensure_shader_ready() will reprocess the
    // newly generated eval_shader_source on the next GetShader() call.
    // This handles the case where GetShader() was called before Sync()
    // (which can happen when a material is created mid-frame).
    shader_ready = false;

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

bool Hd_RUZINO_MaterialX::isEmissive() const
{
    // MaterialX standard_surface: scalar "emission" > 0 (regardless of color).
    // UsdPreviewSurface: "emissiveColor" with any non-zero channel.
    // We check both parameter name conventions. The mappings are populated
    // after shader generation (MtlxGenerateShader); if empty, the material
    // hasn't been finalized yet and we conservatively return false.
    if (cached_parameter_mappings.empty())
        return false;

    // standard_surface: emission (scalar float)
    auto itEmission = cached_parameter_mappings.find("emission");
    if (itEmission != cached_parameter_mappings.end()) {
        float emissionStrength = reinterpret_cast<const float&>(
            material_data.data[itEmission->second.dataLocation]);
        if (emissionStrength > 0.0f)
            return true;
    }
    // UsdPreviewSurface: emissiveColor (color3f) — any channel > 0
    auto itEmissiveColor = cached_parameter_mappings.find("emissiveColor");
    if (itEmissiveColor != cached_parameter_mappings.end()) {
        unsigned int loc = itEmissiveColor->second.dataLocation;
        for (int c = 0; c < 3; c++) {
            float ch =
                reinterpret_cast<const float&>(material_data.data[loc + c]);
            if (ch > 0.0f)
                return true;
        }
    }
    return false;
}

GfVec3f Hd_RUZINO_MaterialX::getEmissionRadiance() const
{
    if (cached_parameter_mappings.empty())
        return GfVec3f(0.0f);

    GfVec3f radiance(0.0f);

    // standard_surface: emission_color (color3f) * emission (scalar float)
    auto itEmission = cached_parameter_mappings.find("emission");
    auto itEmissionColor = cached_parameter_mappings.find("emission_color");
    if (itEmission != cached_parameter_mappings.end() &&
        itEmissionColor != cached_parameter_mappings.end()) {
        float strength = reinterpret_cast<const float&>(
            material_data.data[itEmission->second.dataLocation]);
        unsigned int colorLoc = itEmissionColor->second.dataLocation;
        radiance[0] =
            reinterpret_cast<const float&>(material_data.data[colorLoc + 0]) *
            strength;
        radiance[1] =
            reinterpret_cast<const float&>(material_data.data[colorLoc + 1]) *
            strength;
        radiance[2] =
            reinterpret_cast<const float&>(material_data.data[colorLoc + 2]) *
            strength;
        return radiance;
    }

    // UsdPreviewSurface: emissiveColor (color3f)
    auto itEmissiveColor = cached_parameter_mappings.find("emissiveColor");
    if (itEmissiveColor != cached_parameter_mappings.end()) {
        unsigned int loc = itEmissiveColor->second.dataLocation;
        radiance[0] =
            reinterpret_cast<const float&>(material_data.data[loc + 0]);
        radiance[1] =
            reinterpret_cast<const float&>(material_data.data[loc + 1]);
        radiance[2] =
            reinterpret_cast<const float&>(material_data.data[loc + 2]);
        return radiance;
    }

    return GfVec3f(0.0f);
}

uint32_t Hd_RUZINO_MaterialX::getEmissionTextureIndex() const
{
    // The emission texture, if any, is registered in texture_id_locations under
    // the texture variable name derived from the emission_color/emissiveColor
    // input. For standard_surface the MaterialX texture node feeding
    // emission_color is named like "emission_color" (the uniform variable);
    // for UsdPreviewSurface it is "emissiveColor". We search
    // texture_id_locations for any key containing these tokens.
    if (texture_id_locations.empty())
        return 0xffffffffu;

    // Look for a texture variable matching the emission inputs. The exact key
    // depends on the shader generator's variable naming, so we do a substring
    // search to be robust.
    for (const auto& [name, loc] : texture_id_locations) {
        if (name.find("emission") != std::string::npos ||
            name.find("emissive") != std::string::npos) {
            uint32_t texIdx = material_data.data[loc];
            return texIdx;
        }
    }
    return 0xffffffffu;
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
