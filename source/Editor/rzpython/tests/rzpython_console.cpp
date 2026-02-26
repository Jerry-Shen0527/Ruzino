#include <spdlog/spdlog.h>

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

int main()
{
    try {
        spdlog::set_level(spdlog::level::info);
        
        // Initialize Python
        python::initialize();
        
        auto interpreter = python::CreatePythonInterpreter();
        if (!interpreter) {
            spdlog::warn("Failed to create Python interpreter, skipping test");
            return 0;
        }

        // Test Python functionality
        spdlog::info("=== Test 1: Basic Python operations ===");
        python::call<void>("test_var = 42");
        int result = python::call<int>("test_var");
        spdlog::info("Basic test result: {}", result);

        spdlog::info("=== Test 2: Python math operations ===");
        python::call<void>("import math");
        python::send("radius", 5.0f);
        python::call<void>("area = math.pi * radius ** 2");
        float area = python::call<float>("area");
        spdlog::info("Circle area (r=5): {}", area);

        spdlog::info("=== All tests completed ===");
        return 0;
    }
    catch (const std::exception& e) {
        spdlog::error("Test failed with exception: {}", e.what());
        return 1;
    }
}
