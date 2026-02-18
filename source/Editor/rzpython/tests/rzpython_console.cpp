#include <GUI/widget.h>
#include <GUI/window.h>
#include <gtest/gtest.h>
#include <rzconsole/ConsoleInterpreter.h>
#include <rzconsole/ConsoleObjects.h>
#include <rzconsole/imgui_console.h>
#include <rzconsole/spdlog_console_sink.h>

#include <memory>
#include <rzpython/interpreter.hpp>
#include <rzpython/rzpython.hpp>

// Check if we're running in a headless environment
bool is_headless_environment()
{
#ifdef _WIN32
    return false;  // Windows usually has a display
#else
    // On Linux, check if DISPLAY environment variable is set
    const char* display = std::getenv("DISPLAY");
    return display == nullptr || std::string(display).empty();
#endif
}

using namespace Ruzino;

class PythonConsoleWidgetFactory : public IWidgetFactory {
   public:
    std::unique_ptr<IWidget> Create(
        const std::vector<std::unique_ptr<IWidget>>& others) override
    {
        // Create Python interpreter
        auto interpreter = python::CreatePythonInterpreter();

        // Register some test commands that work with Python
        console::CommandDesc test_cmd = {
            "test",
            "A test command that demonstrates Python integration",
            [](console::Command::Args const& args) -> console::Command::Result {
                try {
                    // Use Python to do some computation
                    python::send("test_value", 42);
                    python::call<void>("test_result = test_value * 2");
                    int result = python::call<int>("test_result");

                    return { true,
                             "Test result from Python: " +
                                 std::to_string(result) + "\n" };
                }
                catch (const std::exception& e) {
                    return { false,
                             "Python test failed: " + std::string(e.what()) +
                                 "\n" };
                }
            }
        };
        console::RegisterCommand(test_cmd);

        // Create console with capture_log enabled
        ImGui_Console::Options opts;
        opts.show_info = true;
        opts.show_warnings = true;
        opts.show_errors = true;
        opts.capture_log = true;

        auto console = std::make_unique<ImGui_Console>(interpreter, opts);

        // Add some initial messages
        console->Print("Python Console initialized successfully!");
        console->Print(
            "You can now enter Python code directly (e.g., 'x = 5')");
        console->Print("Or use console commands like 'help', 'test'");
        console->Print("Try: python print('Hello from Python!')");

        return std::move(console);
    }
};

int main()
{
    // Create Python console test
    auto interpreter = python::CreatePythonInterpreter();

    // Add simple debug command to test if commands work at all
    console::CommandDesc simple_test_cmd = {
        "simple_test",
        "Simple test command",
        [](console::Command::Args const& args) -> console::Command::Result {
            return { true, "Simple test command works!\n" };
        }
    };
    console::RegisterCommand(simple_test_cmd);

    // Test Python functionality
    console::CommandDesc math_cmd = {
        "math_test",
        "Test Python math operations",
        [](console::Command::Args const& args) -> console::Command::Result {
            try {
                python::call<void>("import math");
                python::send("radius", 5.0f);
                python::call<void>("area = math.pi * radius ** 2");
                float area = python::call<float>("area");

                return { true,
                         "Circle area (r=5): " + std::to_string(area) + "\n" };
            }
            catch (const std::exception& e) {
                return { false,
                         "Math test failed: " + std::string(e.what()) + "\n" };
            }
        }
    };
    console::RegisterCommand(math_cmd);

    // Add debug command to test Python interpreter directly
    console::CommandDesc debug_cmd = {
        "debug_python",
        "Debug Python interpreter state",
        [](console::Command::Args const& args) -> console::Command::Result {
            try {
                std::string debug_info = "Python initialized: ";
                debug_info += python::initialized ? "Yes" : "No";
                debug_info += "\nTesting basic operation...\n";

                python::call<void>("test_var = 42");
                int result = python::call<int>("test_var");
                debug_info +=
                    "Basic test result: " + std::to_string(result) + "\n";

                return { true, debug_info };
            }
            catch (const std::exception& e) {
                return { false,
                         "Debug failed: " + std::string(e.what()) + "\n" };
            }
        }
    };
    console::RegisterCommand(debug_cmd);

    ImGui_Console::Options opts;
    opts.capture_log = true;
    auto console = std::make_unique<ImGui_Console>(interpreter, opts);

    // Setup console logging AFTER creating the console
    setup_console_logging(console.get());

    console->Print("=== Python Interactive Console Test ===");
    console->Print("FIRST: Test basic console commands:");
    console->Print("  simple_test      # Test if console commands work");
    console->Print("  debug_python     # Test Python state");
    console->Print("  math_test        # Test Python math");
    console->Print("");
    console->Print("THEN: Test Python commands directly (these should work):");
    console->Print("  x = 10           # Python assignment");
    console->Print("  x                # Python variable lookup");
    console->Print("  print('hello')   # Python function call");
    console->Print("  2 + 3            # Python expression");
    console->Print("");
    console->Print("If Python commands don't work, there's a problem!");
    console->Print("Try 'simple_test' first to verify console is working.");

    Window window;
    python::reference("w", &window);
    window.register_widget(std::move(console));
    window.register_function_after_frame([](Window* window) {
        static int frame_count = 0;
        frame_count++;
        if (frame_count > 100) {
            window->close();
        }
    });
    window.run();

    return 0;
}
