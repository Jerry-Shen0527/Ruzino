#include <spdlog/spdlog.h>

#include <memory>
#include <rzpython/interpreter.hpp>
#include <rzpython/rzpython.hpp>

using namespace Ruzino;

int main()
{
    try {
        spdlog::set_level(spdlog::level::debug);

        python::initialize();
        
        auto interpreter = python::CreatePythonInterpreter();
        if (!interpreter) {
            spdlog::warn("Failed to create Python interpreter, skipping test");
            return 0;
        }

        // 测试1: Python内置函数补全
        spdlog::info("=== Test 1: Python builtin autocomplete ===");
        auto suggestions2 = interpreter->Suggest("pri", 3);
        spdlog::info("Got {} suggestions for 'pri':", suggestions2.size());
        for (const auto& suggestion : suggestions2) {
            spdlog::info("  - {}", suggestion);
        }

        // 测试2: 导入模块后的补全
        spdlog::info("\n=== Test 2: Module autocomplete after import ===");
        python::call<void>("import math");
        auto suggestions3 = interpreter->Suggest("math.", 5);
        spdlog::info("Got {} suggestions for 'math.':", suggestions3.size());
        int count = 0;
        for (const auto& suggestion : suggestions3) {
            if (!suggestion.starts_with("__")) {
                spdlog::info("  - {}", suggestion);
                if (++count >= 10)
                    break;
            }
        }

        // 测试3: 自定义变量补全
        spdlog::info("\n=== Test 3: Custom variable autocomplete ===");
        python::call<void>("my_test_var = 123");
        python::call<void>("my_other_var = 'hello'");
        auto suggestions4 = interpreter->Suggest("my_", 3);
        spdlog::info("Got {} suggestions for 'my_':", suggestions4.size());
        for (const auto& suggestion : suggestions4) {
            spdlog::info("  - {}", suggestion);
        }

        spdlog::info("\n=== All tests completed ===");
        return 0;
    }
    catch (const std::exception& e) {
        spdlog::error("Test failed with exception: {}", e.what());
        return 1;
    }
}