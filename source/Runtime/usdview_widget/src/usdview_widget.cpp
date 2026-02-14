#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include "widgets/usdview/usdview_widget.hpp"

#include <pxr/imaging/hd/driver.h>
#include <spdlog/spdlog.h>

#include <any>

#include "GCore/geom_payload.hpp"
#include "GUI/window.h"
#include "RHI/Hgi/desc_conversion.hpp"
#include "RHI/rhi.hpp"
#include "free_camera.hpp"
#include "imgui.h"
#include "nvrhi/nvrhi.h"
#include "pxr/base/gf/camera.h"
#include "pxr/base/gf/frustum.h"
#include "pxr/imaging/glf/drawTarget.h"
#include "pxr/imaging/hdx/tokens.h"
#include "pxr/imaging/hgi/blitCmds.h"
#include "pxr/imaging/hgi/blitCmdsOps.h"
#include "pxr/imaging/hgi/tokens.h"
#include "pxr/pxr.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/boundable.h"
#include "pxr/usd/usdGeom/camera.h"
#include "pxr/usd/usdGeom/xformable.h"
#include "pxr/usdImaging/usdImagingGL/engine.h"

RUZINO_NAMESPACE_OPEN_SCOPE
class NodeTree;

// Helper function to initialize all required USD camera attributes
static void InitializeCameraAttributes(BaseCamera* camera)
{
    using namespace pxr;
    if (!camera)
        return;

    // Set all required camera attributes to avoid warnings
    camera->CreateFocalLengthAttr().Set(50.0f);
    camera->CreateHorizontalApertureAttr().Set(20.955f);  // Default 35mm equiv
    camera->CreateVerticalApertureAttr().Set(15.2908f);
    camera->CreateHorizontalApertureOffsetAttr().Set(0.0f);
    camera->CreateVerticalApertureOffsetAttr().Set(0.0f);
    camera->CreateClippingRangeAttr().Set(GfVec2f(1.0f, 2000.0f));
    camera->CreateClippingPlanesAttr().Set(VtArray<GfVec4f>());
    camera->CreateFStopAttr().Set(0.0f);
    camera->CreateFocusDistanceAttr().Set(10.0f);
}

struct UsdviewEnginePrivateData {
    nvrhi::TextureHandle nvrhi_texture = nullptr;
    nvrhi::StagingTextureHandle staging = nullptr;
    nvrhi::Format present_format = nvrhi::Format::RGBA32_FLOAT;
};

UsdviewEngine::UsdviewEngine(Stage* stage) : stage_(stage)
{
    data_ = std::make_unique<UsdviewEnginePrivateData>();
    // Initialize OpenGL context using WGL
    CreateGLContext();
    GarchGLApiLoad();
    pxr::UsdImagingGLEngine::Parameters params;
    params.allowAsynchronousSceneProcessing = true;

    hgi = pxr::Hgi::CreateNamedHgi(pxr::HgiTokens->OpenGL);
    pxr::HdDriver hdDriver;
    hdDriver.name = pxr::HgiTokens->renderDriver;
    hdDriver.driver = pxr::VtValue(hgi.get());
    params.driver = hdDriver;

    renderer_ = std::make_unique<pxr::UsdImagingGLEngine>(params);

    renderer_->SetEnablePresentation(false);
    free_camera_ = std::make_unique<ThirdPersonCamera>();

    auto prim = pxr::UsdGeomCamera::Get(
        stage_->get_usd_stage(), pxr::SdfPath("/FreeCamera"));
    if (prim) {
        // Load existing camera
        static_cast<pxr::UsdGeomCamera&>(*free_camera_) = prim;
        // Load third person camera state
        auto* third_camera =
            static_cast<ThirdPersonCamera*>(free_camera_.get());
        third_camera->LoadState();
    }
    else {
        // Create new camera with all required attributes
        static_cast<pxr::UsdGeomCamera&>(*free_camera_) =
            pxr::UsdGeomCamera::Define(
                stage_->get_usd_stage(), pxr::SdfPath("/FreeCamera"));

        InitializeCameraAttributes(free_camera_.get());

        // Initialize third person camera to look at origin
        auto* third_camera =
            static_cast<ThirdPersonCamera*>(free_camera_.get());
        third_camera->LookAt(pxr::GfVec3d{ 5, 5, 5 }, pxr::GfVec3d{ 0, 0, 0 });
        third_camera->SaveState();
    }
    auto plugins = renderer_->GetRendererPlugins();

    ChooseRenderer(plugins, engine_status.renderer_id);

    // Set selection highlight color to bright orange
    renderer_->SetSelectionColor(pxr::GfVec4f(1.0f, 0.7f, 0.0f, 1.0f));
}

void UsdviewEngine::ChooseRenderer(
    const pxr::TfTokenVector& available_renderers,
    unsigned i)
{
    renderer_->SetRendererPlugin(available_renderers[i]);
    spdlog::info(
        "Switching to renderer {}", available_renderers[i].GetString().c_str());

    if (available_renderers[i].GetString() == "Hd_RUZINO_RendererPlugin") {
        renderer_ui_control =
            renderer_->GetRendererSetting(pxr::TfToken("RenderNodeSystem"))
                .Get<const void*>();
    }

    if (available_renderers[i].GetString() == "Hd_RUZINO_GL_RendererPlugin") {
        renderer_ui_control =
            renderer_->GetRendererSetting(pxr::TfToken("RenderNodeSystem"))
                .Get<const void*>();
    }

    renderer_->SetEnablePresentation(false);
    data_->nvrhi_texture = nullptr;

    this->engine_status.renderer_id = i;

    // Set selection color for the new renderer
    renderer_->SetSelectionColor(pxr::GfVec4f(1.0f, 0.7f, 0.0f, 1.0f));
}

std::string UsdviewEngine::GetCurrentRenderer() const
{
    auto plugins = renderer_->GetRendererPlugins();
    return plugins[engine_status.renderer_id].GetString();
}

void UsdviewEngine::DrawMenuBar()
{
    ImGui::BeginMenuBar();
    if (ImGui::BeginMenu("Free Camera")) {
        if (ImGui::BeginMenu("Camera Type")) {
            if (ImGui::MenuItem(
                    "First Personal",
                    0,
                    this->engine_status.cam_type == CamType::First)) {
                if (engine_status.cam_type != CamType::First) {
                    // Save third person state before switching
                    if (engine_status.cam_type == CamType::Third) {
                        auto* third_camera =
                            static_cast<ThirdPersonCamera*>(free_camera_.get());
                        third_camera->SaveState();
                    }

                    // Get current camera state
                    auto cam_prim = free_camera_->GetPrim();
                    auto current_pos = free_camera_->GetPosition();
                    auto current_dir = free_camera_->GetDir();
                    auto current_up = free_camera_->GetUp();
                    double current_speed = 1.0;
                    cam_prim.GetAttribute(pxr::TfToken("move_speed"))
                        .Get(&current_speed);

                    // Switch to first person
                    free_camera_ = std::make_unique<FirstPersonCamera>();
                    static_cast<pxr::UsdGeomCamera&>(*free_camera_) =
                        pxr::UsdGeomCamera(cam_prim);

                    // Preserve position, orientation and speed
                    auto* first_camera =
                        static_cast<FirstPersonCamera*>(free_camera_.get());
                    first_camera->LookTo(current_pos, current_dir, current_up);
                    first_camera->SetMoveSpeed(current_speed);

                    engine_status.cam_type = CamType::First;
                }
            }
            if (ImGui::MenuItem(
                    "Third Personal",
                    0,
                    this->engine_status.cam_type == CamType::Third)) {
                if (engine_status.cam_type != CamType::Third) {
                    // Get current camera state
                    auto cam_prim = free_camera_->GetPrim();
                    auto current_pos = free_camera_->GetPosition();
                    auto current_dir = free_camera_->GetDir();
                    auto current_up = free_camera_->GetUp();
                    double current_speed = 1.0;
                    cam_prim.GetAttribute(pxr::TfToken("move_speed"))
                        .Get(&current_speed);

                    // Try to find a smart target position by raycasting from
                    // screen center
                    using namespace pxr;
                    pxr::GfVec3d target;
                    double distance = 10.0;

                    spdlog::info("Attempting raycast from screen center...");

                    // Raycast from screen center
                    GfVec3d hit_point;
                    GfVec3d hit_normal;
                    SdfPath hit_path;
                    SdfPath instancer;
                    int hit_instance_index;
                    HdInstancerContext instancer_context;

                    // Create narrowed frustum around screen center (NDC 0,0)
                    auto narrowed = cached_frustum_.ComputeNarrowedFrustum(
                        GfVec2d(0.0, 0.0),
                        GfVec2d(
                            1.0 / render_buffer_size_[0],
                            1.0 / render_buffer_size_[1]));

                    UsdPrim root = stage_->get_usd_stage()->GetPseudoRoot();
                    if (renderer_->TestIntersection(
                            narrowed.ComputeViewMatrix(),
                            narrowed.ComputeProjectionMatrix(),
                            root,
                            _renderParams,
                            &hit_point,
                            &hit_normal,
                            &hit_path,
                            &instancer,
                            &hit_instance_index,
                            &instancer_context)) {
                        // Hit something! Use the hit point as target
                        target = hit_point;
                        distance = (hit_point - current_pos).GetLength();
                    }
                    else {
                        // No hit, use default: 10 units in front
                        target = current_pos + current_dir * 10.0;
                    }

                    // Switch to third person
                    free_camera_ = std::make_unique<ThirdPersonCamera>();
                    static_cast<pxr::UsdGeomCamera&>(*free_camera_) =
                        pxr::UsdGeomCamera(cam_prim);

                    auto* third_camera =
                        static_cast<ThirdPersonCamera*>(free_camera_.get());

                    // Set target and distance based on raycast result
                    third_camera->SetTargetPosition(target);
                    third_camera->SetDistance(distance);

                    // Calculate yaw and pitch from current direction
                    double azimuth, elevation, length;
                    third_camera->CartesianToSpherical(
                        -current_dir, azimuth, elevation, length);
                    third_camera->SetRotation(azimuth, elevation);

                    // Preserve move speed
                    third_camera->SetMoveSpeed(current_speed);

                    // Save this new state
                    third_camera->SaveState();

                    engine_status.cam_type = CamType::Third;
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Renderer")) {
        if (ImGui::BeginMenu("Select Renderer")) {
            auto available_renderers = renderer_->GetRendererPlugins();
            for (unsigned i = 0; i < available_renderers.size(); ++i) {
                if (ImGui::MenuItem(
                        available_renderers[i].GetText(),
                        0,
                        this->engine_status.renderer_id == i)) {
                    if (this->engine_status.renderer_id != i) {
                        ChooseRenderer(available_renderers, i);
                        renderer_->SetRenderBufferSize(render_buffer_size_);
                        renderer_->SetRenderViewport(
                            pxr::GfVec4d{ 0.0,
                                          0.0,
                                          double(render_buffer_size_[0]),
                                          double(render_buffer_size_[1]) });
                    }
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Gizmo")) {
        ImGui::Text("Operation");
        if (ImGui::RadioButton(
                "Translate (T)", gizmo_operation_ == ImGuizmo::TRANSLATE)) {
            gizmo_operation_ = ImGuizmo::TRANSLATE;
        }
        if (ImGui::RadioButton(
                "Rotate (E)", gizmo_operation_ == ImGuizmo::ROTATE)) {
            gizmo_operation_ = ImGuizmo::ROTATE;
        }
        if (ImGui::RadioButton(
                "Scale (R)", gizmo_operation_ == ImGuizmo::SCALE)) {
            gizmo_operation_ = ImGuizmo::SCALE;
        }
        ImGui::Separator();

        if (gizmo_operation_ != ImGuizmo::SCALE) {
            ImGui::Text("Mode");
            if (ImGui::RadioButton("Local", gizmo_mode_ == ImGuizmo::LOCAL)) {
                gizmo_mode_ = ImGuizmo::LOCAL;
            }
            if (ImGui::RadioButton("World", gizmo_mode_ == ImGuizmo::WORLD)) {
                gizmo_mode_ = ImGuizmo::WORLD;
            }
            ImGui::Separator();
        }

        ImGui::Checkbox("Use Snap", &gizmo_use_snap_);
        if (gizmo_use_snap_) {
            switch (gizmo_operation_) {
                case ImGuizmo::TRANSLATE:
                    ImGui::InputFloat3("Snap", gizmo_snap_);
                    break;
                case ImGuizmo::ROTATE:
                    ImGui::InputFloat("Angle Snap", &gizmo_snap_[0]);
                    break;
                case ImGuizmo::SCALE:
                    ImGui::InputFloat("Scale Snap", &gizmo_snap_[0]);
                    break;
            }
        }

        ImGui::Separator();
        ImGui::SliderFloat(
            "View Manipulator Size", &view_manipulate_size_, 64.0f, 256.0f);

        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

void UsdviewEngine::copy_to_presentation()
{
    // Since Hgi and nvrhi vulkan are on different Vulkan instances and we
    // don't
    // want to modify Hgi's external information definition, we need to do a
    // CPU read back to send the information to nvrhi.

    auto hgi_texture = renderer_->GetAovTexture(pxr::HdAovTokens->color);
    if (hgi_texture) {
        auto hgi_desc = hgi_texture->GetDescriptor();
        nvrhi::TextureDesc tex_desc = RHI::ConvertToNvrhiTextureDesc(hgi_desc);
        tex_desc.keepInitialState = true;
        tex_desc.initialState = nvrhi::ResourceStates::CopyDest;

        // Calculate buffer size based on actual format
        size_t bytes_per_pixel =
            RHI::calculate_bytes_per_pixel(tex_desc.format);
        size_t required_size =
            hgi_desc.dimensions[0] * hgi_desc.dimensions[1] * bytes_per_pixel;

        if (texture_data_.size() != required_size) {
            texture_data_.resize(required_size);
            data_->nvrhi_texture =
                nullptr;  // Force recreation with correct size
        }

        pxr::HgiBlitCmdsUniquePtr blitCmds = hgi->CreateBlitCmds();
        pxr::HgiTextureGpuToCpuOp copyOp;
        copyOp.gpuSourceTexture = hgi_texture;
        copyOp.cpuDestinationBuffer = texture_data_.data();
        copyOp.destinationBufferByteSize = texture_data_.size();
        blitCmds->CopyTextureGpuToCpu(copyOp);

        hgi->SubmitCmds(
            blitCmds.get(), pxr::HgiSubmitWaitTypeWaitUntilCompleted);
        if (!data_->nvrhi_texture) {
            std::tie(data_->nvrhi_texture, data_->staging) =
                RHI::load_texture(tex_desc, texture_data_.data());
        }
        else {
            RHI::write_texture(
                data_->nvrhi_texture.Get(),
                data_->staging.Get(),
                texture_data_.data());
        }
    }
}

void UsdviewEngine::OnFrame(float delta_time)
{
    if (first_draw) {
        first_draw = false;
        return;
    }

    // Handle Gizmo operation shortcuts (T, E, R)
    if (is_active) {
        if (ImGui::IsKeyPressed(ImGuiKey_T)) {
            gizmo_operation_ = ImGuizmo::TRANSLATE;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_E)) {
            gizmo_operation_ = ImGuizmo::ROTATE;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_R)) {
            gizmo_operation_ = ImGuizmo::SCALE;
        }
    }

    DrawMenuBar();

    auto previous = data_->nvrhi_texture.Get();

    using namespace pxr;

    // Use the current render time for camera query
    UsdTimeCode current_render_time = stage_->get_render_time();

    GfFrustum frustum =
        free_camera_->GetCamera(current_render_time).GetFrustum();

    double fov, aspect_ratio, near_distance, far_distance;

    frustum.GetPerspective(&fov, &aspect_ratio, &near_distance, &far_distance);

    frustum.SetPerspective(
        fov,
        float(render_buffer_size_[0]) / float(render_buffer_size_[1]),
        near_distance,
        far_distance);

    GfMatrix4d projectionMatrix = frustum.ComputeProjectionMatrix();
    GfMatrix4d viewMatrix = frustum.ComputeViewMatrix();

    // Cache frustum for raycast during camera switching
    cached_frustum_ = frustum;

    renderer_->SetCameraState(viewMatrix, projectionMatrix);

    // Update third person camera's view information (for proper panning)
    if (engine_status.cam_type == CamType::Third) {
        auto* third_camera =
            static_cast<ThirdPersonCamera*>(free_camera_.get());
        third_camera->SetView(frustum);

        // Update cache for Inspector delta calculation
        cached_camera_pos_ = third_camera->GetPosition();
        cached_target_pos_ = third_camera->GetTargetPosition();
        camera_state_cached_ = true;
    }

    _renderParams.enableLighting = true;
    _renderParams.enableSceneMaterials = true;
    _renderParams.showRender = true;
    _renderParams.highlight = true;  // CRITICAL: Enable selection highlighting!

    // Ensure we render using the current stage time, match what the Gizmo is
    // using Re-use variable defined above
    if (current_render_time.IsDefault())
        _renderParams.frame = pxr::UsdTimeCode::Default();
    else
        _renderParams.frame = current_render_time;

    _renderParams.drawMode = UsdImagingGLDrawMode::DRAW_WIREFRAME_ON_SURFACE;
    _renderParams.colorCorrectionMode = pxr::HdxColorCorrectionTokens->disabled;

    _renderParams.clearColor = GfVec4f(1.f, 1.f, 1.f, 1.f);
    // _renderParams.clearColor = GfVec4f(0.2f, 0.2f, 0.2f, 1.f);

    for (int i = 0; i < free_camera_->GetCamera(current_render_time)
                            .GetClippingPlanes()
                            .size();
         ++i) {
        _renderParams.clipPlanes[i] =
            free_camera_->GetCamera(current_render_time).GetClippingPlanes()[i];
    }

    GlfSimpleLightVector lights;

    if (engine_status.renderer_id == 0) {
        lights = GlfSimpleLightVector(1);
        auto cam_pos = frustum.GetPosition();
        lights[0].SetPosition(
            GfVec4f{ float(cam_pos[0]),
                     float(cam_pos[1]),
                     float(cam_pos[2]),
                     1.0f });
        lights[0].SetAmbient(GfVec4f(0.8, 0.8, 0.8, 1));
        lights[0].SetDiffuse(GfVec4f(1.0f));
        lights[0].SetSpecular(GfVec4f(0.0f));
    }
    GlfSimpleMaterial material;
    float kA = 6.8f;
    float kS = 0.4f;
    float shiness = 0.8f;
    material.SetDiffuse(GfVec4f(kA, kA, kA, 1.0f));
    material.SetSpecular(GfVec4f(kS, kS, kS, 1.0f));
    material.SetShininess(shiness);
    GfVec4f sceneAmbient = { 1.0, 1.0, 1.0, 1.0 };
    renderer_->SetLightingState(lights, material, sceneAmbient);
    renderer_->SetRendererAov(HdAovTokens->color);

    for (auto&& setting : settings) {
        renderer_->SetRendererSetting(setting.first, setting.second);
    }

    UsdPrim root = stage_->get_usd_stage()->GetPseudoRoot();

    // First try is there a hack?
    renderer_->Render(root, _renderParams);

    auto imgui_frame_size =
        ImVec2(render_buffer_size_[0], render_buffer_size_[1]);

    ImGui::BeginChild("ViewPort", imgui_frame_size, 0, ImGuiWindowFlags_NoMove);

    if (data_->nvrhi_texture.Get()) {
        persistent_texture = data_->nvrhi_texture;
        ImGui::Image(
            static_cast<ImTextureID>(persistent_texture.Get()),
            imgui_frame_size,
            ImVec2(0.0f, 1.0f),
            ImVec2(1.0f, 0.0f));
    }
    // else {
    //     spdlog::warn("No image!");
    // }
    is_active = ImGui::IsWindowFocused();
    is_hovered = ImGui::IsItemHovered();

    // Save the viewport rect for Gizmo and ViewManipulate
    ImVec2 viewport_pos = ImGui::GetItemRectMin();
    ImVec2 viewport_size = ImGui::GetItemRectSize();

    // Set ImGuizmo rect early so IsOver/IsViewManipulateHovered checks use
    // correct coordinates
    ImGuizmo::SetRect(
        viewport_pos.x, viewport_pos.y, viewport_size.x, viewport_size.y);

    // Prevent picking if Gizmo is being hovered/used
    bool gizmo_hovered =
        ImGuizmo::IsOver() || ImGuizmo::IsViewManipulateHovered();

    // Key Fix: If we are hovering the viewport but NOT the gizmo/manipulator,
    // tell ImGui we don't want to capture the mouse. This allows the
    // application to process picking (which usually checks !WantCaptureMouse).
    // If we ARE hovering the gizmo, we leave WantCaptureMouse as true (default
    // for window) so ImGui/ImGuizmo handles the input.
    if (is_hovered && !gizmo_hovered) {
        ImGui::GetIO().WantCaptureMouse = false;
    }

    if (gizmo_hovered) {
        if (left_mouse_pressed)
            spdlog::info("Picking blocked because Gizmo is hovered");
        left_mouse_pressed = false;
    }

    // Left click picking
    if (left_mouse_pressed && is_hovered) {
        spdlog::warn("Executing Raycast Picking Logic in OnFrame...");
        left_mouse_pressed = false;  // Reset flag

        auto mouse_pos_rel = ImGui::GetMousePos() - ImGui::GetItemRectMin();

        spdlog::warn(
            "Mouse Position Relative: (%.2f, %.2f)",
            mouse_pos_rel.x,
            mouse_pos_rel.y);
        // Normalize the mouse position to be in the range [0, 1]
        ImVec2 mousePosNorm = ImVec2(
            mouse_pos_rel.x / render_buffer_size_[0],
            mouse_pos_rel.y / render_buffer_size_[1]);

        // Convert to NDC coordinates
        ImVec2 mousePosNDC =
            ImVec2(mousePosNorm.x * 2.0f - 1.0f, 1.0f - mousePosNorm.y * 2.0f);

        spdlog::info(
            "Mouse Position NDC: (%.2f, %.2f)", mousePosNDC.x, mousePosNDC.y);

        using namespace pxr;
        UsdPrim root = stage_->get_usd_stage()->GetPseudoRoot();

        GfVec3d point;
        GfVec3d normal;
        SdfPath path;
        SdfPath instancer;
        HdInstancerContext outInstancerContext;
        int outHitInstanceIndex;

        auto narrowed = frustum.ComputeNarrowedFrustum(
            { mousePosNDC[0], mousePosNDC[1] },
            { 1.0 / render_buffer_size_[0], 1.0 / render_buffer_size_[1] });
        if (renderer_->TestIntersection(
                narrowed.ComputeViewMatrix(),
                narrowed.ComputeProjectionMatrix(),
                root,
                _renderParams,
                &point,
                &normal,
                &path,
                &instancer,
                &outHitInstanceIndex,
                &outInstancerContext)) {
            // Create and store the pick event

#ifdef GEOM_USD_EXTENSION
            current_pick_event_ = std::make_shared<PickEvent>(
                glm::vec3(point[0], point[1], point[2]),
                glm::vec3(normal[0], normal[1], normal[2]),
                path,
                instancer);
#endif

            spdlog::info("Picked prim " + path.GetAsString());
            spdlog::info(
                "Picked point: (%.2f, %.2f, %.2f)",
                point[0],
                point[1],
                point[2]);

            // Emit viewport pick event to notify UsdFileViewer
            if (window) {
                window->events().emit_any("viewport_prim_picked", path);
            }

            // Also update our own selection
            on_prim_selected(path);
        }
        else {
            // Clicked on nothing (infinity), clear selection
            on_prim_selected(pxr::SdfPath());
            if (window) {
                window->events().emit_any(
                    "viewport_prim_picked", pxr::SdfPath());
            }
        }
    }

    // Draw ViewManipulate first (always visible)
    DrawViewManipulate(viewport_pos, viewport_size);

    // Draw Gizmo for selected object
    DrawGizmo(viewport_pos, viewport_size);

    // Update bounding box for selected prim to match current animation
    // time/transform
    if (!current_selected_path_.IsEmpty()) {
        auto prim =
            stage_->get_usd_stage()->GetPrimAtPath(current_selected_path_);
        if (prim && prim.IsA<pxr::UsdGeomBoundable>()) {
            pxr::UsdGeomBoundable boundable(prim);
            pxr::VtArray<pxr::GfVec3f> extent;
            if (boundable.GetExtentAttr().Get(&extent) && extent.size() == 2) {
                pxr::GfRange3d range(
                    pxr::GfVec3d(extent[0][0], extent[0][1], extent[0][2]),
                    pxr::GfVec3d(extent[1][0], extent[1][1], extent[1][2]));

                pxr::UsdGeomXformable xformable(prim);
                pxr::GfMatrix4d xform;
                bool reset;
                // Use RENDER time to match viewport
                xformable.GetLocalTransformation(
                    &xform, &reset, stage_->get_render_time());

                // Update the first bbox (assuming single selection for now)
                if (!_renderParams.bboxes.empty()) {
                    _renderParams.bboxes[0] = pxr::GfBBox3d(range, xform);
                }
                else {
                    _renderParams.bboxes.push_back(pxr::GfBBox3d(range, xform));
                }
            }
        }
    }

    ImGui::EndChild();
    time_controller();
}

void UsdviewEngine::time_controller()
{
    timecode = stage_->get_render_time().GetValue();

    if (is_active && ImGui::IsKeyReleased(ImGuiKey_Space)) {
        playing = !playing;
    }
    if (playing) {
        timecode += 1.0f / 60.f;

        if (timecode > time_code_max) {
            timecode = 0;
        }
        stage_->set_render_time(timecode);
    }

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::SliderFloat("##timecode", &timecode, 0, time_code_max)) {
        stage_->set_render_time(timecode);
    }

    // TODO:  1.加个显示当前仿真进度的进度条 (current_time)
}

bool UsdviewEngine::JoystickButtonUpdate(int button, bool pressed)
{
    free_camera_->JoystickButtonUpdate(button, pressed);
    return false;
}

bool UsdviewEngine::JoystickAxisUpdate(int axis, float value)
{
    free_camera_->JoystickUpdate(axis, value);
    return false;
}

bool UsdviewEngine::KeyboardUpdate(int key, int scancode, int action, int mods)
{
    if (is_active) {
        free_camera_->KeyboardUpdate(key, scancode, action, mods);
    }
    return false;
}

bool UsdviewEngine::MousePosUpdate(double xpos, double ypos)
{
    free_camera_->MousePosUpdate(xpos, ypos);
    return false;
}

bool UsdviewEngine::MouseScrollUpdate(double xoffset, double yoffset)
{
    if (is_active && is_hovered) {
        free_camera_->MouseScrollUpdate(xoffset, yoffset);
    }
    return false;
}

bool UsdviewEngine::MouseButtonUpdate(int button, int action, int mods)
{
    ImGuiIO& io = ImGui::GetIO();

    // If ImGui wants the mouse (including ImGuizmo), don't process picking
    if (io.WantCaptureMouse)
        return false;

    bool shift_pressed = io.KeyShift;

    // Left button: picking/selection only
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS &&
        is_hovered) {
        // Trigger picking
        left_mouse_pressed = true;
        return false;
    }
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
        left_mouse_pressed = false;
        return false;
    }

    // Right button: clear selection
    if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS &&
        is_hovered) {
        // Clear selection
        on_prim_selected(pxr::SdfPath());  // Empty path clears selection
        if (window) {
            window->events().emit_any("viewport_prim_picked", pxr::SdfPath());
        }
        return false;
    }

    // Middle button: camera control (orbit or pan based on Shift)
    if (button == GLFW_MOUSE_BUTTON_MIDDLE && is_hovered) {
        free_camera_->MouseButtonUpdate(button, action, mods);
        return false;
    }

    return false;
}

void UsdviewEngine::Animate(float elapsed_time_seconds)
{
    // Ensure camera uses the correct time code for any transform updates
    if (free_camera_) {
        free_camera_->SetCurrentTime(stage_->get_render_time());
    }

    free_camera_->Animate(elapsed_time_seconds);

    // Notify Inspector only if there was actual user interaction
    // This clears the cached Euler angles so rotation values update in UI
    if (engine_status.cam_type == CamType::Third) {
        if (free_camera_->HadInteractionLastFrame() && window) {
            window->events().emit_any("camera_transform_modified", std::any());
        }
    }

    IWidget::Animate(elapsed_time_seconds);
}

void UsdviewEngine::CreateGLContext()
{
#ifdef _WIN32
    HDC hdc = GetDC(GetConsoleWindow());
    PIXELFORMATDESCRIPTOR pfd;
    ZeroMemory(&pfd, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;

    int pixelFormat = ChoosePixelFormat(hdc, &pfd);
    SetPixelFormat(hdc, pixelFormat, &pfd);

    HGLRC hglrc = wglCreateContext(hdc);
    wglMakeCurrent(hdc, hglrc);
#endif
}

UsdviewEngine::~UsdviewEngine()
{
    // Save camera state before destruction
    if (engine_status.cam_type == CamType::Third && free_camera_) {
        auto* third_camera =
            static_cast<ThirdPersonCamera*>(free_camera_.get());
        third_camera->SaveState();
    }

    command_list_ = nullptr;
    data_.reset();
    assert(RHI::get_device());
    renderer_.reset();
    hgi.reset();
}

bool UsdviewEngine::BuildUI()
{
    // Initialize ImGuizmo for this frame
    ImGuizmo::BeginFrame();

    // Subscribe to selection events on first frame
    subscribe_to_selection_events();

    // Subscribe to camera transform modification events
    subscribe_to_camera_transform_events();

    auto delta_time = ImGui::GetIO().DeltaTime;

    if (size_changed) {
        auto size = ImGui::GetContentRegionAvail();
        if (size.y > 26)
            size.y -= 26;
        RenderBackBufferResized(size.x, size.y);
    }

    if (render_buffer_size_[0] > 0 && render_buffer_size_[1] > 0) {
        OnFrame(delta_time);
    }

    return true;
}

void UsdviewEngine::SetEditMode(bool editing)
{
    is_editing_ = editing;
}

const void* UsdviewEngine::emit_create_renderer_ui_control()
{
    auto temp = renderer_ui_control;
    renderer_ui_control = nullptr;
    return temp;
}

pxr::VtValue UsdviewEngine::get_renderer_setting(const pxr::TfToken& id) const
{
    return renderer_->GetRendererSetting(id);
}

void UsdviewEngine::set_renderer_setting(
    const pxr::TfToken& id,
    const pxr::VtValue& value)
{
    settings[id] = value;
    renderer_->SetRendererSetting(id, value);
}

void UsdviewEngine::finish_render()
{
    if (!renderer_) {
        return;
    }

    renderer_->StopRenderer();
    auto hacked_handle =
        renderer_->GetRendererSetting(pxr::TfToken("VulkanColorAov"));

    if (hacked_handle.IsHolding<const void*>()) {
        auto rendered = *reinterpret_cast<const nvrhi::TextureHandle*>(
            hacked_handle.Get<const void*>());
        if (rendered) {
            if (!command_list_) {
                command_list_ = RHI::get_device()->createCommandList();
            }
            RHI::copy_from_texture(
                data_->nvrhi_texture, rendered, command_list_.Get());
        }
        RHI::get_device()->waitForIdle();
        RHI::get_device()->runGarbageCollection();
    }
    else {
        copy_to_presentation();
    }

    // Also retrieve TLAS if needed
    auto tlas_handle =
        renderer_->GetRendererSetting(pxr::TfToken("VulkanTLAS"));
    if (tlas_handle.IsHolding<const void*>()) {
        auto tlas_ptr = tlas_handle.Get<const void*>();
        auto tlas =
            static_cast<nvrhi::rt::IAccelStruct*>(const_cast<void*>(tlas_ptr));
        // TLAS can now be used here if needed
        if (tlas) {
            spdlog::debug("Successfully retrieved TLAS from renderer");
        }
    }
}

ImGuiWindowFlags UsdviewEngine::GetWindowFlag()
{
    return ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoCollapse |
           ImGuiWindowFlags_NoScrollbar;
}

const char* UsdviewEngine::GetWindowName()
{
    return "UsdView Engine";
}

std::string UsdviewEngine::GetWindowUniqueName()
{
    return "Hydra Renderer";
}

void UsdviewEngine::RenderBackBufferResized(float x, float y)
{
    render_buffer_size_[0] = x;
    render_buffer_size_[1] = y;

    renderer_->SetRenderBufferSize(render_buffer_size_);
    renderer_->SetRenderViewport(
        pxr::GfVec4d{ 0.0,
                      0.0,
                      double(render_buffer_size_[0]),
                      double(render_buffer_size_[1]) });

    data_->nvrhi_texture = nullptr;
    data_->staging = nullptr;
    texture_data_.resize(
        render_buffer_size_[0] * render_buffer_size_[1] *
        RHI::calculate_bytes_per_pixel(data_->present_format));
}

std::shared_ptr<PickEvent> UsdviewEngine::consume_pick_event()
{
    auto event = current_pick_event_;
    current_pick_event_ = nullptr;
    return event;
}
void UsdviewEngine::subscribe_to_selection_events()
{
    if (!selection_event_subscribed_ && window) {
        selection_event_subscribed_ = true;
        window->events().subscribe_any(
            "prim_selected", [this](const std::any& event_data) {
                try {
                    const auto& path = std::any_cast<pxr::SdfPath>(event_data);
                    on_prim_selected(path);
                }
                catch (const std::bad_any_cast&) {
                    // Silently ignore invalid event data
                }
            });
    }
}

void UsdviewEngine::subscribe_to_camera_transform_events()
{
    if (!camera_transform_event_subscribed_ && window) {
        camera_transform_event_subscribed_ = true;
        window->events().subscribe_any(
            "camera_transform_modified", [this](const std::any& event_data) {
                on_camera_transform_modified();
            });
    }
}

void UsdviewEngine::on_camera_transform_modified()
{
    if (free_camera_) {
        bool isThirdPerson = (engine_status.cam_type == CamType::Third);

        // Ensure we are reading from the correct time
        free_camera_->SetCurrentTime(stage_->get_render_time());

        // Reload camera state from USD prim (this reads Inspector's
        // modifications) Inspector only modifies translation, rotation stays
        // the same
        free_camera_->ReloadFromUsd();

        // For third person camera, synchronize spherical coords to match the
        // new Matrix
        if (isThirdPerson) {
            auto* third_camera =
                static_cast<ThirdPersonCamera*>(free_camera_.get());

            // 1. Get the new Cartesian state from BaseCamera (populated by
            // ReloadFromUsd)
            pxr::GfVec3d newPos = third_camera->GetPosition();
            pxr::GfVec3d newDir = third_camera->GetDir();  // View direction

            // 2. Read stored Distance from USD (Matrix loses distance info)
            double savedDistance = 10.0;
            auto prim = third_camera->GetPrim();
            if (prim) {
                auto distAttr =
                    prim.GetAttribute(pxr::TfToken("third_person:distance"));
                if (distAttr) {
                    double d;
                    distAttr.Get(&d);
                    if (d > 0.1)
                        savedDistance = d;
                }
            }

            // 3. Recalculate Target and Rotation from Pos/Dir/Dist
            // Target = Pos + Dir * Dist
            pxr::GfVec3d newTargetPos = newPos + newDir * savedDistance;

            // Offset Direction (Target -> Camera) = -Dir
            pxr::GfVec3d offsetDir = -newDir;

            // Pitch = asin(z)
            double newPitch =
                std::asin(std::max(-1.0, std::min(1.0, offsetDir[2])));

            // Yaw = atan2(y, x)
            double newYaw = std::atan2(offsetDir[1], offsetDir[0]);

            // 4. Update Internal State & Write back to USD attributes
            third_camera->SetTargetPosition(newTargetPos);
            third_camera->SetDistance(savedDistance);
            third_camera->SetRotation(newYaw, newPitch);

            // Write the derived spherical coords back to USD so they match the
            // Matrix
            third_camera->SaveState();

            // Update cache for next time
            cached_camera_pos_ = third_camera->GetPosition();
            cached_target_pos_ = third_camera->GetTargetPosition();
            camera_state_cached_ = true;
        }
    }
}

void UsdviewEngine::on_prim_selected(const pxr::SdfPath& path)
{
    current_selected_path_ = path;

    // Highlight the selected prim in the viewport
    pxr::SdfPathVector selected_paths;
    if (!path.IsEmpty()) {
        selected_paths.push_back(path);
    }
    renderer_->SetSelected(selected_paths);

    // Show bounding box for selected prim
    pxr::UsdImagingGLRenderParams::BBoxVector bboxes;
    if (!path.IsEmpty()) {
        auto prim = stage_->get_usd_stage()->GetPrimAtPath(path);
        if (prim && prim.IsA<pxr::UsdGeomBoundable>()) {
            pxr::UsdGeomBoundable boundable(prim);
            pxr::VtArray<pxr::GfVec3f> extent;
            if (boundable.GetExtentAttr().Get(&extent) && extent.size() == 2) {
                // Create bounding box from extent
                pxr::GfRange3d range(
                    pxr::GfVec3d(extent[0][0], extent[0][1], extent[0][2]),
                    pxr::GfVec3d(extent[1][0], extent[1][1], extent[1][2]));

                // Get world transform
                pxr::UsdGeomXformable xformable(prim);
                pxr::GfMatrix4d xform;
                bool reset;
                xformable.GetLocalTransformation(
                    &xform, &reset, stage_->get_current_time());

                bboxes.push_back(pxr::GfBBox3d(range, xform));
            }
        }
    }

    _renderParams.bboxes = bboxes;
    _renderParams.bboxLineColor =
        pxr::GfVec4f(1.0f, 0.7f, 0.0f, 1.0f);  // Orange
    _renderParams.bboxLineDashSize = 3.0f;
}

void UsdviewEngine::DrawGizmo(
    const ImVec2& viewport_pos,
    const ImVec2& viewport_size)
{
    using namespace pxr;

    // Only show gizmo if there's a selected object
    if (current_selected_path_.IsEmpty())
        return;

    auto prim = stage_->get_usd_stage()->GetPrimAtPath(current_selected_path_);
    if (!prim || !prim.IsA<UsdGeomXformable>())
        return;

    UsdGeomXformable xformable(prim);

    // Get world transform matrix using the RENDER time (not necessarily
    // Default)
    UsdTimeCode current_time = stage_->get_render_time();
    GfMatrix4d object_matrix =
        xformable.ComputeLocalToWorldTransform(current_time);

    // Get camera matrices
    GfFrustum frustum = free_camera_->GetCamera(current_time).GetFrustum();

    // Maintain correct aspect ratio for Gizmo projection
    double fov, aspect_ratio, near_distance, far_distance;
    frustum.GetPerspective(&fov, &aspect_ratio, &near_distance, &far_distance);
    frustum.SetPerspective(
        fov,
        float(render_buffer_size_[0]) / float(render_buffer_size_[1]),
        near_distance,
        far_distance);

    GfMatrix4d viewMatrix = frustum.ComputeViewMatrix();
    GfMatrix4d projectionMatrix = frustum.ComputeProjectionMatrix();

    // Convert to float arrays for ImGuizmo
    float view[16], projection[16], matrix[16];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            view[i * 4 + j] = static_cast<float>(viewMatrix[i][j]);
            projection[i * 4 + j] = static_cast<float>(projectionMatrix[i][j]);
            matrix[i * 4 + j] = static_cast<float>(object_matrix[i][j]);
        }
    }

    // Set ImGuizmo rect to match the viewport
    ImGuizmo::SetRect(
        viewport_pos.x, viewport_pos.y, viewport_size.x, viewport_size.y);
    ImGuizmo::SetDrawlist();
    ImGuizmo::Enable(true);

    // Draw the gizmo
    ImGuizmo::Manipulate(
        view,
        projection,
        gizmo_operation_,
        gizmo_mode_,
        matrix,
        nullptr,
        gizmo_use_snap_ ? gizmo_snap_ : nullptr);

    // If the gizmo was manipulated, update the USD transform
    if (ImGuizmo::IsUsing()) {
        // Convert back to GfMatrix4d
        GfMatrix4d new_world_matrix;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                new_world_matrix[i][j] = matrix[i * 4 + j];
            }
        }

        // Convert world space transform to local space
        GfMatrix4d parent_world_matrix(1.0);
        UsdPrim parent = prim.GetParent();
        if (parent && parent.IsA<UsdGeomXformable>()) {
            UsdGeomXformable parent_xformable(parent);
            parent_world_matrix =
                parent_xformable.ComputeLocalToWorldTransform(current_time);
        }

        GfMatrix4d local_matrix =
            new_world_matrix * parent_world_matrix.GetInverse();

        // Optimize: Check if we can update an existing TransformOp instead of
        // clearing/recreating This prevents massive schema churn and helps with
        // "drifting" (update lag)
        bool op_set = false;
        bool resets = false;
        std::vector<UsdGeomXformOp> ops = xformable.GetOrderedXformOps(&resets);

        if (ops.size() == 1 &&
            ops[0].GetOpType() == UsdGeomXformOp::TypeTransform) {
            spdlog::warn(
                "Setting Transform Op at TimeCode: {}",
                current_time.GetValue());
            ops[0].Set(local_matrix, current_time);
            op_set = true;
        }
        else {
            // If complex stack, fallback to collapse strategy
            xformable.ClearXformOpOrder();
            UsdGeomXformOp transformOp = xformable.MakeMatrixXform();
            spdlog::warn(
                "Setting New Transform Op at TimeCode: {}",
                current_time.GetValue());
            transformOp.Set(local_matrix, current_time);
            op_set = true;
        }

        // Notify that transform was modified via Gizmo
        if (op_set && window) {
            window->events().emit_any("camera_transform_modified", std::any());
        }
    }
}

void UsdviewEngine::DrawViewManipulate(
    const ImVec2& viewport_pos,
    const ImVec2& viewport_size)
{
    using namespace pxr;

    // Get camera matrices using current render time
    UsdTimeCode current_time = stage_->get_render_time();
    GfFrustum frustum = free_camera_->GetCamera(current_time).GetFrustum();
    GfMatrix4d viewMatrix = frustum.ComputeViewMatrix();

    // Convert to float array for ImGuizmo
    float view[16];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            view[i * 4 + j] = static_cast<float>(viewMatrix[i][j]);
        }
    }

    // Place the view manipulator in top-right corner of viewport
    float view_manipulate_right = viewport_pos.x + viewport_size.x;
    float view_manipulate_top = viewport_pos.y;

    // Calculate camera distance for ViewManipulate
    GfVec3d cam_pos = free_camera_->GetPosition();
    GfVec3d target_pos(0, 0, 0);
    if (engine_status.cam_type == CamType::Third) {
        auto* third_camera =
            static_cast<ThirdPersonCamera*>(free_camera_.get());
        target_pos = third_camera->GetTargetPosition();
    }
    else {
        // For first person, use a point in front of the camera
        target_pos = cam_pos + free_camera_->GetDir() * 10.0;
    }
    float distance = static_cast<float>((target_pos - cam_pos).GetLength());

    // Setup ImGuizmo environment for this draw call
    ImGuizmo::SetRect(
        viewport_pos.x, viewport_pos.y, viewport_size.x, viewport_size.y);
    ImGuizmo::SetDrawlist();

    // Draw the view manipulator in the top-right corner (using 0x00000000 for
    // transparent background) 0x10101010 was very faint. 0x00000000 is fully
    // transparent background.
    ImGuizmo::ViewManipulate(
        view,
        distance,
        ImVec2(
            view_manipulate_right - view_manipulate_size_ - 10,
            view_manipulate_top + 10),
        ImVec2(view_manipulate_size_, view_manipulate_size_),
        0x00000000);

    // If the view was manipulated, update the camera
    if (ImGuizmo::IsUsingViewManipulate()) {
        // Convert back to GfMatrix4d
        GfMatrix4d new_view_matrix;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                new_view_matrix[i][j] = view[i * 4 + j];
            }
        }

        // Extract camera orientation from view matrix
        // View matrix transforms from world to camera space
        // We need to invert it to get camera-to-world transform
        GfMatrix4d camera_matrix = new_view_matrix.GetInverse();

        // Extract position and direction from camera matrix
        GfVec3d new_cam_pos(
            camera_matrix[3][0], camera_matrix[3][1], camera_matrix[3][2]);
        GfVec3d new_cam_dir(
            -camera_matrix[2][0], -camera_matrix[2][1], -camera_matrix[2][2]);
        GfVec3d new_cam_up(
            camera_matrix[1][0], camera_matrix[1][1], camera_matrix[1][2]);

        new_cam_dir.Normalize();
        new_cam_up.Normalize();

        // Update camera based on type
        if (engine_status.cam_type == CamType::Third) {
            auto* third_camera =
                static_cast<ThirdPersonCamera*>(free_camera_.get());
            GfVec3d target = third_camera->GetTargetPosition();

            // Set new camera position while keeping target
            double new_distance = (new_cam_pos - target).GetLength();
            third_camera->SetDistance(new_distance);

            // Calculate azimuth and elevation from direction
            GfVec3d to_camera = (new_cam_pos - target).GetNormalized();
            double azimuth, elevation, length;
            third_camera->CartesianToSpherical(
                -to_camera, azimuth, elevation, length);
            third_camera->SetRotation(azimuth, elevation);

            // Force update of internal matrices and USD transform
            third_camera->Animate(
                0.0);  // Recalculate position/matrices based on new rotation
            third_camera->UpdateUsdTransform();  // Write to USD
            third_camera->SaveState();           // Write extra attributes
        }
        else {
            // For first person camera
            auto* first_camera =
                static_cast<FirstPersonCamera*>(free_camera_.get());
            first_camera->LookTo(new_cam_pos, new_cam_dir, new_cam_up);
            first_camera->UpdateUsdTransform();  // Ensure we write back to USD
        }

        // Notify changes
        if (window) {
            window->events().emit_any("camera_transform_modified", std::any());
        }
    }
}

RUZINO_NAMESPACE_CLOSE_SCOPE
