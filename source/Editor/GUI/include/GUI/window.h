#pragma once

#include <any>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "GUI/api.h"
#include "input/input_state.h"
#include "widget.h"
struct GLFWwindow;

RUZINO_NAMESPACE_OPEN_SCOPE

class DockingImguiRenderer;

// Simple event system for widget communication
class GUI_API WindowEventSystem {
   public:
    using EventCallback = std::function<void(const std::string& event_data)>;
    using EventCallbackAny = std::function<void(const std::any& event_data)>;

    void subscribe(const std::string& event_name, EventCallback callback);
    void subscribe_any(
        const std::string& event_name,
        EventCallbackAny callback);

    void emit(
        const std::string& event_name,
        const std::string& event_data = "");
    void emit_any(const std::string& event_name, const std::any& event_data);

   private:
    std::unordered_map<std::string, std::vector<EventCallback>> subscribers_;
    std::unordered_map<std::string, std::vector<EventCallbackAny>>
        subscribers_any_;
};

// Represents a window in a GUI application, providing basic functionalities
// such as initialization and rendering.
class GUI_API Window {
   public:
    // Constructor that sets the window's title.
    explicit Window();

    virtual ~Window();

    // Enters the main rendering loop.
    float get_elapsed_time();
    void run();
    void register_widget(std::unique_ptr<IWidget> unique);

    void register_function_before_frame(
        const std::function<void(Window*)>& callback);
    void register_function_after_frame(
        const std::function<void(Window*)>& callback);

    void register_openable_widget(
        std::unique_ptr<IWidgetFactory> window_factory,
        const std::vector<std::string>& menu_item);
    IWidget* get_widget(const std::string& unique_name) const;
    std::vector<IWidget*> get_widgets() const;

    // Event system access
    WindowEventSystem& events()
    {
        return event_system_;
    }

    // Menu action callbacks
    void register_menu_action(
        const std::string& action_name,
        std::function<void()> callback);
    void trigger_menu_action(const std::string& action_name);

    void close();

    // Gameplay input plumbing: the window layer publishes every raw
    // key/mouse event it receives into this snapshot (before dispatching to
    // widgets), so consumers like the character controllers see input even
    // though they live outside the widget tree. The Stage owns the state.
    void set_input_state(input::InputState* state)
    {
        input_state_ = state;
    }
    input::InputState* get_input_state()
    {
        return input_state_;
    }

    int get_size_x() const;
    int get_size_y() const;

    virtual void SetFullscreen(bool enabled);
    [[nodiscard]] bool IsFullscreen() const;

    virtual void SetMaximized(bool enabled);
    [[nodiscard]] bool IsMaximized() const;

   protected:
    std::unique_ptr<DockingImguiRenderer> imguiRenderPass;
    float elapsedTimeSeconds = 0.0f;
    WindowEventSystem event_system_;
    std::unordered_map<std::string, std::function<void()>> menu_actions_;
    input::InputState* input_state_ = nullptr;
    friend class DockingImguiRenderer;
};

RUZINO_NAMESPACE_CLOSE_SCOPE
