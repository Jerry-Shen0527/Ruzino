#pragma once

#include <memory>

#include "GUI/api.h"
#include "GUI/widget.h"

// Forward declaration - TextEditor is in global namespace
class TextEditor;

RUZINO_NAMESPACE_OPEN_SCOPE

class GUI_API TextEditorWidget : public IWidget {
   public:
    explicit TextEditorWidget(const std::string& title = "Text Editor");
    ~TextEditorWidget() override;

    bool BuildUI() override;

    // Font control
    void SetFontSize(float size);
    float GetFontSize() const;

    // Language support
    enum class Language {
        CPlusPlus,
        HLSL,
        GLSL,
        C,
        SQL,
        AngelScript,
        Lua,
        XML
    };
    void SetLanguage(Language lang);

    // Read-only mode
    void SetReadOnly(bool readonly);
    bool IsReadOnly() const;

    // Update text without triggering callbacks
    void UpdateText(const std::string& text);

    // Text binding - bind to an external string for editing
    void BindText(std::string* text_ptr);
    void UnbindText();

    // Get current text
    std::string GetText() const;

    // Save callback - called when user wants to save changes
    void SetSaveCallback(std::function<void(const std::string&)> callback);

    // Apply current changes to bound text (if any)
    void ApplyChanges();

   protected:
    const char* GetWindowName() override;

   private:
    std::string title_;
    std::unique_ptr<TextEditor> editor_;

    // Font size
    float font_size_ = 14.0f;

    // Text binding
    std::string* bound_text_ = nullptr;

    // Save callback
    std::function<void(const std::string&)> save_callback_;
};

RUZINO_NAMESPACE_CLOSE_SCOPE
