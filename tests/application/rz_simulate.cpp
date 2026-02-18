#define _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

// Framework includes
#include <spdlog/spdlog.h>

#include "GCore/GOP.h"
#include "RHI/rhi.hpp"
#include "cmdparser.hpp"
#include "nodes/system/node_system.hpp"
#include "stage/stage.hpp"
#include "usd_nodejson.hpp"

// USD includes
#include "pxr/base/tf/setenv.h"
#include "pxr/usd/usd/stage.h"

#include <rzpython/rzpython.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace Ruzino;
using namespace pxr;

int main(int argc, char* argv[])
{
    python::initialize();
#ifdef _WIN32
    // 禁止 abort 弹窗，改为直接退出
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // 或者设置错误模式，避免 Windows 弹窗
    _set_error_mode(_OUT_TO_STDERR);
#endif

    // 解除 C++ 流与 C 流的同步以加速输出
    std::ios_base::sync_with_stdio(false);

    // Parse command line using cmdparser
    cmdline::parser parser;
    parser.add<std::string>("usd", 'u', "USD scene file", true);
    parser.add<int>(
        "frames",
        'f',
        "Number of frames to simulate",
        false,
        10);
    parser.add<float>(
        "fps",
        'r',
        "Frames per second (simulation delta time)",
        false,
        60.0f);
    parser.add("verbose", 'v', "Enable verbose logging");

    parser.parse_check(argc, argv);

    // Extract settings
    std::string usd_file = parser.get<std::string>("usd");
    bool verbose = parser.exist("verbose");
    int num_frames = parser.get<int>("frames");
    float fps = parser.get<float>("fps");
    float delta_time = 1.0f / fps;

    // Validate input files
    if (!std::filesystem::exists(usd_file)) {
        std::cerr << "Error: USD file not found: " << usd_file << std::endl;
        return 1;
    }

    // Initialize logging
    spdlog::set_level(verbose ? spdlog::level::info : spdlog::level::warn);
    spdlog::set_pattern("%^[%T] %n: %v%$");

    spdlog::info("Starting simulation...");
    spdlog::info("USD file: {}", usd_file);
    spdlog::info("Frames: {}", num_frames);
    spdlog::info("FPS: {} (dt={:.4f}s)", fps, delta_time);

    try {
        // Initialize RHI (headless mode for GPU simulation)
        RHI::init(false, true);  // with_window=false (headless), use_dx12=true

        // Create USD stage
        auto stage = create_custom_global_stage(usd_file);
        if (!stage) {
            throw std::runtime_error(
                "Failed to load USD stage from " + usd_file);
        }
        
        printf("Starting simulation...\n");
        printf("Total frames: %d, Delta time: %.4fs (%.0f fps)\n",
               num_frames,
               delta_time,
               fps);
        fflush(stdout);

        auto total_start_time = std::chrono::high_resolution_clock::now();

        // Simulation loop
        for (int frame = 0; frame < num_frames; ++frame) {
            auto frame_start = std::chrono::high_resolution_clock::now();
            
            // Update simulation (skip first frame)
            if (frame > 0) {
                stage->tick(delta_time);
                stage->finish_tick();
            }

            // Set time code
            pxr::UsdTimeCode time_code(frame * delta_time);
            stage->set_render_time(time_code);

            auto frame_end = std::chrono::high_resolution_clock::now();
            auto frame_duration =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    frame_end - frame_start)
                    .count();

            printf(
                "\r[Frame %d/%d] Simulation time: %.4fs",
                frame + 1,
                num_frames,
                frame_duration / 1000.0);
            fflush(stdout);
        }
        printf("\n");

        auto total_end_time = std::chrono::high_resolution_clock::now();
        auto total_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                total_end_time - total_start_time)
                .count();

        printf("\n========================================\n");
        printf("Simulation completed successfully!\n");
        printf("Total frames simulated: %d\n", num_frames);
        printf(
            "Total time: %.2fs (%.4fs per frame)\n",
            total_duration / 1000.0,
            total_duration / 1000.0 / num_frames);
        printf("========================================\n");
        fflush(stdout);

        // Cleanup
        stage.reset();
        unregister_cpp_type();
        printf("Successfully finished all operations.\n");
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    python::finalize();
}
