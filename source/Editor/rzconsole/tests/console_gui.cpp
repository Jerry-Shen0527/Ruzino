#include <GUI/widget.h>
#include <GUI/window.h>
#include <gtest/gtest.h>
#include <rzconsole/ConsoleInterpreter.h>
#include <rzconsole/ConsoleObjects.h>
#include <rzconsole/imgui_console.h>
#include <rzconsole/spdlog_console_sink.h>
#include <spdlog/spdlog.h>

#include <memory>

#ifdef __linux__
#include <cstdlib>
#include <iostream>
#endif

using namespace Ruzino;

class ConsoleWidgetFactory : public IWidgetFactory {
   public:
    std::unique_ptr<IWidget> Create(
        const std::vector<std::unique_ptr<IWidget>>& others) override
    {
        auto interpreter = std::make_shared<console::Interpreter>();

        // Register some test commands
        console::CommandDesc test_cmd = {
            "test",
            "A test command that prints hello world",
            [](console::Command::Args const& args) -> console::Command::Result {
                return { true, "Hello from test command!\n" };
            }
        };
        console::RegisterCommand(test_cmd);

        console::CommandDesc log_test_cmd = {
            "log_test",
            "Test spdlog integration",
            [](console::Command::Args const& args) -> console::Command::Result {
                spdlog::info("This is an info message");
                spdlog::warn("This is a warning message");
                spdlog::error("This is an error message");
                return { true, "Log test messages sent\n" };
            }
        };
        console::RegisterCommand(log_test_cmd);

        // Create console with capture_log enabled
        ImGui_Console::Options opts;
        opts.show_info = true;
        opts.show_warnings = true;
        opts.show_errors = true;
        opts.capture_log = true;  // Enable spdlog integration

        auto console = std::make_unique<ImGui_Console>(interpreter, opts);

        // Add some initial messages
        console->Print("Console initialized successfully!");
        console->Print(
            "Spdlog capture is enabled - try 'log_test' to see captured logs");
        console->Print("Type 'help' for available commands");

        return std::move(console);
    }
};

int main()
{
#ifdef __linux__
    const char* display = std::getenv("DISPLAY");
    if (!display || std::string(display).empty()) {
        std::cout << "Skipping: no DISPLAY available (headless environment)" << std::endl;
        return 0;
    }
#endif
    // Create console with capture disabled to demonstrate the difference
    auto interpreter = std::make_shared<console::Interpreter>();

    // Register a simple test command
    console::CommandDesc echo_cmd = {
        "echo",
        "Echo text to console",
        [](console::Command::Args const& args) -> console::Command::Result {
            std::string output;
            for (size_t i = 1; i < args.size(); ++i) {
                output += args[i] + " ";
            }
            return { true, output + "\n" };
        }
    };
    console::RegisterCommand(echo_cmd);

    ImGui_Console::Options opts;
    opts.capture_log = true;  // Disable spdlog integration initially
    auto console = std::make_unique<ImGui_Console>(interpreter, opts);

    console->Print("Direct widget console test");
    console->Print(
        "Spdlog capture is DISABLED - spdlog messages won't appear here");
    console->Print(
        "Use 'enable_capture' command or console->SetLogCapture(true) to "
        "enable");

    Window window;
    // Don't setup console logging since capture_log is false
    // setup_console_logging(console.get());  // Remove this line

    window.register_widget(std::move(console));
    window.register_function_after_frame([](Window* window) {
        static int frame_count = 0;
        frame_count++;
        if (frame_count > 100) {
            window->close();
        }
    });
    window.run();
}
