#include "GUI/window.h"

#include "spdlog/spdlog.h"
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1

#include <imgui.h>

#include <RHI/rhi.hpp>
#include <any>
#include <format>

#include "GUI/ImGuiFileDialog.h"
#include "RHI/DeviceManager/DeviceManager.h"
#include "RHI/ShaderFactory/shader.hpp"
#include "imgui_renderer.h"

RUZINO_NAMESPACE_OPEN_SCOPE

// WindowEventSystem implementation
void WindowEventSystem::subscribe(
    const std::string& event_name,
    EventCallback callback)
{
    subscribers_[event_name].push_back(callback);
}

void WindowEventSystem::subscribe_any(
    const std::string& event_name,
    EventCallbackAny callback)
{
    subscribers_any_[event_name].push_back(callback);
}

void WindowEventSystem::emit(
    const std::string& event_name,
    const std::string& event_data)
{
    auto it = subscribers_.find(event_name);
    if (it != subscribers_.end()) {
        for (auto& callback : it->second) {
            callback(event_data);
        }
    }
}

void WindowEventSystem::emit_any(
    const std::string& event_name,
    const std::any& event_data)
{
    auto it = subscribers_any_.find(event_name);
    if (it != subscribers_any_.end()) {
        for (auto& callback : it->second) {
            callback(event_data);
        }
    }
}

class DockingImguiRenderer final : public ImGui_Renderer {
    friend class Window;

   public:
    explicit DockingImguiRenderer(Window* window, DeviceManager* devManager)
        : window_(window),
          ImGui_Renderer(devManager)
    {
    }

    ~DockingImguiRenderer() override;

    bool JoystickButtonUpdate(int button, bool pressed) override;
    bool JoystickAxisUpdate(int axis, float value) override;
    bool KeyboardUpdate(int key, int scancode, int action, int mods) override;
    bool KeyboardCharInput(unsigned unicode, int mods) override;
    bool MousePosUpdate(double xpos, double ypos) override;
    bool MouseScrollUpdate(double xoffset, double yoffset) override;
    bool MouseButtonUpdate(int button, int action, int mods) override;
    void Animate(float elapsedTimeSeconds) override;
    void register_function_before_frame(
        const std::function<void(Window*)>& callback);
    void register_function_after_frame(
        const std::function<void(Window*)>& callback);
    void register_openable_widget(
        std::unique_ptr<IWidgetFactory>& widget_factory,
        const std::vector<std::string>& menu_item);

   private:
    void register_widget(std::unique_ptr<IWidget> widget);
    void drawMenuBar();
    void buildUI() override;

    std::vector<std::unique_ptr<IWidget>> widgets_;
    std::vector<std::unique_ptr<IWidget>> pending_widgets_;
    Window* window_;
    std::vector<std::function<void(Window*)>> callbacks_before_frame_;
    std::vector<std::function<void(Window*)>> callbacks_after_frame_;

    struct MenuNode {
        std::unordered_map<std::string, std::unique_ptr<MenuNode>> children;
        std::unique_ptr<IWidgetFactory> widget_factory;

        void register_node(
            const std::vector<std::string>& path,
            std::unique_ptr<IWidgetFactory>& factory)
        {
            if (path.empty()) {
                widget_factory = std::move(factory);
                return;
            }

            auto& child = children[path.front()];
            if (!child) {
                child = std::make_unique<MenuNode>();
            }

            child->register_node(
                std::vector(path.begin() + 1, path.end()), factory);
        }
    };

    MenuNode menu_tree;

    void recursive_draw(MenuNode& node);
};

DockingImguiRenderer::~DockingImguiRenderer()
{
    callbacks_after_frame_.clear();

    // widgets_ should be cleared from the last to the first instead of using
    // widgets_.clear();

    for (auto it = widgets_.rbegin(); it != widgets_.rend(); ++it) {
        it->reset();
    }
}

bool DockingImguiRenderer::JoystickButtonUpdate(int button, bool pressed)
{
    for (auto&& widget : widgets_) {
        if (widget->JoystickButtonUpdate(button, pressed)) {
            return true;
        }
    }
    return ImGui_Renderer::JoystickButtonUpdate(button, pressed);
}

bool DockingImguiRenderer::JoystickAxisUpdate(int axis, float value)
{
    for (auto&& widget : widgets_) {
        if (widget->JoystickAxisUpdate(axis, value)) {
            return true;
        }
    }
    return ImGui_Renderer::JoystickAxisUpdate(axis, value);
}

bool DockingImguiRenderer::KeyboardUpdate(
    int key,
    int scancode,
    int action,
    int mods)
{
    // Publish raw key events into the gameplay input snapshot before any
    // widget can consume them. Skipped while ImGui owns the keyboard (text
    // fields / console), mirroring standard game-engine input routing.
    if (window_ && window_->get_input_state() &&
        !ImGui::GetIO().WantCaptureKeyboard) {
        window_->get_input_state()->set_key(key, action);
    }

    for (auto&& widget : widgets_) {
        if (widget->KeyboardUpdate(key, scancode, action, mods)) {
            return true;
        }
    }
    return ImGui_Renderer::KeyboardUpdate(key, scancode, action, mods);
}

bool DockingImguiRenderer::KeyboardCharInput(unsigned unicode, int mods)
{
    for (auto&& widget : widgets_) {
        if (widget->KeyboardCharInput(unicode, mods)) {
            return true;
        }
    }
    return ImGui_Renderer::KeyboardCharInput(unicode, mods);
}

bool DockingImguiRenderer::MousePosUpdate(double xpos, double ypos)
{
    for (auto&& widget : widgets_) {
        if (widget->MousePosUpdate(xpos, ypos)) {
            return true;
        }
    }
    return ImGui_Renderer::MousePosUpdate(xpos, ypos);
}

bool DockingImguiRenderer::MouseScrollUpdate(double xoffset, double yoffset)
{
    for (auto&& widget : widgets_) {
        if (widget->MouseScrollUpdate(xoffset, yoffset)) {
            return true;
        }
    }
    return ImGui_Renderer::MouseScrollUpdate(xoffset, yoffset);
}

bool DockingImguiRenderer::MouseButtonUpdate(int button, int action, int mods)
{
    if (window_ && window_->get_input_state() &&
        !ImGui::GetIO().WantCaptureMouse) {
        window_->get_input_state()->set_mouse_button(button, action);
    }

    for (auto&& widget : widgets_) {
        if (widget->MouseButtonUpdate(button, action, mods)) {
            return true;
        }
    }
    return ImGui_Renderer::MouseButtonUpdate(button, action, mods);
}

void DockingImguiRenderer::Animate(float elapsedTimeSeconds)
{
    for (auto&& widget : widgets_) {
        widget->Animate(elapsedTimeSeconds);
    }
    ImGui_Renderer::Animate(elapsedTimeSeconds);
    window_->elapsedTimeSeconds = elapsedTimeSeconds;
}

void DockingImguiRenderer::register_function_before_frame(
    const std::function<void(Window*)>& callback)
{
    callbacks_before_frame_.push_back(callback);
}

void DockingImguiRenderer::register_function_after_frame(
    const std::function<void(Window*)>& callback)
{
    callbacks_after_frame_.push_back(callback);
}

void DockingImguiRenderer::register_openable_widget(
    std::unique_ptr<IWidgetFactory>& widget_factory,
    const std::vector<std::string>& menu_item)
{
    menu_tree.register_node(menu_item, widget_factory);
}

void DockingImguiRenderer::register_widget(std::unique_ptr<IWidget> widget)
{
    if (!widget) {
        return;
    }

    // If the widget with the "UniqueName" exists, replace it
    std::string unique_name = widget->GetWindowUniqueName();
    for (auto& w : widgets_) {
        if (w && w->GetWindowUniqueName() == unique_name) {
            w = std::move(widget);
            return;
        }
    }

    // Add to pending queue to avoid modifying widgets_ during iteration
    pending_widgets_.push_back(std::move(widget));
}

void DockingImguiRenderer::drawMenuBar()
{
    if (ImGui::BeginMenuBar()) {
        // Reset padding for menu items to default size
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 4.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 4.0f));

        // File menu
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open", "Ctrl+O")) {
                window_->trigger_menu_action("file_open");
            }
            if (ImGui::MenuItem("Save", "Ctrl+S")) {
                window_->trigger_menu_action("file_save");
            }
            if (ImGui::MenuItem("Save As", "Ctrl+Shift+S")) {
                window_->trigger_menu_action("file_save_as");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                window_->close();
            }
            ImGui::EndMenu();
        }

        recursive_draw(menu_tree);

        ImGui::PopStyleVar(2);
        ImGui::EndMenuBar();
    }
}

void DockingImguiRenderer::buildUI()
{
    // Process pending widgets before frame callbacks
    if (!pending_widgets_.empty()) {
        for (auto& widget : pending_widgets_) {
            if (widget) {
                widgets_.push_back(std::move(widget));
            }
        }
        pending_widgets_.clear();
    }

    for (auto&& callback : callbacks_before_frame_) {
        callback(window_);
    }
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_None;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoNavFocus |
                    ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin(("DockSpace" + std::to_string(0)).c_str(), 0, window_flags);

    // Temporarily restore normal padding for menu bar
    ImGui::PopStyleVar(1);  // Pop the WindowPadding
    drawMenuBar();

    ImGui::PopStyleVar(2);
    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");

    ImGui::DockSpace(
        dockspace_id,
        ImVec2(0.0f, 0.0f),
        ImGuiDockNodeFlags_PassthruCentralNode);
    std::vector<IWidget*> widget_to_remove;
    for (auto& widget : widgets_) {
        if (!widget) {
            spdlog::warn("Empty widget encountered!");
            continue;
        }

        if (widget->Begin()) {
            // Draw widget-specific menu bar if it has one
            if (widget->HasMenuBar()) {
                if (ImGui::BeginMenuBar()) {
                    widget->DrawMenuBar();
                    ImGui::EndMenuBar();
                }
            }

            if (widget->SizeChanged()) {
                widget->BackBufferResized(widget->Width(), widget->Height(), 1);
            }

            widget->BuildUI();
            widget->SetStatus();
        }

        widget->End();

        if (!widget->IsOpen()) {
            widget_to_remove.push_back(widget.get());
        }
    }

    for (auto widget : widget_to_remove) {
        widgets_.erase(
            std::remove_if(
                widgets_.begin(),
                widgets_.end(),
                [widget](const std::unique_ptr<IWidget>& w) {
                    return w.get() == widget;
                }),
            widgets_.end());
    }
    for (size_t i = 0; i < widgets_.size(); ++i) {
        widgets_[i]->CallBack();
    }

    ImGui::End();

    // Handle file dialogs - must be called after ImGui::End() but within the
    // frame
    auto file_dialog = IGFD::FileDialog::Instance();
    if (file_dialog->Display("OpenStageDialog")) {
        if (file_dialog->IsOk()) {
            std::string file_path = file_dialog->GetFilePathName();
            window_->events().emit("file_open_selected", file_path);
        }
        file_dialog->Close();
    }

    if (file_dialog->Display("SaveStageDialog")) {
        if (file_dialog->IsOk()) {
            std::string file_path = file_dialog->GetFilePathName();
            window_->events().emit("file_save_as_selected", file_path);
        }
        file_dialog->Close();
    }

    for (auto&& callback : callbacks_after_frame_) {
        callback(window_);
    }
}

void DockingImguiRenderer::recursive_draw(MenuNode& node)
{
    for (auto& [name, child] : node.children) {
        if (child->children.empty()) {
            if (ImGui::MenuItem(name.c_str())) {
                auto widget = child->widget_factory->Create(widgets_);
                register_widget(std::move(widget));
            }
        }
        else {
            if (ImGui::BeginMenu(name.c_str())) {
                recursive_draw(*child);
                ImGui::EndMenu();
            }
        }
    }
}

Window::Window()
{
    RHI::init(true);

    auto manager = RHI::internal::get_device_manager();
    imguiRenderPass = std::make_unique<DockingImguiRenderer>(this, manager);
    imguiRenderPass->Init(std::make_shared<ShaderFactory>());

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();

    manager->AddRenderPassToBack(imguiRenderPass.get());
}

Window::~Window()
{
    auto manager = RHI::internal::get_device_manager();

    manager->RemoveRenderPass(imguiRenderPass.get());

    // Destroy imguiRenderPass first to release RHI resources
    imguiRenderPass.reset();

    // Then shutdown RHI (which no longer uses spdlog)
    RHI::shutdown();
}

float Window::get_elapsed_time()
{
    return elapsedTimeSeconds;
}

void Window::run()
{
    auto manager = RHI::internal::get_device_manager();
    manager->RunMessageLoop();
}

void Window::register_widget(std::unique_ptr<IWidget> unique)
{
    unique->SetWindow(this);
    imguiRenderPass->register_widget(std::move(unique));
}

void Window::register_function_before_frame(
    const std::function<void(Window*)>& callback)
{
    imguiRenderPass->register_function_before_frame(callback);
}

void Window::register_function_after_frame(
    const std::function<void(Window*)>& callback)
{
    imguiRenderPass->register_function_after_frame(callback);
}

void Window::register_openable_widget(
    std::unique_ptr<IWidgetFactory> window_factory,
    const std::vector<std::string>& menu_item)
{
    imguiRenderPass->register_openable_widget(window_factory, menu_item);
}
IWidget* Window::get_widget(const std::string& unique_name) const
{
    for (auto& widget : imguiRenderPass->widgets_) {
        if (widget->GetWindowUniqueName() == unique_name) {
            return widget.get();
        }
    }
    return nullptr;
}

std::vector<IWidget*> Window::get_widgets() const
{
    std::vector<IWidget*> widgets;
    for (auto& widget : imguiRenderPass->widgets_) {
        widgets.push_back(widget.get());
    }
    return widgets;
}

void Window::close()
{
    auto manager = RHI::internal::get_device_manager();
    manager->RequestWindowClose();
}
int Window::get_size_x() const
{
    int x, y;
    imguiRenderPass->GetDeviceManager()->GetWindowDimensions(x, y);
    return x;
}
int Window::get_size_y() const
{
    int x, y;
    imguiRenderPass->GetDeviceManager()->GetWindowDimensions(x, y);
    return y;
}

void Window::SetFullscreen(bool enabled)
{
    auto manager = imguiRenderPass->GetDeviceManager();
    manager->SetFullscreen(enabled);
}

bool Window::IsFullscreen() const
{
    auto manager = imguiRenderPass->GetDeviceManager();
    if (!manager)
        return false;

    return manager->IsFullscreen();
}

void Window::SetMaximized(bool enabled)
{
    auto manager = imguiRenderPass->GetDeviceManager();
    manager->SetMaximized(enabled);
}

bool Window::IsMaximized() const
{
    auto manager = imguiRenderPass->GetDeviceManager();
    if (!manager)
        return false;

    return manager->IsMaximized();
}

void Window::register_menu_action(
    const std::string& action_name,
    std::function<void()> callback)
{
    menu_actions_[action_name] = callback;
}

void Window::trigger_menu_action(const std::string& action_name)
{
    auto it = menu_actions_.find(action_name);
    if (it != menu_actions_.end()) {
        it->second();
    }
}

RUZINO_NAMESPACE_CLOSE_SCOPE
