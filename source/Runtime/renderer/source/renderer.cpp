#include "renderer.h"

#include <spdlog/spdlog.h>

#include "camera.h"
#include "material/material.h"
#include "node_exec_eager_render.hpp"
#include "nodes/core/api.hpp"
#include "nodes/system/node_system.hpp"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/imaging/hd/tokens.h"
#include "renderBuffer.h"
#include "renderParam.h"

RUZINO_NAMESPACE_OPEN_SCOPE
using namespace pxr;

Hd_RUZINO_Renderer::Hd_RUZINO_Renderer(Hd_RUZINO_RenderParam* render_param)
    : _enableSceneColors(false),
      render_param(render_param)
{
}

Hd_RUZINO_Renderer::~Hd_RUZINO_Renderer()
{
    auto executor = dynamic_cast<EagerNodeTreeExecutorRender*>(
        render_param->node_system->get_node_tree_executor());
}

static TextureHandle create_empty_texture(
    const pxr::GfVec2i& size,
    nvrhi::Format format = nvrhi::Format::RGBA32_FLOAT)
{
    nvrhi::TextureDesc desc =
        nvrhi::TextureDesc{}
            .setWidth(size[0])
            .setHeight(size[1])
            .setFormat(format)
            .setInitialState(nvrhi::ResourceStates::ShaderResource)
            .setKeepInitialState(true)
            .setIsUAV(true);
    auto d = RHI::get_device();
    auto texture = d->createTexture(desc);

    auto commandList = d->createCommandList();
    commandList->open();
    commandList->clearTextureFloat(
        texture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
    commandList->close();
    d->executeCommandList(commandList);
    d->waitForIdle();

    return texture;
}

void Hd_RUZINO_Renderer::Render(HdRenderThread* renderThread)
{
    _completedSamples.store(0);

    render_param->default_texture_name.clear();
    render_param->presented_textures.clear();

    for (auto& material_thread : render_param->material_loading_threads) {
        material_thread.join();
    }
    render_param->material_loading_threads.clear();

    for (auto& texture_thread : render_param->texture_loading_threads) {
        texture_thread.join();
    }
    render_param->texture_loading_threads.clear();

    // Upload material data to GPU after all textures are loaded
    for (auto& material : *render_param->material_map) {
        if (!material.second) {
            continue;
        }

        material.second->upload_material_data();
    }

    auto node_system = render_param->node_system;

    {
        auto& global_payload = node_system->get_node_tree_executor()
                                   ->get_global_payload<RenderGlobalPayload&>();

        // Clear all dirty flags from previous frame at the beginning of each
        // frame
        global_payload.clear_dirty(
            RenderGlobalPayload::SceneDirtyBits::DirtyMaterials);
        global_payload.clear_dirty(
            RenderGlobalPayload::SceneDirtyBits::DirtyGeometry);
        global_payload.clear_dirty(
            RenderGlobalPayload::SceneDirtyBits::DirtyLights);

        global_payload.InstanceCollection =
            render_param->InstanceCollection.get();

        // Ensure all materials have their shaders compiled
        {
            std::vector<std::future<void>> futures;

            for (auto& material : *render_param->material_map) {
                if (!material.second) {
                    continue;
                }

                auto material_path = material.first;

                futures.push_back(
                    std::async(std::launch::async, [&, material_path]() {
                        auto mat = (*render_param->material_map)[material_path];
                        if (!mat)
                            return;

                        mat->ensure_shader_ready(global_payload.shader_factory);
                    }));
            }

            // Wait for all shader compilations to complete
            for (auto& future : futures) {
                future.wait();
            }
        }

        // Check for material changes
        uint32_t current_material_version =
            global_payload.InstanceCollection->get_material_version();
        if (render_param->last_material_version != current_material_version) {
            global_payload.mark_dirty(
                RenderGlobalPayload::SceneDirtyBits::DirtyMaterials);
            render_param->last_material_version = current_material_version;
        }

        // Check for geometry/buffer changes
        uint32_t current_geometry_version =
            global_payload.InstanceCollection->get_geometry_version();
        if (render_param->last_geometry_version != current_geometry_version) {
            global_payload.mark_dirty(
                RenderGlobalPayload::SceneDirtyBits::DirtyGeometry);
            render_param->last_geometry_version = current_geometry_version;
        }

        uint32_t current_light_version =
            global_payload.InstanceCollection->get_light_version();
        if (render_param->last_light_version != current_light_version) {
            global_payload.mark_dirty(
                RenderGlobalPayload::SceneDirtyBits::DirtyLights);
            render_param->last_light_version = current_light_version;
        }

        // global_payload.resource_allocator.gc();
        global_payload.resource_allocator.gc();

        global_payload.lens_system = render_param->lens_system;

        global_payload.reset_accumulation = false;

        // Find a present node to use as required_node so the tree actually
        // executes. Without this, execute() calls compile(tree, nullptr) which
        // only marks ALWAYS_REQUIRED nodes.
        Node* required_node = nullptr;
        for (auto&& node : node_system->get_node_tree()->nodes) {
            std::string id(node->typeinfo->id_name);
            if (id == "present_color" || id == "present_depth") {
                required_node = node.get();
                break;
            }
        }

        node_system->execute(false, required_node);

        // Clear dirty flags after execution
        // Note: nodes should clear specific flags as they handle them
    }

    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        std::string present_name;

        if (_aovBindings[i].aovName == HdAovTokens->depth) {
            present_name = "present_depth";
        }

        if (_aovBindings[i].aovName == HdAovTokens->color) {
            present_name = "present_color";
        }

        // Find ALL present nodes of this type and store each one with data
        for (auto&& node : node_system->get_node_tree()->nodes) {
            std::string node_id(node->typeinfo->id_name);
            if (node_id != present_name) {
                continue;  // Skip non-matching nodes
            }

            // Try to fetch texture from this node
            assert(node->get_inputs().size() == 1);
            auto output_socket = node->get_inputs()[0];
            entt::meta_any data;
            node_system->get_node_tree_executor()
                ->sync_node_to_external_storage(output_socket, data);

            if (!data) {
                spdlog::warn(
                    "Present node '{}' input socket has no data",
                    node->ui_name);
                continue;  // Skip nodes with no data
            }

            // The present node's input holds an nvrhi::TextureHandle
            // (nvrhi::RefCountPtr<nvrhi::ITexture>) produced by upstream render
            // nodes. entt's meta_any::cast<T>() / try_cast route through the
            // meta node table, which is resolved per-DLL; when the value was
            // produced by a different render-node DLL (e.g. path_tracing.dll),
            // that lookup fails even though data holds a valid texture of the
            // exact concrete type. The present node's single input is declared
            // as TextureHandle, so extract it directly via data() after
            // confirming the held type's RTTI name matches
            // RefCountPtr<ITexture>.
            nvrhi::TextureHandle texture;
            bool extracted = false;
            if (auto dt = data.type()) {
                std::string tname(dt.info().name());
                if (tname.find("RefCountPtr") != std::string::npos &&
                    tname.find("ITexture") != std::string::npos) {
                    texture =
                        *static_cast<const nvrhi::TextureHandle*>(data.data());
                    extracted = true;
                }
            }
            if (!extracted) {
                spdlog::warn(
                    "Present node '{}' data is not a TextureHandle",
                    node->ui_name);
                continue;  // Skip invalid textures
            }
            if (!texture) {
                spdlog::warn(
                    "Present node '{}' extracted texture is null (upstream "
                    "render produced empty output)",
                    node->ui_name);
                continue;  // Skip invalid textures
            }

            // Store texture with node's UI name
            std::string texture_name =
                node->ui_name.empty() ? present_name : node->ui_name;
            render_param->presented_textures[texture_name] = texture;

            // Keep backward compatibility: first texture becomes default
            if (render_param->default_texture_name.empty()) {
                render_param->default_texture_name = texture_name;
            }

            // Update render buffer
            auto rb = static_cast<Hd_RUZINO_RenderBuffer*>(
                _aovBindings[i].renderBuffer);
#ifdef RUZINO_DIRECT_VK_DISPLAY
            // Already stored above
#else
            rb->Present(texture);
#endif
            rb->SetConverged(true);
        }

        // Create empty texture if nothing was presented
        if (render_param->default_texture_name.empty()) {
            auto empty_tex = create_empty_texture(
                GfVec2i{ 16, 16 }, nvrhi::Format::RGBA32_FLOAT);
            render_param->presented_textures["_empty"] = empty_tex;
            render_param->default_texture_name = "_empty";
        }
    }

    node_system->finalize();

    // executor->finalize(node_tree);
}

void Hd_RUZINO_Renderer::Clear()
{
    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        if (_aovBindings[i].clearValue.IsEmpty()) {
            continue;
        }

        auto rb =
            static_cast<Hd_RUZINO_RenderBuffer*>(_aovBindings[i].renderBuffer);
        rb->Clear();
    }
}

void Hd_RUZINO_Renderer::SetAovBindings(
    const HdRenderPassAovBindingVector& aovBindings)
{
    _aovBindings = aovBindings;
    _aovNames.resize(_aovBindings.size());
    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        _aovNames[i] = HdParsedAovToken(_aovBindings[i].aovName);
    }

    // Re-validate the attachments.
    _aovBindingsNeedValidation = true;
}

void Hd_RUZINO_Renderer::MarkAovBuffersUnconverged()
{
    for (size_t i = 0; i < _aovBindings.size(); ++i) {
        auto rb =
            static_cast<Hd_RUZINO_RenderBuffer*>(_aovBindings[i].renderBuffer);
        rb->SetConverged(false);
    }
}

void Hd_RUZINO_Renderer::renderTimeUpdateCamera(
    const HdRenderPassStateSharedPtr& renderPassState)
{
    camera_ =
        static_cast<const Hd_RUZINO_Camera*>(renderPassState->GetCamera());
    if (camera_)
        camera_->update(renderPassState);
}

bool Hd_RUZINO_Renderer::nodetree_modified()
{
    //    return render_param->node_tree->GetDirty();
    return false;
}

bool Hd_RUZINO_Renderer::nodetree_modified(bool new_status)
{
    // auto old_status = render_param->node_tree->GetDirty();
    // render_param->node_tree->SetDirty(new_status);
    // return old_status;

    return false;
}

RUZINO_NAMESPACE_CLOSE_SCOPE
