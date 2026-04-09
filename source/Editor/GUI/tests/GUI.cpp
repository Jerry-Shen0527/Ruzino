#include <gtest/gtest.h>
#include <nvrhi/nvrhi.h>

#include "GUI/image_widget.hpp"
#include "GUI/text_editor_widget.hpp"
#include "GUI/widget.h"
#include "GUI/window.h"
#include "RHI/rhi.hpp"
#include "imgui.h"

#if RUZINO_WITH_CUDA
#include "RHI/internal/cuda_extension.hpp"
#endif

#ifdef __linux__
#include <cstdlib>
#endif

using namespace Ruzino;

// Helper to skip tests that require a display in headless environments.
static bool is_headless()
{
#ifdef __linux__
    const char* display = std::getenv("DISPLAY");
    return !display || std::string(display).empty();
#else
    return false;
#endif
}

TEST(CreateRHI, window)
{
    if (is_headless()) GTEST_SKIP() << "Skipping: headless environment";
    Window window;
    window.register_function_after_frame([](Window* window) {
        static int frame_count = 0;
        frame_count++;
        if (frame_count > 100) {
            window->close();
        }
    });
    window.run();
}

class Widget : public IWidget {
   public:
    explicit Widget(const char* title) : title(title)
    {
    }

    bool BuildUI() override
    {
        ImGui::Text("Hello, world!");
        return true;
    }

   private:
    std::string title;
};

class WidgetFactory : public IWidgetFactory {
   public:
    std::unique_ptr<IWidget> Create(
        const std::vector<std::unique_ptr<IWidget>>& others) override
    {
        return std::make_unique<Widget>("widget");
    }
};

TEST(CreateRHI, widget_factory)
{
    if (is_headless()) GTEST_SKIP() << "Skipping: headless environment";
    Window window;
    window.register_function_after_frame([](Window* window) {
        static int frame_count = 0;
        frame_count++;
        if (frame_count > 100) {
            window->close();
        }
    });
    window.register_openable_widget(
        std::make_unique<WidgetFactory>(), { "File", "Open", "widget" });
    window.run();
}

TEST(CreateRHI, widget)
{
    if (is_headless()) GTEST_SKIP() << "Skipping: headless environment";
    Window window;
    window.register_function_after_frame([](Window* window) {
        static int frame_count = 0;
        frame_count++;
        if (frame_count > 100) {
            window->close();
        }
    });
    std::unique_ptr<IWidget> widget = std::make_unique<Widget>("widget");
    window.register_widget(std::move(widget));
    window.run();
}

TEST(CreateRHI, multiple_widgets)
{
    if (is_headless()) GTEST_SKIP() << "Skipping: headless environment";
    Window window;
    window.register_function_after_frame([](Window* window) {
        static int frame_count = 0;
        frame_count++;
        if (frame_count > 100) {
            window->close();
        }
    });
    window.register_widget(std::make_unique<Widget>("widget"));
    window.register_widget(std::make_unique<Widget>("widget2"));
    window.run();
}

#include "GUI/ImGuiFileDialog.h"

class FileWidget : public IWidget {
   public:
    explicit FileWidget(const char* title) : title(title)
    {
    }

    bool BuildUI() override
    {
        auto instance = IGFD::FileDialog::Instance();

        instance->Display("SelectFile");

        return true;
    }

   protected:
    const char* GetWindowName() override
    {
        return title.c_str();
    }

   private:
    std::string title;
};

TEST(FileDialog, create_dialog)
{
    if (is_headless()) GTEST_SKIP() << "Skipping: headless environment";
    Window window;
    window.register_function_after_frame([](Window* window) {
        static int frame_count = 0;
        frame_count++;
        if (frame_count > 100) {
            window->close();
        }
    });
    window.register_widget(std::make_unique<FileWidget>("file"));
    window.run();
}

TEST(ImageWidget, red_texture)
{
    if (is_headless()) GTEST_SKIP() << "Skipping: headless environment";
#if RUZINO_WITH_CUDA
    // Initialize RHI and CUDA
    RHI::init(true);
    Ruzino::cuda::cuda_init();

    {
        Window window;
        window.register_function_after_frame([](Window* window) {
            static int frame_count = 0;
            frame_count++;
            if (frame_count > 100) {
                window->close();
            }
        });
        // Create a 256x256 texture using CUDA linear buffer
        const int width = 256;
        const int height = 256;
        const int pixel_count = width * height;

        // Create CUDA linear buffer for RGBA data
        auto cuda_buffer =
            Ruzino::cuda::create_cuda_linear_buffer<uint32_t>(pixel_count);

        // Fill the buffer with red color using CUDA (validation of CUDA
        // filling)
        std::vector<uint32_t> red_data(pixel_count);
        for (int i = 0; i < pixel_count; ++i) {
            // RGBA format as uint32_t: 0xAABBGGRR
            red_data[i] = 0xFF007FFF;  // Alpha=255, Blue=0, Green=127, Red=255
        }
        cuda_buffer->assign_host_vector(red_data);

        // Create texture description
        nvrhi::TextureDesc texture_desc;
        texture_desc.width = width;
        texture_desc.height = height;
        texture_desc.format = nvrhi::Format::RGBA8_UNORM;
        texture_desc.debugName = "cuda_red_texture";
        texture_desc.isRenderTarget = false;
        texture_desc.isShaderResource = true;
        texture_desc.keepInitialState = true;
        texture_desc.initialState = nvrhi::ResourceStates::CopyDest;

        // Convert CUDA linear buffer to NVRHI texture (validating the
        // conversion)
        auto device = RHI::get_device();
        auto texture_handle = Ruzino::cuda::cuda_linear_buffer_to_nvrhi_texture(
            device, cuda_buffer, texture_desc);

        // Create and register the image widget
        auto image_widget = std::make_unique<ImageWidget>(texture_handle);
        window.register_widget(std::move(image_widget));
        window.register_function_after_frame([](Window* window) {
            static int frame_count = 0;
            frame_count++;
            if (frame_count > 100) {
                window->close();
            }
        });

        window.run();
    }  // window is destroyed here, before RHI::shutdown()

    RHI::shutdown();

#else
    // Fallback for non-CUDA builds
    RHI::init(true);

    {
        Window window;
        window.register_function_after_frame([](Window* window) {
            static int frame_count = 0;
            frame_count++;
            if (frame_count > 100) {
                window->close();
            }
        });
        // Create a simple red texture without CUDA
        const int width = 256;
        const int height = 256;
        std::vector<uint8_t> red_data(width * height * 4);

        for (int i = 0; i < width * height; ++i) {
            red_data[i * 4 + 0] = 255;  // Red
            red_data[i * 4 + 1] = 127;  // Green
            red_data[i * 4 + 2] = 0;    // Blue
            red_data[i * 4 + 3] = 255;  // Alpha
        }

        nvrhi::TextureDesc texture_desc;
        texture_desc.width = width;
        texture_desc.height = height;
        texture_desc.format = nvrhi::Format::RGBA8_UNORM;
        texture_desc.debugName = "fallback_red_texture";
        texture_desc.isRenderTarget = false;
        texture_desc.isShaderResource = true;
        texture_desc.keepInitialState = true;
        texture_desc.initialState = nvrhi::ResourceStates::CopyDest;

        auto [texture_handle, staging_handle] =
            RHI::load_texture(texture_desc, red_data.data());

        auto image_widget = std::make_unique<ImageWidget>(texture_handle);
        window.register_widget(std::move(image_widget));
        window.register_function_after_frame([](Window* window) {
            static int frame_count = 0;
            frame_count++;
            if (frame_count > 100) {
                window->close();
            }
        });
        window.run();
    }  // window is destroyed here, before RHI::shutdown()

    RHI::shutdown();
#endif
}

TEST(TextEditor, basic_editor)
{
    if (is_headless()) GTEST_SKIP() << "Skipping: headless environment";
    Window window;
    window.register_function_after_frame([](Window* window) {
        static int frame_count = 0;
        frame_count++;
        if (frame_count > 100) {
            window->close();
        }
    });
    window.register_widget(std::make_unique<TextEditorWidget>("Code Editor"));
    window.run();
}

TEST(TextEditor, advanced_features)
{
    if (is_headless()) GTEST_SKIP() << "Skipping: headless environment";
    Window window;
    window.register_function_after_frame([](Window* window) {
        static int frame_count = 0;
        frame_count++;
        if (frame_count > 100) {
            window->close();
        }
    });

    // Create a text editor widget
    auto editor_widget = std::make_unique<TextEditorWidget>("Advanced Editor");

    // Set custom font size
    editor_widget->SetFontSize(16.0f);

    // Bind to an external string for editing
    static std::string my_code = R"(// Edit this code!
#include <iostream>

int main() {
    std::cout << "Hello from bound text!" << std::endl;
    return 0;
})";

    editor_widget->BindText(&my_code);

    // Set a save callback
    editor_widget->SetSaveCallback([](const std::string& text) {
        std::cout << "Text saved! Length: " << text.length() << " bytes"
                  << std::endl;
        std::cout << "Content preview: " << text.substr(0, 50) << "..."
                  << std::endl;
    });

    window.register_widget(std::move(editor_widget));
    window.run();

    // After window closes, the bound text will contain any edits made
    std::cout << "Final text:\n" << my_code << std::endl;
}

TEST(TextEditor, xml_support)
{
    if (is_headless()) GTEST_SKIP() << "Skipping: headless environment";
    Window window;
    window.register_function_after_frame([](Window* window) {
        static int frame_count = 0;
        frame_count++;
        if (frame_count > 100) {
            window->close();
        }
    });

    auto xml_editor = std::make_unique<TextEditorWidget>("XML Editor");

    // Set XML language
    xml_editor->SetLanguage(TextEditorWidget::Language::XML);

    // Set sample XML text
    static std::string xml_text = R"(<?xml version="1.0" encoding="UTF-8"?>
<!-- This is a comment -->
<root>
    <element attribute="value">
        <child>Content here</child>
        <empty />
    </element>
    <data>
        Special characters: &lt; &gt; &amp; &quot; &apos;
    </data>
</root>)";

    xml_editor->BindText(&xml_text);
    xml_editor->SetFontSize(16.0f);

    xml_editor->SetSaveCallback([](const std::string& text) {
        std::cout << "XML saved!" << std::endl;
    });

    window.register_widget(std::move(xml_editor));
    window.run();
}
