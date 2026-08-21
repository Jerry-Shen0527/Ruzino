#pragma once

#include <array>
#include <cstdint>

#include "api.h"

RUZINO_NAMESPACE_OPEN_SCOPE

namespace input {

// Key codes mirror GLFW key codes (letters/digits are ASCII). Publishers pass
// the raw GLFW code through, so this module has no GLFW dependency.
namespace key {
    inline constexpr int Escape = 256;
    inline constexpr int Space = 32;
    inline constexpr int W = 87;
    inline constexpr int A = 65;
    inline constexpr int S = 83;
    inline constexpr int D = 68;
    inline constexpr int LeftShift = 340;
    inline constexpr int RightShift = 341;
    inline constexpr int LeftControl = 342;
    inline constexpr int RightControl = 343;
    inline constexpr int Up = 265;
    inline constexpr int Down = 264;
    inline constexpr int Left = 263;
    inline constexpr int Right = 262;
}  // namespace key

// GLFW action codes, passed through unchanged by publishers.
inline constexpr int kRelease = 0;
inline constexpr int kPress = 1;
inline constexpr int kRepeat = 2;

// Mouse button codes (GLFW convention: 0 = left, 1 = right, 2 = middle).
inline constexpr int kMouseButtonLeft = 0;
inline constexpr int kMouseButtonRight = 1;
inline constexpr int kMouseButtonMiddle = 2;

// Max GLFW keycode is 348; index by code with a small guard band.
inline constexpr int kKeyCount = 512;
inline constexpr int kMouseButtonCount = 8;

// Process-wide input snapshot. The window layer publishes raw GLFW events
// into it; gameplay code (character controller, future consumers) reads a
// per-frame view. `begin_frame()` marks the frame boundary and clears the
// "pressed this frame" edges and accumulated deltas.
//
// All access happens on the main thread (event callbacks and Stage::tick),
// so no synchronization is needed.
class INPUT_API InputState {
   public:
    InputState() = default;

    // ---- publisher side (window layer) ------------------------------------

    // action is a GLFW action code (kPress / kRelease / kRepeat).
    void set_key(int key_code, int action);
    void set_mouse_button(int button, int action);
    void add_mouse_delta(double dx, double dy);
    void add_scroll(double dx, double dy);

    // Clears per-frame edges/deltas; call once per frame, before the first
    // consumer of the frame (the app registers this ahead of Stage::tick).
    void begin_frame();

    // ---- consumer side -----------------------------------------------------

    bool is_down(int key_code) const;
    bool was_pressed(int key_code) const;  // press edge since last begin_frame
    bool mouse_is_down(int button) const;

    double mouse_delta_x() const
    {
        return mouse_delta_x_;
    }
    double mouse_delta_y() const
    {
        return mouse_delta_y_;
    }
    double scroll_delta() const
    {
        return scroll_delta_;
    }

    bool run_held() const
    {
        return is_down(key::LeftShift) || is_down(key::RightShift);
    }

    // WASD movement axis in view space: x = strafe (D positive), y = forward
    // (W positive), normalized to unit length (or zero). When an axis
    // override is active (headless tests / Python console) it is returned
    // instead, so a character can be driven without a window.
    void move_axis(float& x, float& y) const;

    // Axis override for programmatic control (tests, scripting). The
    // override is not cleared by begin_frame; call clear_axis_override().
    void set_axis_override(float x, float y);
    void clear_axis_override()
    {
        axis_override_active_ = false;
    }
    bool has_axis_override() const
    {
        return axis_override_active_;
    }

    // Ground-projected camera reference frame, published by the viewport so
    // gameplay movement is camera-relative. Defaults: forward +Y, right +X
    // (identity view). Vectors are normalized on set.
    void set_view_frame(
        float forward_x,
        float forward_y,
        float right_x,
        float right_y);
    float view_forward_x() const
    {
        return view_forward_[0];
    }
    float view_forward_y() const
    {
        return view_forward_[1];
    }
    float view_right_x() const
    {
        return view_right_[0];
    }
    float view_right_y() const
    {
        return view_right_[1];
    }

   private:
    std::array<uint8_t, kKeyCount> key_down_{};
    std::array<uint8_t, kKeyCount> key_pressed_{};
    std::array<uint8_t, kMouseButtonCount> mouse_down_{};

    double mouse_delta_x_ = 0.0;
    double mouse_delta_y_ = 0.0;
    double scroll_delta_ = 0.0;

    float view_forward_[2] = { 0.0f, 1.0f };
    float view_right_[2] = { 1.0f, 0.0f };

    bool axis_override_active_ = false;
    float axis_override_[2] = { 0.0f, 0.0f };
};

}  // namespace input

RUZINO_NAMESPACE_CLOSE_SCOPE
