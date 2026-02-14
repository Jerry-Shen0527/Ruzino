#pragma once

#include <memory>
#include <string>

#include "GUI/ImGuizmo.h"
#include "GUI/widget.h"
#include "nvrhi/nvrhi.h"
#include "pxr/base/tf/token.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usdImaging/usdImagingGL/engine.h"
#include "stage/stage.hpp"
#include "widgets/api.h"

struct PickEvent;
RUZINO_NAMESPACE_OPEN_SCOPE
class BaseCamera;
class FreeCamera;
class NodeTree;

struct UsdviewEnginePrivateData;

class USDVIEW_WIDGET_API UsdviewEngine final : public IWidget {
   public:
    explicit UsdviewEngine(Stage* stage);
    void ChooseRenderer(
        const pxr::TfTokenVector& available_renderers,
        unsigned i);

    std::string GetCurrentRenderer() const;
    ~UsdviewEngine() override;
    bool BuildUI() override;
    void SetEditMode(bool editing);
    bool borderless() override
    {
        return true;
    }

    const void* emit_create_renderer_ui_control();
    pxr::VtValue get_renderer_setting(const pxr::TfToken& id) const;
    void set_renderer_setting(
        const pxr::TfToken& id,
        const pxr::VtValue& value);
    void finish_render();

    std::shared_ptr<PickEvent> consume_pick_event();

   protected:
    ImGuiWindowFlags GetWindowFlag() override;
    const char* GetWindowName() override;
    std::string GetWindowUniqueName() override;

   private:
    void RenderBackBufferResized(float x, float y);

    enum class CamType { First, Third };
    struct Status {
        CamType cam_type = CamType::Third;  // Default to 3rd person camera
        unsigned renderer_id = 0;
    } engine_status;

    bool is_editing_ = false;
    bool is_active = false;
    bool is_hovered = false;

    bool playing = false;
    bool left_mouse_pressed = false;

    std::unique_ptr<BaseCamera> free_camera_;
    std::unique_ptr<pxr::UsdImagingGLEngine> renderer_;
    pxr::UsdImagingGLRenderParams _renderParams;
    pxr::GfVec2i render_buffer_size_;

    Stage* stage_;
    pxr::HgiUniquePtr hgi;
    std::vector<uint8_t> texture_data_;
    const void* renderer_ui_control = nullptr;
    bool first_draw = true;
    pxr::TfHashMap<pxr::TfToken, pxr::VtValue, pxr::TfHash> settings;
    nvrhi::TextureHandle persistent_texture;
    nvrhi::CommandListHandle command_list_;

    void DrawMenuBar();
    void OnFrame(float delta_time);
    void time_controller();

    static void CreateGLContext();

   protected:
    bool JoystickButtonUpdate(int button, bool pressed) override;
    bool JoystickAxisUpdate(int axis, float value) override;
    bool KeyboardUpdate(int key, int scancode, int action, int mods) override;
    bool MousePosUpdate(double xpos, double ypos) override;
    bool MouseScrollUpdate(double xoffset, double yoffset) override;
    bool MouseButtonUpdate(int button, int action, int mods) override;
    void Animate(float elapsed_time_seconds) override;

    void copy_to_presentation();
    std::unique_ptr<UsdviewEnginePrivateData> data_;

    float timecode = 0;
    float time_code_max = 10;

    std::shared_ptr<PickEvent> current_pick_event_;

    // Selection highlight support
    bool selection_event_subscribed_ = false;
    void subscribe_to_selection_events();
    void on_prim_selected(const pxr::SdfPath& path);
    pxr::SdfPath current_selected_path_;  // Track current selection

    // Camera transform synchronization support
    bool camera_transform_event_subscribed_ = false;
    void subscribe_to_camera_transform_events();
    void on_camera_transform_modified();

    // Cache last camera state for delta calculation when Inspector modifies
    // transform
    pxr::GfVec3d cached_camera_pos_;
    pxr::GfVec3d cached_target_pos_;
    bool camera_state_cached_ = false;

    // Cached frustum for raycast during camera switching
    pxr::GfFrustum cached_frustum_;

    // ImGuizmo state
    ImGuizmo::OPERATION gizmo_operation_ = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE gizmo_mode_ = ImGuizmo::WORLD;
    bool gizmo_use_snap_ = false;
    float gizmo_snap_[3] = { 1.0f, 1.0f, 1.0f };
    float view_manipulate_size_ = 128.0f;

    void DrawGizmo(const ImVec2& viewport_pos, const ImVec2& viewport_size);
    void DrawViewManipulate(
        const ImVec2& viewport_pos,
        const ImVec2& viewport_size);
};
RUZINO_NAMESPACE_CLOSE_SCOPE
