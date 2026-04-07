
//
// Copyright 2020 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
// #

#include "renderDelegate.h"

#include <pxr/imaging/hd/field.h>
#include <pxr/usdImaging/usdVolImaging/tokens.h>
#include <spdlog/spdlog.h>

#include <iostream>

#include "RHI/Hgi/desc_conversion.hpp"
#include "RHI/rhi.hpp"
#include "config.h"
#include "geometries/field.h"
#include "geometries/mesh.h"
#include "geometries/points.h"
#include "geometries/volume.h"
#include "gpu_compute.h"
#include "hd_RUZINO/render_global_payload.hpp"
#include "instancer.h"
#include "light.h"
#include "material/material.h"
#include "material/materialX.h"
#include "node_exec_eager_render.hpp"
#include "nodes/system/node_system.hpp"
#include "nvrhi/nvrhi.h"
#include "pxr/imaging/hd/extComputation.h"
#include "renderBuffer.h"
#include "renderParam.h"
#include "renderPass.h"
#include "renderer.h"

#define HR_RETURN(hr) \
    if (FAILED(hr))   \
        assert(false);

RUZINO_NAMESPACE_OPEN_SCOPE
using namespace pxr;
TF_DEFINE_PUBLIC_TOKENS(
    Hd_RUZINO_RenderSettingsTokens,
    Hd_RUZINO_RENDER_SETTINGS_TOKEN);

const TfTokenVector Hd_RUZINO_RenderDelegate::SUPPORTED_RPRIM_TYPES = {
    HdPrimTypeTokens->mesh,
    HdPrimTypeTokens->volume,
    HdPrimTypeTokens->points,
};

const TfTokenVector Hd_RUZINO_RenderDelegate::SUPPORTED_SPRIM_TYPES = {
    HdPrimTypeTokens->camera,         HdPrimTypeTokens->simpleLight,
    HdPrimTypeTokens->sphereLight,    HdPrimTypeTokens->domeLight,
    HdPrimTypeTokens->material,       HdPrimTypeTokens->drawTarget,
    HdPrimTypeTokens->extComputation, HdPrimTypeTokens->cylinderLight,
    HdPrimTypeTokens->diskLight,      HdPrimTypeTokens->distantLight,
    HdPrimTypeTokens->rectLight,      HdPrimTypeTokens->imageShader
};

const TfTokenVector Hd_RUZINO_RenderDelegate::SUPPORTED_BPRIM_TYPES = {
    HdPrimTypeTokens->renderBuffer,
    UsdVolImagingTokens->openvdbAsset
};

Hd_RUZINO_RenderDelegate::Hd_RUZINO_RenderDelegate() : HdRenderDelegate()
{
    _Initialize();
}

Hd_RUZINO_RenderDelegate::Hd_RUZINO_RenderDelegate(
    const HdRenderSettingsMap& settingsMap)
    : HdRenderDelegate(settingsMap)
{
    _Initialize();
}

static void _RenderCallback(
    Hd_RUZINO_Renderer* renderer,
    HdRenderThread* renderThread)
{
    renderer->Render(renderThread);
}

std::mutex Hd_RUZINO_RenderDelegate::_mutexResourceRegistry;
std::atomic_int Hd_RUZINO_RenderDelegate::_counterResourceRegistry;
HdResourceRegistrySharedPtr Hd_RUZINO_RenderDelegate::_resourceRegistry;

void Hd_RUZINO_RenderDelegate::_Initialize()
{
    // Initialize the settings and settings descriptors.
    _settingDescriptors.resize(5);
    _settingDescriptors[0] = {
        "Enable Scene Colors",
        Hd_RUZINO_RenderSettingsTokens->enableSceneColors,
        VtValue(Hd_RUZINO_Config::GetInstance().useFaceColors)
    };
    _settingDescriptors[1] = {
        "Enable Ambient Occlusion",
        Hd_RUZINO_RenderSettingsTokens->enableAmbientOcclusion,
        VtValue(Hd_RUZINO_Config::GetInstance().ambientOcclusionSamples > 0)
    };
    _settingDescriptors[2] = {
        "Ambient Occlusion Samples",
        Hd_RUZINO_RenderSettingsTokens->ambientOcclusionSamples,
        VtValue(
            static_cast<int>(
                Hd_RUZINO_Config::GetInstance().ambientOcclusionSamples))
    };
    _settingDescriptors[3] = {
        "Samples To Convergence",
        HdRenderSettingsTokens->convergedSamplesPerPixel,
        VtValue(
            static_cast<int>(
                Hd_RUZINO_Config::GetInstance().samplesToConvergence))
    };

    _settingDescriptors[4] = { "Render Mode",
                               Hd_RUZINO_RenderSettingsTokens->renderMode,
                               VtValue(0) };
    _PopulateDefaultSettings(_settingDescriptors);

    // Device
    nvrhi_device = RHI::get_device();

    RenderGlobalPayload global_payload =
        RenderGlobalPayload(&cameras, &lights, &materials, nvrhi_device);

    std::unique_ptr<NodeTreeExecutor> render_executor =
        std::make_unique<EagerNodeTreeExecutorRender>();

    node_system = create_dynamic_loading_system();

    // Try multiple possible locations for render_nodes.json
    std::vector<std::string> search_paths = {
        "render_nodes.json",  // Current directory
        "./render_nodes.json",
        "../render_nodes.json",
        "../../Binaries/Debug/render_nodes.json",
        "../../Binaries/Release/render_nodes.json"
    };

    bool config_loaded = false;
    for (const auto& path : search_paths) {
        if (std::filesystem::exists(path)) {
            try {
                node_system->load_configuration(path);
                config_loaded = true;
                spdlog::info(
                    "Loaded render_nodes.json from: {}",
                    std::filesystem::absolute(path).string());
                break;
            }
            catch (const std::exception& e) {
                spdlog::warn(
                    "Failed to load config from {}: {}", path, e.what());
            }
        }
    }

    if (!config_loaded) {
        spdlog::warn(
            "Could not find render_nodes.json in search paths, node system may "
            "be empty");
    }

    auto plugin_path = std::filesystem::path("./renderer_plugins");
    if (std::filesystem::exists(plugin_path)) {
        for (auto& p : std::filesystem::directory_iterator(plugin_path)) {
            if (p.path().extension() == ".json") {
                try {
                    node_system->load_configuration(p.path().string());
                    spdlog::info("Loaded plugin config: {}", p.path().string());
                }
                catch (const std::exception& e) {
                    spdlog::warn(
                        "Failed to load plugin config {}: {}",
                        p.path().string(),
                        e.what());
                }
            }
        }
    }

    node_system->set_node_tree_executor(std::move(render_executor));
    node_system->allow_ui_execution = false;
    node_system->init();

    node_system->set_global_params(global_payload);

    _renderParam = std::make_shared<Hd_RUZINO_RenderParam>(
        &_renderThread, &_sceneVersion, node_system.get(), &materials);

    _renderer = std::make_shared<Hd_RUZINO_Renderer>(_renderParam.get());

    // Initialize GPU Scene Assembler resource allocator
    GPUSceneAssember::initialize_instance();

    spdlog::info("GPU Scene Assembler initialized");

    // Set the background render thread's rendering entrypoint to
    // Hd_RUZINO_Renderer::Render.
    _renderThread.SetRenderCallback(
        std::bind(_RenderCallback, _renderer.get(), &_renderThread));
    _renderThread.StartThread();

    // Initialize one resource registry for all embree plugins
    std::lock_guard<std::mutex> guard(_mutexResourceRegistry);

    if (_counterResourceRegistry.fetch_add(1) == 0) {
        _resourceRegistry = std::make_shared<HdResourceRegistry>();
    }
    _resourceRegistry = std::make_shared<HdResourceRegistry>();
}

HdAovDescriptor Hd_RUZINO_RenderDelegate::GetDefaultAovDescriptor(
    const TfToken& name) const
{
    if (name == HdAovTokens->color) {
        return HdAovDescriptor(
            HdFormatFloat32Vec4, false, VtValue(GfVec4f(0.0f)));
    }
    if (name == HdAovTokens->normal || name == HdAovTokens->Neye) {
        return HdAovDescriptor(
            HdFormatFloat32Vec3, false, VtValue(GfVec3f(-1.0f)));
    }
    if (name == HdAovTokens->depth) {
        return HdAovDescriptor(HdFormatFloat32, false, VtValue(1.0f));
    }
    if (name == HdAovTokens->primId || name == HdAovTokens->instanceId ||
        name == HdAovTokens->elementId) {
        return HdAovDescriptor(HdFormatInt32, false, VtValue(-1));
    }
    HdParsedAovToken aovId(name);
    if (aovId.isPrimvar) {
        return HdAovDescriptor(
            HdFormatFloat32Vec3, false, VtValue(GfVec3f(0.0f)));
    }

    return HdAovDescriptor();
}

Hd_RUZINO_RenderDelegate::~Hd_RUZINO_RenderDelegate()
{
    // Clean up GPU Scene Assembler before destroying other resources
    GPUSceneAssember::destroy_instance();
    spdlog::info("GPU Scene Assembler destroyed");

    node_system->get_node_tree_executor()->finalize(
        node_system->get_node_tree());
    // for (auto&& node : _renderParam->node_tree->nodes) {
    //    node->runtime_storage.reset();
    //}
    _resourceRegistry.reset();
    _renderer.reset();
    _globalPayload.reset();
    _renderParam.reset();

    RHI::get_device()->runGarbageCollection();

    std::cout << "Destroying RenderDelegate" << std::endl;
}

const TfTokenVector& Hd_RUZINO_RenderDelegate::GetSupportedRprimTypes() const
{
    return SUPPORTED_RPRIM_TYPES;
}

const TfTokenVector& Hd_RUZINO_RenderDelegate::GetSupportedSprimTypes() const
{
    return SUPPORTED_SPRIM_TYPES;
}

const TfTokenVector& Hd_RUZINO_RenderDelegate::GetSupportedBprimTypes() const
{
    return SUPPORTED_BPRIM_TYPES;
}

HdResourceRegistrySharedPtr Hd_RUZINO_RenderDelegate::GetResourceRegistry()
    const
{
    return _resourceRegistry;
}

void Hd_RUZINO_RenderDelegate::CommitResources(HdChangeTracker* tracker)
{
}

HdRenderPassSharedPtr Hd_RUZINO_RenderDelegate::CreateRenderPass(
    HdRenderIndex* index,
    const HdRprimCollection& collection)
{
    return std::make_shared<Hd_RUZINO_RenderPass>(
        index, collection, &_renderThread, _renderer.get(), &_sceneVersion);
}

HdRprim* Hd_RUZINO_RenderDelegate::CreateRprim(
    const TfToken& typeId,
    const SdfPath& rprimId)
{
    if (typeId == HdPrimTypeTokens->mesh) {
        auto mesh = new Hd_RUZINO_Mesh(rprimId);
        // spdlog::info(("Create Rprim id=" + rprimId.GetString()).c_str());

        meshes.push_back(mesh);
        return mesh;
    }
    else if (typeId == HdPrimTypeTokens->volume) {
        auto volume = new Hd_RUZINO_Volume(rprimId);
        spdlog::info("Created volume: {}", rprimId.GetText());
        return volume;
    }
    else if (typeId == HdPrimTypeTokens->points) {
        auto points = new Hd_RUZINO_Points(rprimId);
        spdlog::info("Created points: {}", rprimId.GetText());
        return points;
    }
    TF_CODING_ERROR(
        "Unknown Rprim type=%s id=%s", typeId.GetText(), rprimId.GetText());
    return nullptr;
}

void Hd_RUZINO_RenderDelegate::DestroyRprim(HdRprim* rPrim)
{
    // spdlog::info(("Destroy Rprim id=" + rPrim->GetId().GetString()).c_str());
    meshes.erase(
        std::remove(meshes.begin(), meshes.end(), rPrim), meshes.end());
    delete rPrim;
}

HdSprim* Hd_RUZINO_RenderDelegate::CreateSprim(
    const TfToken& typeId,
    const SdfPath& sprimId)
{
    if (typeId == HdPrimTypeTokens->camera) {
        auto camera = new Hd_RUZINO_Camera(sprimId);
        cameras.push_back(camera);
        return camera;
    }
    else if (typeId == HdPrimTypeTokens->extComputation) {
        return new HdExtComputation(sprimId);
    }
    else if (typeId == HdPrimTypeTokens->material) {
        // Check if this is a shader-based material by looking for shader_path
        // Note: At creation time we don't have access to sceneDelegate,
        // so we'll create MaterialX by default and let Sync handle shader
        // detection For now, we'll just create MaterialX which can fall back to
        // shader if needed
        auto material = new Hd_RUZINO_MaterialX(sprimId);
        spdlog::info("=== Created material: {} ===", sprimId.GetText());
        materials[sprimId] = material;

        assert(materials[sprimId] != nullptr);
        spdlog::info(
            "Material stored in map, total materials: {}", materials.size());

        return material;
    }
    else if (typeId == HdPrimTypeTokens->simpleLight) {
        auto light = new Hd_RUZINO_Simple_Light(sprimId, typeId);
        lights.push_back(light);
        return light;
    }
    else if (typeId == HdPrimTypeTokens->distantLight) {
        auto light = new Hd_RUZINO_Distant_Light(sprimId, typeId);
        lights.push_back(light);
        return light;
    }
    else if (typeId == HdPrimTypeTokens->sphereLight) {
        auto light = new Hd_RUZINO_Sphere_Light(sprimId, typeId);
        lights.push_back(light);
        return light;
    }
    else if (typeId == HdPrimTypeTokens->rectLight) {
        auto light = new Hd_RUZINO_Rect_Light(sprimId, typeId);
        lights.push_back(light);
        return light;
    }
    else if (typeId == HdPrimTypeTokens->diskLight) {
        auto light = new Hd_RUZINO_Disk_Light(sprimId, typeId);
        lights.push_back(light);
        return light;
    }
    else if (typeId == HdPrimTypeTokens->cylinderLight) {
        auto light = new Hd_RUZINO_Cylinder_Light(sprimId, typeId);
        lights.push_back(light);
        return light;
    }
    else if (typeId == HdPrimTypeTokens->domeLight) {
        auto light = new Hd_RUZINO_Dome_Light(sprimId, typeId);
        lights.push_back(light);
        return light;
    }
    else if (
        typeId == TfToken("drawTarget") || typeId == TfToken("imageShader")) {
        // MaterialX specific types - create a minimal fallback
        spdlog::info("Creating Sprim of type: {}", typeId.GetText());
        return new HdExtComputation(sprimId);
    }
    else {
        TF_CODING_ERROR("Unknown Sprim Type %s", typeId.GetText());
    }

    return nullptr;
}

HdSprim* Hd_RUZINO_RenderDelegate::CreateFallbackSprim(const TfToken& typeId)
{
    // For fallback sprims, create objects with an empty scene path.
    // They'll use default values and won't be updated by a scene delegate.
    if (typeId == HdPrimTypeTokens->camera) {
        auto camera = new Hd_RUZINO_Camera(SdfPath::EmptyPath());
        cameras.push_back(camera);
        return camera;
    }
    else if (typeId == HdPrimTypeTokens->extComputation) {
        return new HdExtComputation(SdfPath::EmptyPath());
    }
    else if (typeId == HdPrimTypeTokens->material) {
        auto material = new Hd_RUZINO_MaterialX(SdfPath::EmptyPath());
        materials[SdfPath::EmptyPath()] = material;

        assert(materials[SdfPath::EmptyPath()] != nullptr);

        return material;
    }
    else if (typeId == HdPrimTypeTokens->simpleLight) {
        // Don't add fallback lights to the lights list
        return new Hd_RUZINO_Simple_Light(SdfPath::EmptyPath(), typeId);
    }
    else if (typeId == HdPrimTypeTokens->distantLight) {
        return new Hd_RUZINO_Distant_Light(SdfPath::EmptyPath(), typeId);
    }
    else if (typeId == HdPrimTypeTokens->sphereLight) {
        return new Hd_RUZINO_Sphere_Light(SdfPath::EmptyPath(), typeId);
    }
    else if (typeId == HdPrimTypeTokens->rectLight) {
        return new Hd_RUZINO_Rect_Light(SdfPath::EmptyPath(), typeId);
    }
    else if (typeId == HdPrimTypeTokens->diskLight) {
        return new Hd_RUZINO_Disk_Light(SdfPath::EmptyPath(), typeId);
    }
    else if (typeId == HdPrimTypeTokens->cylinderLight) {
        auto light = new Hd_RUZINO_Cylinder_Light(SdfPath::EmptyPath(), typeId);
        lights.push_back(light);
        return new Hd_RUZINO_Cylinder_Light(SdfPath::EmptyPath(), typeId);
    }
    else if (typeId == HdPrimTypeTokens->domeLight) {
        return new Hd_RUZINO_Dome_Light(SdfPath::EmptyPath(), typeId);
    }
    else if (
        typeId == TfToken("drawTarget") || typeId == TfToken("imageShader")) {
        // MaterialX specific types - create a minimal fallback
        return new HdExtComputation(SdfPath::EmptyPath());
    }
    else {
        TF_CODING_ERROR("Unknown Sprim Type %s", typeId.GetText());
    }

    return nullptr;
}

void Hd_RUZINO_RenderDelegate::DestroySprim(HdSprim* sPrim)
{
    // spdlog::info((sPrim->GetId().GetAsString() + " destroyed").c_str());
    lights.erase(
        std::remove(lights.begin(), lights.end(), sPrim), lights.end());
    cameras.erase(
        std::remove(cameras.begin(), cameras.end(), sPrim), cameras.end());

    auto material = dynamic_cast<Hd_RUZINO_Material*>(sPrim);
    if (material) {
        materials.erase(material->GetId());
    }
    delete sPrim;
}

HdBprim* Hd_RUZINO_RenderDelegate::CreateBprim(
    const TfToken& typeId,
    const SdfPath& bprimId)
{
    if (typeId == HdPrimTypeTokens->renderBuffer) {
        // spdlog::info(("Create bprim: type id=" + typeId.GetString() +
        //            ",prim id = " + bprimId.GetString())
        //               .c_str());

        return new Hd_RUZINO_RenderBuffer(bprimId);
    }
    if (typeId == UsdVolImagingTokens->openvdbAsset) {
        // spdlog::info(("Create bprim: type id=" + typeId.GetString() +
        //            ",prim id = " + bprimId.GetString())
        //               .c_str());
        return new Hd_RUZINO_Field(bprimId);
    }

    TF_CODING_ERROR("Unknown Bprim Type %s", typeId.GetText());
    return nullptr;
}

HdBprim* Hd_RUZINO_RenderDelegate::CreateFallbackBprim(const TfToken& typeId)
{
    if (typeId == HdPrimTypeTokens->renderBuffer) {
        spdlog::info(
            ("Create fallback bprim: type id=" + typeId.GetString()).c_str());
        return new Hd_RUZINO_RenderBuffer(SdfPath::EmptyPath());
    }
    if (typeId == UsdVolImagingTokens->openvdbAsset) {
        spdlog::info(
            ("Create fallback bprim: type id=" + typeId.GetString()).c_str());
        return new Hd_RUZINO_Field(SdfPath::EmptyPath());
    }
    TF_CODING_ERROR("Unknown Bprim Type %s", typeId.GetText());
    return nullptr;
}

void Hd_RUZINO_RenderDelegate::DestroyBprim(HdBprim* bPrim)
{
    std::string sentence = "Destroy Bprim";
    auto bprim_name = bPrim->GetId().GetString();
    if (!bprim_name.empty()) {
        sentence += " id=" + bprim_name;
    }
    // spdlog::info(sentence.c_str());
    delete bPrim;
}

HdInstancer* Hd_RUZINO_RenderDelegate::CreateInstancer(
    HdSceneDelegate* delegate,
    const SdfPath& id)
{
    return new Hd_RUZINO_Instancer(delegate, id);
}

void Hd_RUZINO_RenderDelegate::DestroyInstancer(HdInstancer* instancer)
{
    delete instancer;
    // TF_CODING_ERROR("Destroy instancer not supported");

    // spdlog::info(
    //     ("Destroy Instancer id=" + instancer->GetId().GetString()).c_str());
}

HdRenderParam* Hd_RUZINO_RenderDelegate::GetRenderParam() const
{
    return _renderParam.get();
}

VtValue Hd_RUZINO_RenderDelegate::GetRenderSetting(TfToken const& key) const
{
    if (key == TfToken("RenderNodeSystem")) {
        return VtValue(reinterpret_cast<const void*>(&node_system));
    }

#ifdef RUZINO_DIRECT_VK_DISPLAY
    if (key == TfToken("VulkanColorAov")) {
        // Legacy: return default texture for backward compatibility
        if (!_renderParam->default_texture_name.empty()) {
            auto it = _renderParam->presented_textures.find(
                _renderParam->default_texture_name);
            if (it != _renderParam->presented_textures.end() && it->second) {
                // Safe: map element addresses are stable until erase/rehash
                return VtValue(reinterpret_cast<const void*>(&it->second));
            }
        }
    }

    // New: support querying named textures
    // Format: "VulkanColorAov:<texture_name>"
    std::string key_str = key.GetString();
    if (key_str.rfind("VulkanColorAov:", 0) == 0) {
        std::string texture_name =
            key_str.substr(15);  // Skip "VulkanColorAov:"
        auto it = _renderParam->presented_textures.find(texture_name);
        if (it != _renderParam->presented_textures.end()) {
            if (it->second) {
                return VtValue(reinterpret_cast<const void*>(&it->second));
            }
            else {
            }
        }
        else {
            spdlog::warn("  Texture '{}' not found in map", texture_name);
        }
    }

    // Support for TLAS access
    if (key == TfToken("VulkanTLAS")) {
        if (_renderParam && _renderParam->InstanceCollection) {
            auto tlas = _renderParam->InstanceCollection->get_tlas();
            if (tlas) {
                return VtValue(reinterpret_cast<const void*>(tlas));
            }
        }
    }

    // Support for InstanceDescBuffer access
    if (key == TfToken("VulkanInstanceDescBuffer")) {
        if (_renderParam && _renderParam->InstanceCollection) {
            auto buffer = _renderParam->InstanceCollection->instance_pool
                              .get_device_buffer();
            if (buffer) {
                return VtValue(reinterpret_cast<const void*>(buffer));
            }
        }
    }

    // Support for MeshDescBuffer access
    if (key == TfToken("VulkanMeshDescBuffer")) {
        if (_renderParam && _renderParam->InstanceCollection) {
            auto buffer =
                _renderParam->InstanceCollection->mesh_pool.get_device_buffer();
            if (buffer) {
                return VtValue(reinterpret_cast<const void*>(buffer));
            }
        }
    }

    // Support for Bindless Buffer Descriptor Table access
    if (key == TfToken("VulkanBindlessBufferTable")) {
        if (_renderParam && _renderParam->InstanceCollection) {
            auto table =
                _renderParam->InstanceCollection->get_buffer_descriptor_table();
            if (table) {
                // Return the descriptor table, not the manager
                auto descriptor_table = table->GetDescriptorTable();
                return VtValue(reinterpret_cast<const void*>(descriptor_table));
            }
        }
    }

    // Support for Bindless Buffer Layout access
    if (key == TfToken("VulkanBindlessBufferLayout")) {
        if (_renderParam && _renderParam->InstanceCollection) {
            auto layout = _renderParam->InstanceCollection->bindlessData
                              .bufferBindlessLayout.Get();
            if (layout) {
                return VtValue(reinterpret_cast<const void*>(layout));
            }
        }
    }
#endif
    return HdRenderDelegate::GetRenderSetting(key);
}

void Hd_RUZINO_RenderDelegate::SetRenderSetting(
    const TfToken& key,
    const VtValue& value)

{
    if (key == pxr::TfToken("lens_system_ptr")) {
        _renderParam->lens_system =
            static_cast<LensSystem*>(value.Get<void*>());
    }
    else {
        HdRenderDelegate::SetRenderSetting(key, value);
    }
}

bool Hd_RUZINO_RenderDelegate::Stop(bool blocking)
{
    _renderThread.StopRender();
    return HdRenderDelegate::Stop(blocking);
}
bool Hd_RUZINO_RenderDelegate::IsParallelSyncEnabled(
    const TfToken& primType) const
{
    // if (primType == HdPrimTypeTokens->material)
    //     return true;
    return HdRenderDelegate::IsParallelSyncEnabled(primType);
}

RUZINO_NAMESPACE_CLOSE_SCOPE
