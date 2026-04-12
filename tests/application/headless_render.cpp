#define _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING

#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// Framework includes
#include <spdlog/spdlog.h>

#include "GCore/GOP.h"
#include "GCore/algorithms/intersection.h"
#include "RHI/rhi.hpp"
#include "cmdparser.hpp"
#include "nodes/core/logging.hpp"
#include "nodes/system/node_system.hpp"
#include "render_util.hpp"
#include "stage/stage.hpp"

// USD includes
#include <rzpython/rzpython.hpp>

#include "pxr/base/tf/setenv.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/camera.h"

// Hydra includes
#include "pxr/base/gf/camera.h"
#include "pxr/base/gf/frustum.h"
#include "pxr/imaging/hd/driver.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdx/tokens.h"
#include "pxr/imaging/hgi/tokens.h"
#include "pxr/usdImaging/usdImagingGL/engine.h"

// NVRHI includes
#include "nvrhi/nvrhi.h"

// Image saving
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "pxr/imaging/garch/glApi.h"
#include "stb_image_write.h"

// USD Hio for HDR/EXR support

#ifdef _WIN32
#include <gl/GL.h>
#include <windows.h>

#endif

using namespace Ruzino;
using namespace pxr;
using namespace RenderUtil;

int main(int argc, char* argv[])
{
    initialize_framework_logging("headless_render", spdlog::level::info);
    python::initialize();

    // 禁止 abort 弹窗，改为直接退出
    // _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // 或者设置错误模式，避免 Windows 弹窗
    // _set_error_mode(_OUT_TO_STDERR);

    // 解除 C++ 流与 C 流的同步以加速输出
    std::ios_base::sync_with_stdio(false);

    // Parse command line using cmdparser
    cmdline::parser parser;
    parser.add<std::string>("usd", 'u', "USD file to render", true);
    parser.add<std::string>(
        "json",
        'j',
        "JSON rendering script (required for Ruzino renderer)",
        false,
        "");
    parser.add<std::string>(
        "output", 'o', "Output image filename (PNG/HDR/EXR)", true);
    parser.add<int>("width", 'w', "Image width", false, 1920);
    parser.add<int>("height", 'h', "Image height", false, 1080);
    parser.add<int>("spp", 's', "Samples per pixel", false, 16);
    parser.add<std::string>(
        "camera", 'c', "Camera prim path (e.g., /Camera)", false, "");
    parser.add<int>(
        "renderer",
        'd',
        "Renderer index (0=Storm/first available, 1=Ruzino). Default: "
        "auto-select Ruzino if available",
        false,
        -1);
    parser.add("verbose", 'v', "Enable verbose logging");
    parser.add<int>(
        "frames",
        'f',
        "Number of frames to render (for animation sequences)",
        false,
        0);
    parser.add<float>(
        "fps",
        'r',
        "Frames per second (for animation delta time)",
        false,
        60.0f);
    parser.add("no-save", 'n', "Skip saving images (for profiling)");
    parser.add("no-progress", 'p', "Disable progress bar display");

    parser.parse_check(argc, argv);
    const bool verbose = parser.exist("verbose");

    initialize_framework_logging(
        "headless_render",
        verbose ? spdlog::level::info : spdlog::level::warn);

    // Set MaterialX standard library path using USD's TfSetenv (preferred
    // method)
    std::string mtlx_stdlib = "libraries";
    if (std::filesystem::exists(mtlx_stdlib)) {
        pxr::TfSetenv("PXR_MTLX_STDLIB_SEARCH_PATHS", mtlx_stdlib.c_str());
        spdlog::info("Set PXR_MTLX_STDLIB_SEARCH_PATHS={}", mtlx_stdlib);
    }
    else {
        spdlog::warn("MaterialX stdlib not found at {}", mtlx_stdlib);
    }

    // Extract settings
    std::string usd_file = parser.get<std::string>("usd");
    std::string json_script = parser.get<std::string>("json");
    std::string output_image = parser.get<std::string>("output");
    int width = parser.get<int>("width");
    int height = parser.get<int>("height");
    int spp = parser.get<int>("spp");
    std::string camera_path = parser.get<std::string>("camera");
    int renderer_index = parser.get<int>("renderer");
    bool skip_save = parser.exist("no-save");
    bool show_progress = !parser.exist("no-progress");
    int num_frames = parser.get<int>("frames");
    float fps = parser.get<float>("fps");
    float delta_time = 1.0f / fps;

    // Validate input files
    if (!std::filesystem::exists(usd_file)) {
        spdlog::error("USD file not found: {}", usd_file);
        return 1;
    }
    // JSON validation will be done later based on selected renderer

    spdlog::info("Starting headless render...");
    spdlog::info("USD file: {}", usd_file);
    spdlog::info("JSON script: {}", json_script);
    spdlog::info("Output image: {}", output_image);
    spdlog::info("Resolution: {}x{}", width, height);
    spdlog::info("SPP: {}", spp);

    try {
        // Initialize RHI first (headless mode, with DX12 backend)
        // This must happen before any USD rendering operations
        RHI::init(false, true);  // with_window=false (headless), use_dx12=true

        // Initialize OpenGL context
        CreateGLContext();
        GarchGLApiLoad();

        // Create USD stage
        auto stage = create_custom_global_stage(usd_file);
        stage->save_on_destruct = false;
        if (!stage) {
            throw std::runtime_error(
                "Failed to load USD stage from " + usd_file);
        }

        // Find camera
        auto camera = GetCamera(stage->get_usd_stage(), camera_path);
        if (!camera) {
            throw std::runtime_error("No camera found in USD file");
        }

        // Setup rendering engine
        auto hgi = Hgi::CreateNamedHgi(HgiTokens->OpenGL);
        HdDriver hd_driver;
        hd_driver.name = HgiTokens->renderDriver;
        hd_driver.driver = VtValue(hgi.get());

        UsdImagingGLEngine::Parameters params;
        params.allowAsynchronousSceneProcessing = false;
        params.driver = hd_driver;

        auto renderer = std::make_unique<UsdImagingGLEngine>(params);

        // Get available renderers
        auto available_renderers = renderer->GetRendererPlugins();
        spdlog::info("Available renderers:");
        for (size_t i = 0; i < available_renderers.size(); ++i) {
            spdlog::info("  [{}] {}", i, available_renderers[i].GetString());
        }

        // Select renderer
        int selected_renderer = renderer_index;
        if (selected_renderer < 0) {
            // Auto-select: prefer Ruzino renderer
            selected_renderer = 0;
            for (size_t i = 0; i < available_renderers.size(); ++i) {
                if (available_renderers[i].GetString() ==
                    "Hd_RUZINO_RendererPlugin") {
                    selected_renderer = i;
                    break;
                }
            }
        }

        if (selected_renderer >= static_cast<int>(available_renderers.size())) {
            spdlog::error(
                "Renderer index {} is out of range. available_renderers={}",
                selected_renderer,
                available_renderers.size());
            return 1;
        }

        renderer->SetRendererPlugin(available_renderers[selected_renderer]);
        spdlog::info(
            "Selected renderer: [{}] {}",
            selected_renderer,
            available_renderers[selected_renderer].GetString());

        bool is_ruzino_renderer =
            (available_renderers[selected_renderer].GetString() ==
             "Hd_RUZINO_RendererPlugin");
        bool is_storm_renderer =
            (selected_renderer == 0 && !is_ruzino_renderer);

        renderer->SetEnablePresentation(false);

        // Configure render settings
        GfVec2i render_size(width, height);
        renderer->SetRenderBufferSize(render_size);
        renderer->SetRenderViewport(GfVec4d(0.0, 0.0, width, height));

        // Setup camera
        auto gf_camera = camera.GetCamera(UsdTimeCode::Default());
        auto frustum = gf_camera.GetFrustum();
        renderer->SetCameraState(
            frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix());

        // Configure render parameters
        UsdImagingGLRenderParams render_params;
        render_params.enableLighting = true;
        render_params.enableSceneMaterials = true;
        render_params.showRender = true;
        render_params.frame = UsdTimeCode(0);
        render_params.drawMode =
            UsdImagingGLDrawMode::DRAW_WIREFRAME_ON_SURFACE;
        render_params.colorCorrectionMode = HdxColorCorrectionTokens->disabled;
        render_params.clearColor = GfVec4f(1.f, 1.f, 1.f, 0.0f);
        renderer->SetRendererAov(HdAovTokens->color);

        // Load and apply JSON script (only for Ruzino renderer)
        if (is_ruzino_renderer) {
            if (json_script.empty() || !std::filesystem::exists(json_script)) {
                spdlog::error(
                    "JSON script required for Ruzino renderer but not found: {}",
                    json_script);
                return 1;
            }

            auto node_system = static_cast<const std::shared_ptr<NodeSystem>*>(
                renderer->GetRendererSetting(pxr::TfToken("RenderNodeSystem"))
                    .Get<const void*>());

            if (node_system) {
                std::string nodes_json = LoadJSONScript(json_script);
                (*node_system)->get_node_tree()->deserialize(nodes_json);
                spdlog::info("Loaded JSON script: {}", json_script);
            }
        }

        GlfSimpleLightVector lights;

        if (is_storm_renderer) {
            lights = GlfSimpleLightVector(1);
            auto cam_pos = frustum.GetPosition();
            lights[0].SetPosition(
                GfVec4f{ float(cam_pos[0]),
                         float(cam_pos[1]),
                         float(cam_pos[2]),
                         1.0f });
            lights[0].SetAmbient(GfVec4f(0.8, 0.8, 0.8, 1));
            lights[0].SetDiffuse(GfVec4f(1.0f));
            lights[0].SetSpecular(GfVec4f(0.0f));
        }
        GlfSimpleMaterial material;
        float kA = 6.8f;
        float kS = 0.4f;
        float shiness = 0.8f;
        material.SetDiffuse(GfVec4f(kA, kA, kA, 1.0f));
        material.SetSpecular(GfVec4f(kS, kS, kS, 1.0f));
        material.SetShininess(shiness);
        GfVec4f sceneAmbient = { 1.0, 1.0, 1.0, 1.0 };
        renderer->SetLightingState(lights, material, sceneAmbient);

        // Determine if we're rendering a sequence or single frame
        bool is_sequence = (num_frames > 0);
        int frames_to_render = is_sequence ? num_frames : 1;

        printf(
            "Starting %s render...\n",
            is_sequence ? "sequence" : "single frame");
        if (is_sequence) {
            printf(
                "Total frames: %d, Delta time: %.4fs (%.0f fps)\n",
                frames_to_render,
                delta_time,
                fps);
        }
        printf("Samples per pixel: %d\n", spp);
        fflush(stdout);

        // Track async save operations
        std::future<void> previous_save_task;
        bool has_previous_task = false;

        // Track total time for multi-frame rendering
        auto total_start_time = std::chrono::high_resolution_clock::now();

        // Render loop for each frame
        for (int frame = 0; frame < frames_to_render; ++frame) {
            // Update stage for animation (including first frame for sequences)
            if (is_sequence) {
                stage->tick(delta_time);
                stage->finish_tick();
            }

            // Set time code
            pxr::UsdTimeCode time_code(frame * delta_time);
            stage->set_render_time(time_code);
            render_params.frame = time_code;

            // Render the scene with multiple samples
            UsdPrim root = stage->get_usd_stage()->GetPseudoRoot();

            // Start timing (will be set after first sample)
            auto render_start = std::chrono::high_resolution_clock::now();
            long long total_sample_time = 0;
            int timed_samples = 0;

            // Storm renderer: single render pass, Ruzino: use spp
            int samples_to_render = is_storm_renderer ? 1 : spp;

            for (int sample = 0; sample < samples_to_render; ++sample) {
                auto sample_start = std::chrono::high_resolution_clock::now();

                renderer->Render(root, render_params);

                // Wait for idle and cleanup only for Ruzino renderer
                if (is_ruzino_renderer) {
                    RHI::get_device()->waitForIdle();
                    RHI::get_device()->runGarbageCollection();
                }

                renderer->StopRenderer();

                auto sample_end = std::chrono::high_resolution_clock::now();
                auto sample_duration =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        sample_end - sample_start)
                        .count();

                // Skip first sample for timing (shader compilation, etc.) -
                // only for Ruzino
                if (sample == 0 && frame == 0 && is_ruzino_renderer) {
                    render_start = std::chrono::high_resolution_clock::now();
                    if (show_progress) {
                        printf(
                            "Sample 1/%d completed in %.2fs (warmup)\n",
                            spp,
                            sample_duration / 1000.0);
                        fflush(stdout);
                    }
                    continue;
                }

                total_sample_time += sample_duration;
                timed_samples++;

                if (show_progress && is_ruzino_renderer) {
                    // Calculate progress and ETA (based on samples after
                    // warmup)
                    int progress_percent = ((sample + 1) * 100) / spp;
                    double avg_time_per_sample =
                        (double)total_sample_time / timed_samples;
                    int remaining_samples = spp - (sample + 1);
                    double eta_seconds =
                        (avg_time_per_sample * remaining_samples) / 1000.0;

                    // Create progress bar
                    const int bar_width = 40;
                    int filled = (bar_width * (sample + 1)) / spp;
                    char bar[bar_width + 1];
                    memset(bar, ' ', bar_width);
                    for (int i = 0; i < filled; ++i) {
                        bar[i] = '=';
                    }
                    if (filled < bar_width) {
                        bar[filled] = '>';
                    }
                    bar[bar_width] = '\0';

                    // Format ETA
                    int eta_minutes = (int)(eta_seconds / 60);
                    int eta_secs = (int)(eta_seconds) % 60;

                    // Print progress bar with ETA using printf
                    // Include frame info in sequence mode
                    if (is_sequence) {
                        printf(
                            "\r[Frame %d/%d] [%s] %d%% (%d/%d) Sample: %.4fs "
                            "Avg: "
                            "%.4fs ",
                            frame + 1,
                            frames_to_render,
                            bar,
                            progress_percent,
                            sample + 1,
                            spp,
                            sample_duration / 1000.0,
                            avg_time_per_sample / 1000.0);
                    }
                    else {
                        printf(
                            "\r[%s] %d%% (%d/%d) Sample: %.4fs Avg: %.4fs ",
                            bar,
                            progress_percent,
                            sample + 1,
                            spp,
                            sample_duration / 1000.0,
                            avg_time_per_sample / 1000.0);
                    }

                    if (remaining_samples > 0) {
                        if (eta_minutes > 0) {
                            printf("ETA: %dm %ds", eta_minutes, eta_secs);
                        }
                        else {
                            printf("ETA: %ds", eta_secs);
                        }
                    }
                    else {
                        printf("Complete!");
                    }

                    fflush(stdout);
                }
            }
            if (show_progress && is_ruzino_renderer) {
                printf("\n");
            }

            auto render_end = std::chrono::high_resolution_clock::now();
            auto total_duration =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    render_end - render_start)
                    .count();

            if (!is_sequence) {
                if (is_storm_renderer) {
                    printf(
                        "Render complete. Total time: %.2fs\n",
                        total_duration / 1000.0);
                }
                else {
                    printf(
                        "Render complete. Total time: %.2fs (excluding warmup)",
                        total_duration / 1000.0);
                    if (timed_samples > 0) {
                        printf(
                            ", Avg per sample: %.2fs",
                            total_sample_time / (double)timed_samples / 1000.0);
                    }
                    printf("\n");
                }
                fflush(stdout);
            }

            // Read back texture data
            std::vector<uint8_t> texture_data;
            HgiFormat texture_format = HgiFormatFloat32Vec4;  // Default

            // Try to get the actual texture format
            auto hgi_texture = renderer->GetAovTexture(HdAovTokens->color);
            if (hgi_texture) {
                texture_format = hgi_texture->GetDescriptor().format;
                spdlog::info(
                    "Detected texture format: {}",
                    static_cast<int>(texture_format));
            }

            bool success = ReadTextureDirectly(
                renderer.get(), width, height, texture_data);

            if (!success) {
                success = ReadTextureCPU(
                    renderer.get(), hgi, width, height, texture_data);
            }

            if (!success) {
                throw std::runtime_error(
                    "Failed to read back rendered texture");
            }

            // Generate output filename for this frame
            std::string frame_output =
                GenerateSequenceFilename(output_image, frame, frames_to_render);

            // Launch async save task (capture by value to avoid data races)
            previous_save_task = std::async(
                std::launch::async,
                [frame_output,
                 width,
                 height,
                 texture_data,
                 texture_format,
                 skip_save]() {
                    auto save_start = std::chrono::high_resolution_clock::now();

                    if (!SaveImageToFile(
                            frame_output,
                            width,
                            height,
                            texture_data,
                            texture_format,
                            skip_save)) {
                        fprintf(
                            stderr,
                            "Error: Failed to save image to %s\n",
                            frame_output.c_str());
                    }

                    auto save_end = std::chrono::high_resolution_clock::now();
                    auto save_duration =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            save_end - save_start)
                            .count();

                    fflush(stdout);
                });
            has_previous_task = true;
        }

        // Wait for the last save task to complete
        if (has_previous_task) {
            printf("\nWaiting for final image save to complete...\n");
            fflush(stdout);
            previous_save_task.wait();
        }

        auto total_end_time = std::chrono::high_resolution_clock::now();
        auto total_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                total_end_time - total_start_time)
                .count();

        if (is_sequence) {
            printf("\n========================================\n");
            printf("Sequence render completed successfully!\n");
            printf("Total frames rendered: %d\n", frames_to_render);
            printf(
                "Total time: %.2fs (%.2fs per frame)\n",
                total_duration / 1000.0,
                total_duration / 1000.0 / frames_to_render);
            printf("========================================\n");
        }
        else {
            printf("Headless render completed successfully!\n");
        }
        fflush(stdout);

        // Cleanup
        renderer.reset();
        hgi.reset();
        stage.reset();
        unregister_cpp_type();
#ifdef GPU_GEOM_ALGORITHM
        deinit_gpu_geometry_algorithms();
#endif
        // Shutdown RHI at the end
        printf("Successfully finished all operations.\n");
        return 0;
    }
    catch (const std::exception& e) {
        log_exception_with_context("headless_render failed", e);
        return 1;
    }

    python::finalize();
}
