#include "input/input_state.h"

#include <cmath>

RUZINO_NAMESPACE_OPEN_SCOPE

namespace input {

void InputState::set_key(int key_code, int action)
{
    if (key_code < 0 || key_code >= kKeyCount)
        return;
    key_down_[key_code] = (action != kRelease);
    if (action == kPress)
        key_pressed_[key_code] = 1;
}

void InputState::set_mouse_button(int button, int action)
{
    if (button < 0 || button >= kMouseButtonCount)
        return;
    mouse_down_[button] = (action != kRelease);
}

void InputState::add_mouse_delta(double dx, double dy)
{
    mouse_delta_x_ += dx;
    mouse_delta_y_ += dy;
}

void InputState::add_scroll(double dx, double dy)
{
    scroll_delta_ += dy;
}

void InputState::begin_frame()
{
    key_pressed_.fill(0);
    mouse_delta_x_ = 0.0;
    mouse_delta_y_ = 0.0;
    scroll_delta_ = 0.0;
}

bool InputState::is_down(int key_code) const
{
    if (key_code < 0 || key_code >= kKeyCount)
        return false;
    return key_down_[key_code] != 0;
}

bool InputState::was_pressed(int key_code) const
{
    if (key_code < 0 || key_code >= kKeyCount)
        return false;
    return key_pressed_[key_code] != 0;
}

bool InputState::mouse_is_down(int button) const
{
    if (button < 0 || button >= kMouseButtonCount)
        return false;
    return mouse_down_[button] != 0;
}

void InputState::move_axis(float& x, float& y) const
{
    if (axis_override_active_) {
        x = axis_override_[0];
        y = axis_override_[1];
        return;
    }

    x = 0.0f;
    y = 0.0f;
    if (is_down(key::D))
        x += 1.0f;
    if (is_down(key::A))
        x -= 1.0f;
    if (is_down(key::W))
        y += 1.0f;
    if (is_down(key::S))
        y -= 1.0f;

    float len = std::sqrt(x * x + y * y);
    if (len > 1.0f) {
        x /= len;
        y /= len;
    }
}

void InputState::set_axis_override(float x, float y)
{
    float len = std::sqrt(x * x + y * y);
    if (len > 1.0f) {
        x /= len;
        y /= len;
    }
    axis_override_[0] = x;
    axis_override_[1] = y;
    axis_override_active_ = true;
}

void InputState::set_view_frame(
    float forward_x,
    float forward_y,
    float right_x,
    float right_y)
{
    auto normalize2 = [](float& a, float& b) {
        float len = std::sqrt(a * a + b * b);
        if (len > 1e-6f) {
            a /= len;
            b /= len;
        }
    };
    normalize2(forward_x, forward_y);
    normalize2(right_x, right_y);
    view_forward_[0] = forward_x;
    view_forward_[1] = forward_y;
    view_right_[0] = right_x;
    view_right_[1] = right_y;
}

}  // namespace input

RUZINO_NAMESPACE_CLOSE_SCOPE
