#include <string>

#include "GUI/text_editor_widget.hpp"
#include "imgui.h"
#include "text_editor_widget.h"

RUZINO_NAMESPACE_OPEN_SCOPE

TextEditorWidget::TextEditorWidget(const std::string& title)
    : title_(title),
      editor_(std::make_unique<TextEditor>())
{
    // Don't set fixed width/height - let it be determined by window

    // Set up the text editor with C++ language definition
    editor_->SetLanguageDefinition(TextEditor::LanguageDefinition::CPlusPlus());

    // Set some sample text
    editor_->SetText(R"(#include <iostream>
#include <vector>
#include <string>

int main() {
    std::vector<std::string> messages = {
        "Hello, World!",
        "Welcome to the Text Editor",
        "You can edit this text!"
    };

    for (const auto& msg : messages) {
        std::cout << msg << std::endl;
    }

    return 0;
})");
}

TextEditorWidget::~TextEditorWidget() = default;

bool TextEditorWidget::BuildUI()
{
    // Push font scale if custom size is set
    if (font_size_ > 0) {
        float scale = font_size_ / ImGui::GetFontSize();
        ImGui::SetWindowFontScale(scale);
    }

    // Check for Ctrl+S to save
    if (ImGui::IsWindowFocused() && ImGui::IsKeyDown(ImGuiKey_LeftCtrl) &&
        ImGui::IsKeyPressed(ImGuiKey_S)) {
        ApplyChanges();
    }

    // Get the available content region
    ImVec2 content_region = ImGui::GetContentRegionAvail();

    // Render the text editor
    editor_->Render(title_.c_str(), content_region, false);

    // Reset font scale
    if (font_size_ > 0) {
        ImGui::SetWindowFontScale(1.0f);
    }

    return true;
}

const char* TextEditorWidget::GetWindowName()
{
    return title_.c_str();
}

void TextEditorWidget::SetFontSize(float size)
{
    font_size_ = size;
}

float TextEditorWidget::GetFontSize() const
{
    return font_size_;
}

void TextEditorWidget::SetReadOnly(bool readonly)
{
    if (editor_) {
        editor_->SetReadOnly(readonly);
    }
}

bool TextEditorWidget::IsReadOnly() const
{
    if (editor_) {
        return editor_->IsReadOnly();
    }
    return false;
}

void TextEditorWidget::UpdateText(const std::string& text)
{
    if (editor_) {
        editor_->SetText(text);
    }
}

void TextEditorWidget::SetLanguage(Language lang)
{
    if (!editor_)
        return;

    switch (lang) {
        case Language::CPlusPlus:
            editor_->SetLanguageDefinition(
                TextEditor::LanguageDefinition::CPlusPlus());
            break;
        case Language::HLSL:
            editor_->SetLanguageDefinition(
                TextEditor::LanguageDefinition::HLSL());
            break;
        case Language::GLSL:
            editor_->SetLanguageDefinition(
                TextEditor::LanguageDefinition::GLSL());
            break;
        case Language::C:
            editor_->SetLanguageDefinition(TextEditor::LanguageDefinition::C());
            break;
        case Language::SQL:
            editor_->SetLanguageDefinition(
                TextEditor::LanguageDefinition::SQL());
            break;
        case Language::AngelScript:
            editor_->SetLanguageDefinition(
                TextEditor::LanguageDefinition::AngelScript());
            break;
        case Language::Lua:
            editor_->SetLanguageDefinition(
                TextEditor::LanguageDefinition::Lua());
            break;
        case Language::XML:
            editor_->SetLanguageDefinition(
                TextEditor::LanguageDefinition::XML());
            break;
    }
}

void TextEditorWidget::BindText(std::string* text_ptr)
{
    bound_text_ = text_ptr;
    if (bound_text_ && editor_) {
        editor_->SetText(*bound_text_);
    }
}

void TextEditorWidget::UnbindText()
{
    bound_text_ = nullptr;
}

std::string TextEditorWidget::GetText() const
{
    if (editor_) {
        return editor_->GetText();
    }
    return "";
}

void TextEditorWidget::SetSaveCallback(
    std::function<void(const std::string&)> callback)
{
    save_callback_ = callback;
}

void TextEditorWidget::ApplyChanges()
{
    if (!editor_)
        return;

    std::string current_text = editor_->GetText();

    // Update bound text if any
    if (bound_text_) {
        *bound_text_ = current_text;
    }

    // Call save callback if set
    if (save_callback_) {
        save_callback_(current_text);
    }
}

RUZINO_NAMESPACE_CLOSE_SCOPE
