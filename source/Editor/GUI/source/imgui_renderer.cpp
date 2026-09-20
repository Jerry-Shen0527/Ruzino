/*
 * Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
License for Dear ImGui

Copyright (c) 2014-2019 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "imgui_renderer.h"

#include "GUI/api.h"
RUZINO_NAMESPACE_OPEN_SCOPE

// GLFW keycode -> ImGuiKey. Same table as ImGui_ImplGlfw_KeyToImGuiKey in
// imgui_impl_glfw.cpp (Dear ImGui, MIT); keys ARE their own index since 1.87.
static ImGuiKey GlfwKeyToImGuiKey(int keycode)
{
    switch (keycode) {
        case GLFW_KEY_TAB: return ImGuiKey_Tab;
        case GLFW_KEY_LEFT: return ImGuiKey_LeftArrow;
        case GLFW_KEY_RIGHT: return ImGuiKey_RightArrow;
        case GLFW_KEY_UP: return ImGuiKey_UpArrow;
        case GLFW_KEY_DOWN: return ImGuiKey_DownArrow;
        case GLFW_KEY_PAGE_UP: return ImGuiKey_PageUp;
        case GLFW_KEY_PAGE_DOWN: return ImGuiKey_PageDown;
        case GLFW_KEY_HOME: return ImGuiKey_Home;
        case GLFW_KEY_END: return ImGuiKey_End;
        case GLFW_KEY_INSERT: return ImGuiKey_Insert;
        case GLFW_KEY_DELETE: return ImGuiKey_Delete;
        case GLFW_KEY_BACKSPACE: return ImGuiKey_Backspace;
        case GLFW_KEY_SPACE: return ImGuiKey_Space;
        case GLFW_KEY_ENTER: return ImGuiKey_Enter;
        case GLFW_KEY_ESCAPE: return ImGuiKey_Escape;
        case GLFW_KEY_APOSTROPHE: return ImGuiKey_Apostrophe;
        case GLFW_KEY_COMMA: return ImGuiKey_Comma;
        case GLFW_KEY_MINUS: return ImGuiKey_Minus;
        case GLFW_KEY_PERIOD: return ImGuiKey_Period;
        case GLFW_KEY_SLASH: return ImGuiKey_Slash;
        case GLFW_KEY_SEMICOLON: return ImGuiKey_Semicolon;
        case GLFW_KEY_EQUAL: return ImGuiKey_Equal;
        case GLFW_KEY_LEFT_BRACKET: return ImGuiKey_LeftBracket;
        case GLFW_KEY_BACKSLASH: return ImGuiKey_Backslash;
        case GLFW_KEY_WORLD_1: return ImGuiKey_Oem102;
        case GLFW_KEY_WORLD_2: return ImGuiKey_Oem102;
        case GLFW_KEY_RIGHT_BRACKET: return ImGuiKey_RightBracket;
        case GLFW_KEY_GRAVE_ACCENT: return ImGuiKey_GraveAccent;
        case GLFW_KEY_CAPS_LOCK: return ImGuiKey_CapsLock;
        case GLFW_KEY_SCROLL_LOCK: return ImGuiKey_ScrollLock;
        case GLFW_KEY_NUM_LOCK: return ImGuiKey_NumLock;
        case GLFW_KEY_PRINT_SCREEN: return ImGuiKey_PrintScreen;
        case GLFW_KEY_PAUSE: return ImGuiKey_Pause;
        case GLFW_KEY_KP_0: return ImGuiKey_Keypad0;
        case GLFW_KEY_KP_1: return ImGuiKey_Keypad1;
        case GLFW_KEY_KP_2: return ImGuiKey_Keypad2;
        case GLFW_KEY_KP_3: return ImGuiKey_Keypad3;
        case GLFW_KEY_KP_4: return ImGuiKey_Keypad4;
        case GLFW_KEY_KP_5: return ImGuiKey_Keypad5;
        case GLFW_KEY_KP_6: return ImGuiKey_Keypad6;
        case GLFW_KEY_KP_7: return ImGuiKey_Keypad7;
        case GLFW_KEY_KP_8: return ImGuiKey_Keypad8;
        case GLFW_KEY_KP_9: return ImGuiKey_Keypad9;
        case GLFW_KEY_KP_DECIMAL: return ImGuiKey_KeypadDecimal;
        case GLFW_KEY_KP_DIVIDE: return ImGuiKey_KeypadDivide;
        case GLFW_KEY_KP_MULTIPLY: return ImGuiKey_KeypadMultiply;
        case GLFW_KEY_KP_SUBTRACT: return ImGuiKey_KeypadSubtract;
        case GLFW_KEY_KP_ADD: return ImGuiKey_KeypadAdd;
        case GLFW_KEY_KP_ENTER: return ImGuiKey_KeypadEnter;
        case GLFW_KEY_KP_EQUAL: return ImGuiKey_KeypadEqual;
        case GLFW_KEY_LEFT_SHIFT: return ImGuiKey_LeftShift;
        case GLFW_KEY_LEFT_CONTROL: return ImGuiKey_LeftCtrl;
        case GLFW_KEY_LEFT_ALT: return ImGuiKey_LeftAlt;
        case GLFW_KEY_LEFT_SUPER: return ImGuiKey_LeftSuper;
        case GLFW_KEY_RIGHT_SHIFT: return ImGuiKey_RightShift;
        case GLFW_KEY_RIGHT_CONTROL: return ImGuiKey_RightCtrl;
        case GLFW_KEY_RIGHT_ALT: return ImGuiKey_RightAlt;
        case GLFW_KEY_RIGHT_SUPER: return ImGuiKey_RightSuper;
        case GLFW_KEY_MENU: return ImGuiKey_Menu;
        case GLFW_KEY_0: return ImGuiKey_0;
        case GLFW_KEY_1: return ImGuiKey_1;
        case GLFW_KEY_2: return ImGuiKey_2;
        case GLFW_KEY_3: return ImGuiKey_3;
        case GLFW_KEY_4: return ImGuiKey_4;
        case GLFW_KEY_5: return ImGuiKey_5;
        case GLFW_KEY_6: return ImGuiKey_6;
        case GLFW_KEY_7: return ImGuiKey_7;
        case GLFW_KEY_8: return ImGuiKey_8;
        case GLFW_KEY_9: return ImGuiKey_9;
        case GLFW_KEY_A: return ImGuiKey_A;
        case GLFW_KEY_B: return ImGuiKey_B;
        case GLFW_KEY_C: return ImGuiKey_C;
        case GLFW_KEY_D: return ImGuiKey_D;
        case GLFW_KEY_E: return ImGuiKey_E;
        case GLFW_KEY_F: return ImGuiKey_F;
        case GLFW_KEY_G: return ImGuiKey_G;
        case GLFW_KEY_H: return ImGuiKey_H;
        case GLFW_KEY_I: return ImGuiKey_I;
        case GLFW_KEY_J: return ImGuiKey_J;
        case GLFW_KEY_K: return ImGuiKey_K;
        case GLFW_KEY_L: return ImGuiKey_L;
        case GLFW_KEY_M: return ImGuiKey_M;
        case GLFW_KEY_N: return ImGuiKey_N;
        case GLFW_KEY_O: return ImGuiKey_O;
        case GLFW_KEY_P: return ImGuiKey_P;
        case GLFW_KEY_Q: return ImGuiKey_Q;
        case GLFW_KEY_R: return ImGuiKey_R;
        case GLFW_KEY_S: return ImGuiKey_S;
        case GLFW_KEY_T: return ImGuiKey_T;
        case GLFW_KEY_U: return ImGuiKey_U;
        case GLFW_KEY_V: return ImGuiKey_V;
        case GLFW_KEY_W: return ImGuiKey_W;
        case GLFW_KEY_X: return ImGuiKey_X;
        case GLFW_KEY_Y: return ImGuiKey_Y;
        case GLFW_KEY_Z: return ImGuiKey_Z;
        case GLFW_KEY_F1: return ImGuiKey_F1;
        case GLFW_KEY_F2: return ImGuiKey_F2;
        case GLFW_KEY_F3: return ImGuiKey_F3;
        case GLFW_KEY_F4: return ImGuiKey_F4;
        case GLFW_KEY_F5: return ImGuiKey_F5;
        case GLFW_KEY_F6: return ImGuiKey_F6;
        case GLFW_KEY_F7: return ImGuiKey_F7;
        case GLFW_KEY_F8: return ImGuiKey_F8;
        case GLFW_KEY_F9: return ImGuiKey_F9;
        case GLFW_KEY_F10: return ImGuiKey_F10;
        case GLFW_KEY_F11: return ImGuiKey_F11;
        case GLFW_KEY_F12: return ImGuiKey_F12;
        case GLFW_KEY_F13: return ImGuiKey_F13;
        case GLFW_KEY_F14: return ImGuiKey_F14;
        case GLFW_KEY_F15: return ImGuiKey_F15;
        case GLFW_KEY_F16: return ImGuiKey_F16;
        case GLFW_KEY_F17: return ImGuiKey_F17;
        case GLFW_KEY_F18: return ImGuiKey_F18;
        case GLFW_KEY_F19: return ImGuiKey_F19;
        case GLFW_KEY_F20: return ImGuiKey_F20;
        case GLFW_KEY_F21: return ImGuiKey_F21;
        case GLFW_KEY_F22: return ImGuiKey_F22;
        case GLFW_KEY_F23: return ImGuiKey_F23;
        case GLFW_KEY_F24: return ImGuiKey_F24;
        default: return ImGuiKey_None;
    }
}

// Since 1.87 modifiers must be queued as key events (ImGuiKey_ModXXX); the
// io.KeyCtrl/KeyShift/... fields are read-only outputs now.
static void UpdateImGuiModifierKeys(
    ImGuiIO& io,
    const std::array<bool, GLFW_KEY_LAST + 1>& keyDown)
{
    io.AddKeyEvent(
        ImGuiMod_Ctrl,
        keyDown[GLFW_KEY_LEFT_CONTROL] || keyDown[GLFW_KEY_RIGHT_CONTROL]);
    io.AddKeyEvent(
        ImGuiMod_Shift,
        keyDown[GLFW_KEY_LEFT_SHIFT] || keyDown[GLFW_KEY_RIGHT_SHIFT]);
    io.AddKeyEvent(
        ImGuiMod_Alt,
        keyDown[GLFW_KEY_LEFT_ALT] || keyDown[GLFW_KEY_RIGHT_ALT]);
    io.AddKeyEvent(
        ImGuiMod_Super,
        keyDown[GLFW_KEY_LEFT_SUPER] || keyDown[GLFW_KEY_RIGHT_SUPER]);
}

ImGui_Renderer::ImGui_Renderer(DeviceManager* devManager)
    : IRenderPass(devManager)
{
    // 若使用Polyscope，则此时ImGui已经被初始化，不需要再次初始化
    if (!ImGui::GetCurrentContext()) {
        ImGui::CreateContext();
        contextInitialized = true;
    }
}

ImGui_Renderer::~ImGui_Renderer()
{
    if (contextInitialized) {
        ImGui::DestroyContext();
    }
}

bool ImGui_Renderer::Init(std::shared_ptr<ShaderFactory> shaderFactory)
{
    // Keyboard input is queued per-event in KeyboardUpdate() via
    // io.AddKeyEvent(); the legacy io.KeyMap/io.KeysDown indirection was
    // removed in ImGui 1.92.

    imgui_nvrhi = std::make_unique<ImGui_NVRHI>();
    return imgui_nvrhi->init(GetDevice(), shaderFactory);
}
//
// ImFont* ImGui_Renderer::LoadFont(
//    IFileSystem& fs,
//    const std::filesystem::path& fontFile,
//    float fontSize)
//{
//    auto fontData = fs.readFile(fontFile);
//    if (!fontData)
//        return nullptr;
//
//    ImFontConfig fontConfig;
//
//    // XXXX mk: this appears to be a bug: the atlas copies (& owns) the data
//    // when the flag is set to false !
//    fontConfig.FontDataOwnedByAtlas = false;
//    ImFont* imFont = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
//        (void*)(fontData->data()),
//        (int)(fontData->size()),
//        fontSize,
//        &fontConfig);
//
//    return imFont;
//}

bool ImGui_Renderer::KeyboardUpdate(int key, int scancode, int action, int mods)
{
    auto& io = ImGui::GetIO();

    bool keyIsDown;
    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        keyIsDown = true;
    }
    else {
        keyIsDown = false;
    }

    // update our internal state tracking for this key button
    keyDown[key] = keyIsDown;

    // queue the translated key event; ImGui processes the queue at the next
    // NewFrame, so both press and release are forwarded immediately
    ImGuiKey imguiKey = GlfwKeyToImGuiKey(key);
    if (imguiKey != ImGuiKey_None)
        io.AddKeyEvent(imguiKey, keyIsDown);
    UpdateImGuiModifierKeys(io, keyDown);

    return io.WantCaptureKeyboard;
}

bool ImGui_Renderer::KeyboardCharInput(unsigned int unicode, int mods)
{
    auto& io = ImGui::GetIO();

    io.AddInputCharacter(unicode);

    return io.WantCaptureKeyboard;
}

bool ImGui_Renderer::MousePosUpdate(double xpos, double ypos)
{
    auto& io = ImGui::GetIO();
    io.MousePos.x = float(xpos);
    io.MousePos.y = float(ypos);

    return io.WantCaptureMouse;
}

bool ImGui_Renderer::MouseScrollUpdate(double xoffset, double yoffset)
{
    auto& io = ImGui::GetIO();
    io.MouseWheel += float(yoffset);

    return io.WantCaptureMouse;
}

bool ImGui_Renderer::MouseButtonUpdate(int button, int action, int mods)
{
    auto& io = ImGui::GetIO();

    bool buttonIsDown;
    int buttonIndex;

    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        buttonIsDown = true;
    }
    else {
        buttonIsDown = false;
    }

    switch (button) {
        case GLFW_MOUSE_BUTTON_LEFT: buttonIndex = 0; break;

        case GLFW_MOUSE_BUTTON_RIGHT: buttonIndex = 1; break;

        case GLFW_MOUSE_BUTTON_MIDDLE: buttonIndex = 2; break;

        default: return false;  // unhandled button
    }

    // update our internal state tracking for this mouse button
    mouseDown[buttonIndex] = buttonIsDown;

    if (buttonIsDown) {
        // update ImGui state immediately
        io.MouseDown[buttonIndex] = true;
    }
    else {
        // for mouse up events, ImGui state is only updated after the next frame
        // this ensures that short clicks are not missed
    }

    return io.WantCaptureMouse;
}

void ImGui_Renderer::Animate(float elapsedTimeSeconds)
{
    if (!imgui_nvrhi)
        return;

    int w, h;
    float scaleX, scaleY;

    GetDeviceManager()->GetWindowDimensions(w, h);
    GetDeviceManager()->GetDPIScaleInfo(scaleX, scaleY);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(w), float(h));
    io.DisplayFramebufferScale.x = scaleX;
    io.DisplayFramebufferScale.y = scaleY;

    UpdateImGuiModifierKeys(io, keyDown);

    imgui_nvrhi->beginFrame(elapsedTimeSeconds);
}

void ImGui_Renderer::Render(nvrhi::IFramebuffer* framebuffer)
{
    if (!imgui_nvrhi)
        return;

    buildUI();

    ImGui::Render();
    imgui_nvrhi->render(framebuffer);

    // reconcile mouse button states
    auto& io = ImGui::GetIO();
    for (size_t i = 0; i < mouseDown.size(); i++) {
        if (io.MouseDown[i] == true && mouseDown[i] == false) {
            io.MouseDown[i] = false;
        }
    }
}

void ImGui_Renderer::BackBufferResizing()
{
    if (imgui_nvrhi)
        imgui_nvrhi->backbufferResizing();
}

void ImGui_Renderer::BeginFullScreenWindow()
{
    int width, height;
    GetDeviceManager()->GetWindowDimensions(width, height);
    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(float(width), float(height)), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::SetNextWindowBgAlpha(0.f);
    ImGui::Begin(
        " ",
        0,
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoScrollbar);
}

void ImGui_Renderer::DrawScreenCenteredText(const char* text)
{
    int width, height;
    GetDeviceManager()->GetWindowDimensions(width, height);
    ImVec2 textSize = ImGui::CalcTextSize(text);
    ImGui::SetCursorPosX((float(width) - textSize.x) * 0.5f);
    ImGui::SetCursorPosY((float(height) - textSize.y) * 0.5f);
    ImGui::TextUnformatted(text);
}

void ImGui_Renderer::EndFullScreenWindow()
{
    ImGui::End();
    ImGui::PopStyleVar();
}
RUZINO_NAMESPACE_CLOSE_SCOPE