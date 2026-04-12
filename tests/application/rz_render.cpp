#define _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Framework includes
#include <spdlog/spdlog.h>

#include "RHI/rhi.hpp"
#include "cmdparser.hpp"
#include "nodes/core/logging.hpp"
#include "nodes/system/node_system.hpp"
#include "stage/stage.hpp"
#include "usd_nodejson.hpp"

// USD includes
#include "pxr/base/tf/setenv.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/camera.h"

// Hydra includes
#include "pxr/base/gf/camera.h"
#include "pxr/base/gf/frustum.h"
#include "pxr/imaging/hd/driver.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdx/tokens.h"
#include "pxr/imaging/hgi/blitCmdsOps.h"
#include "pxr/imaging/hgi/tokens.h"
#include "pxr/usdImaging/usdImagingGL/engine.h"

// NVRHI includes
#include "nvrhi/nvrhi.h"

// Image saving
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "pxr/imaging/garch/glApi.h"
#include "stb_image_write.h"

// USD Hio for HDR/EXR support
#include "pxr/imaging/hio/image.h"
#include "pxr/imaging/hio/types.h"

#ifdef _WIN32
#include <gl/GL.h>
#include <windows.h>

#endif

using namespace Ruzino;
using namespace pxr;

// USD utilities
UsdGeomCamera GetCamera(
    const UsdStageRefPtr& stage,
    const std::string& camera_path)
{
    // First, collect all available cameras
    std::vector<std::string> available_cameras;
    for (const UsdPrim& prim : stage->Traverse()) {
        if (prim.IsA<UsdGeomCamera>()) {
            available_cameras.push_back(prim.GetPath().GetString());
        }
    }

    // Print available cameras
    if (available_cameras.empty()) {
        spdlog::warn("No cameras found in the scene");
    }
    else {
        spdlog::info("Available cameras in scene:");
        for (const auto& cam_path : available_cameras) {
            spdlog::info("  - {}", cam_path);
        }
    }

    // If camera_path is specified, try to use it
    if (!camera_path.empty()) {
        SdfPath path(camera_path);
        UsdPrim prim = stage->GetPrimAtPath(path);
        if (prim && prim.IsA<UsdGeomCamera>()) {
            spdlog::info("Using specified camera: {}", camera_path);
            return UsdGeomCamera(prim);
        }
        else {
            spdlog::warn(
                "Specified camera path '{}' not found or not a camera, falling "
                "back to first camera",
                camera_path);
        }
    }

    // Fall back to first camera
    if (!available_cameras.empty()) {
        SdfPath path(available_cameras[0]);
        UsdPrim prim = stage->GetPrimAtPath(path);
        spdlog::info("Using camera: {}", available_cameras[0]);
        return UsdGeomCamera(prim);
    }

    return UsdGeomCamera();
}

// Graphics context initialization
void CreateGLContext()
{
#ifdef _WIN32
    HDC hdc = GetDC(GetConsoleWindow());
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 24;

    int pixelFormat = ChoosePixelFormat(hdc, &pfd);
    SetPixelFormat(hdc, pixelFormat, &pfd);

    HGLRC hglrc = wglCreateContext(hdc);
    wglMakeCurrent(hdc, hglrc);
#endif
}

// Image utilities
std::string GetFileExtension(const std::string& filename)
{
    size_t pos = filename.find_last_of('.');
    if (pos == std::string::npos) {
        return "";
    }
    std::string ext = filename.substr(pos + 1);
    // Convert to lowercase
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext;
}

std::string GenerateSequenceFilename(
    const std::string& base_filename,
    int frame_number,
    int total_frames)
{
    if (total_frames <= 1) {
        return base_filename;
    }

    // Determine number of digits needed
    int num_digits = std::to_string(total_frames - 1).length();
    if (num_digits < 4)
        num_digits = 4;  // Minimum 4 digits

    // Extract extension
    size_t dot_pos = base_filename.find_last_of('.');
    std::string name_part = (dot_pos != std::string::npos)
                                ? base_filename.substr(0, dot_pos)
                                : base_filename;
    std::string ext_part =
        (dot_pos != std::string::npos) ? base_filename.substr(dot_pos) : ".png";

    // Format frame number with leading zeros
    std::ostringstream oss;
    oss << name_part << "_" << std::setfill('0') << std::setw(num_digits)
        << frame_number << ext_part;
    return oss.str();
}

bool SaveImageToFile(
    const std::string& filename,
    int width,
    int height,
    const std::vector<uint8_t>& data,
    bool skip_save = false)
{
    if (skip_save) {
        spdlog::info("Skipping image save (profiling mode): {}", filename);
        return true;
    }

    // Create directory if it doesn't exist
    std::filesystem::path filepath(filename);
    if (filepath.has_parent_path()) {
        std::filesystem::path parent_dir = filepath.parent_path();
        if (!std::filesystem::exists(parent_dir)) {
            try {
                std::filesystem::create_directories(parent_dir);
                spdlog::info(
                    "Created output directory: {}", parent_dir.string());
            }
            catch (const std::exception& e) {
                spdlog::error(
                    "Failed to create directory {}: {}",
                    parent_dir.string(),
                    e.what());
                return false;
            }
        }
    }

    const float* float_data = reinterpret_cast<const float*>(data.data());
    std::string ext = GetFileExtension(filename);

    // Check if it's a HDR/EXR format
    if (ext == "exr" || ext == "hdr") {
        spdlog::info("Saving as HDR format: {}", ext);

        // Create flipped float data
        std::vector<float> flipped_data(width * height * 4);
        for (int y = 0; y < height; ++y) {
            int flipped_y = height - 1 - y;
            for (int x = 0; x < width; ++x) {
                int src_idx = (y * width + x) * 4;
                int dst_idx = (flipped_y * width + x) * 4;
                flipped_data[dst_idx + 0] = float_data[src_idx + 0];
                flipped_data[dst_idx + 1] = float_data[src_idx + 1];
                flipped_data[dst_idx + 2] = float_data[src_idx + 2];
                flipped_data[dst_idx + 3] = float_data[src_idx + 3];
            }
        }

        // Use USD Hio to save HDR/EXR
        HioImage::StorageSpec storage;
        storage.width = width;
        storage.height = height;
        storage.format = HioFormatFloat32Vec4;
        storage.flipped = false;  // Already flipped above
        storage.data = flipped_data.data();

        auto image = HioImage::OpenForWriting(filename);
        if (!image) {
            spdlog::error("Could not create image output for {}", filename);
            return false;
        }

        if (!image->Write(storage)) {
            spdlog::error("Failed to write HDR image to {}", filename);
            return false;
        }

        spdlog::info("Successfully saved HDR image to {}", filename);
        return true;
    }
    else {
        // PNG and other LDR formats - use STB
        spdlog::info("Saving as LDR format (PNG)");

        std::vector<uint8_t> rgba_data(width * height * 4);

        // Flip the image vertically while converting from float to uint8
        for (int y = 0; y < height; ++y) {
            int flipped_y = height - 1 - y;
            for (int x = 0; x < width; ++x) {
                int src_idx = (y * width + x) * 4;
                int dst_idx = (flipped_y * width + x) * 4;
                rgba_data[dst_idx + 0] = static_cast<uint8_t>(
                    std::clamp(float_data[src_idx + 0], 0.0f, 1.0f) * 255.0f);
                rgba_data[dst_idx + 1] = static_cast<uint8_t>(
                    std::clamp(float_data[src_idx + 1], 0.0f, 1.0f) * 255.0f);
                rgba_data[dst_idx + 2] = static_cast<uint8_t>(
                    std::clamp(float_data[src_idx + 2], 0.0f, 1.0f) * 255.0f);
                rgba_data[dst_idx + 3] = static_cast<uint8_t>(
                    std::clamp(float_data[src_idx + 3], 0.0f, 1.0f) * 255.0f);
            }
        }

        return stbi_write_png(
                   filename.c_str(),
                   width,
                   height,
                   4,
                   rgba_data.data(),
                   width * 4) != 0;
    }
}

// Texture reading methods
bool ReadTextureDirectly(
    UsdImagingGLEngine* renderer,
    int width,
    int height,
    std::vector<uint8_t>& texture_data)
{
    auto hacked_handle =
        renderer->GetRendererSetting(pxr::TfToken("VulkanColorAov"));
    if (!hacked_handle.IsHolding<const void*>()) {
        return false;
    }

    spdlog::info("Using direct texture copy method...");

    auto bare_pointer = hacked_handle.Get<const void*>();
    auto texture =
        *static_cast<nvrhi::ITexture**>(const_cast<void*>(bare_pointer));

    texture_data.resize(width * height * 4 * sizeof(float));

    // Create staging texture once and reuse command list
    static nvrhi::StagingTextureHandle staging_texture;
    static nvrhi::CommandListHandle command_list;

    if (!command_list) {
        command_list = RHI::get_device()->createCommandList();
    }

    if (!staging_texture || staging_texture->getDesc().width != width ||
        staging_texture->getDesc().height != height) {
        nvrhi::TextureDesc staging_desc;
        staging_desc.debugName = "headless_staging";
        staging_desc.width = width;
        staging_desc.height = height;
        staging_desc.format = texture->getDesc().format;
        staging_desc.initialState = nvrhi::ResourceStates::CopyDest;

        staging_texture = RHI::get_device()->createStagingTexture(
            staging_desc, nvrhi::CpuAccessMode::Read);
    }

    // Single command list operation
    command_list->open();
    command_list->copyTexture(staging_texture, {}, texture, {});
    command_list->close();
    RHI::get_device()->executeCommandList(command_list.Get());
    RHI::get_device()->waitForIdle();

    // Direct memory copy without row-by-row iteration
    size_t pitch;
    auto mapped = RHI::get_device()->mapStagingTexture(
        staging_texture, {}, nvrhi::CpuAccessMode::Read, &pitch);

    size_t row_size = width * 4 * sizeof(float);
    if (pitch == row_size) {
        // Contiguous memory - single memcpy
        memcpy(texture_data.data(), mapped, height * row_size);
    }
    else {
        // Non-contiguous - batch copy rows
        auto src_ptr = static_cast<uint8_t*>(mapped);
        auto dst_ptr = texture_data.data();
        for (int i = 0; i < height; ++i) {
            memcpy(dst_ptr, src_ptr, row_size);
            src_ptr += pitch;
            dst_ptr += row_size;
        }
    }

    RHI::get_device()->unmapStagingTexture(staging_texture);
    spdlog::info("Direct texture copy completed successfully");
    return true;
}

bool ReadTextureCPU(
    UsdImagingGLEngine* renderer,
    HgiUniquePtr& hgi,
    int width,
    int height,
    std::vector<uint8_t>& texture_data)
{
    spdlog::info("Using CPU readback method...");

    auto hgi_texture = renderer->GetAovTexture(HdAovTokens->color);
    if (!hgi_texture) {
        spdlog::error("Failed to get rendered texture from renderer AOV");
        return false;
    }

    size_t buffer_size = width * height * 4 * sizeof(float);
    texture_data.resize(buffer_size);

    auto blit_cmds = hgi->CreateBlitCmds();
    HgiTextureGpuToCpuOp copy_op;
    copy_op.gpuSourceTexture = hgi_texture;
    copy_op.cpuDestinationBuffer = texture_data.data();
    copy_op.destinationBufferByteSize = texture_data.size();
    blit_cmds->CopyTextureGpuToCpu(copy_op);

    hgi->SubmitCmds(blit_cmds.get(), HgiSubmitWaitTypeWaitUntilCompleted);
    return true;
}

std::string LoadJSONScript(const std::string& filename)
{
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error(
            "Could not open JSON script file: " + filename);
    }

    std::string content;
    std::string line;
    while (std::getline(file, line)) {
        content += line + "\n";
    }
    return content;
}

int main(int argc, char* argv[])
{
    // 禁止 abort 弹窗，改为直接退出
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // 或者设置错误模式，避免 Windows 弹窗
    _set_error_mode(_OUT_TO_STDERR);

    // 解除 C++ 流与 C 流的同步以加速输出
    std::ios_base::sync_with_stdio(false);

    // Parse command line using cmdparser
    cmdline::parser parser;
    parser.add<std::string>("usd", 'u', "USD file to render", true);
    parser.add<std::string>("json", 'j', "JSON rendering script", true);
    parser.add<std::string>(
        "output", 'o', "Output image filename (PNG/HDR/EXR)", true);
    parser.add<int>("width", 'w', "Image width", false, 1920);
    parser.add<int>("height", 'h', "Image height", false, 1080);
    parser.add<int>("spp", 's', "Samples per pixel", false, 16);
    parser.add<std::string>(
        "camera", 'c', "Camera prim path (e.g., /Camera)", false, "");
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

    parser.parse_check(argc, argv);
    const bool verbose = parser.exist("verbose");

    initialize_framework_logging(
        "rz_render", verbose ? spdlog::level::info : spdlog::level::warn);

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
    bool skip_save = parser.exist("no-save");
    int num_frames = parser.get<int>("frames");
    float fps = parser.get<float>("fps");
    float delta_time = 1.0f / fps;

    // Validate input files
    if (!std::filesystem::exists(usd_file)) {
        spdlog::error("USD file not found: {}", usd_file);
        return 1;
    }
    if (!std::filesystem::exists(json_script)) {
        spdlog::error("JSON script not found: {}", json_script);
        return 1;
    }

    spdlog::info("Starting render (pre-simulated scene)...");
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
        renderer->SetRendererPlugin(TfToken("Hd_RUZINO_RendererPlugin"));
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
        render_params.drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
        render_params.colorCorrectionMode = HdxColorCorrectionTokens->disabled;
        render_params.clearColor = GfVec4f(0.2f, 0.2f, 0.2f, 1.0f);
        renderer->SetRendererAov(HdAovTokens->color);

        // Load and apply JSON script
        auto node_system = static_cast<const std::shared_ptr<NodeSystem>*>(
            renderer->GetRendererSetting(pxr::TfToken("RenderNodeSystem"))
                .Get<const void*>());

        std::string nodes_json = LoadJSONScript(json_script);
        (*node_system)->get_node_tree()->deserialize(nodes_json);

        // Determine if we're rendering a sequence or single frame
        bool is_sequence = (num_frames > 0);
        int frames_to_render = is_sequence ? num_frames : 1;

        printf(
            "Starting %s render (pre-simulated scene)...\n",
            is_sequence ? "sequence" : "single frame");
        if (is_sequence) {
            printf(
                "Total frames: %d, Time step: %.4fs (%.0f fps)\n",
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
            // Set time code for pre-simulated scene
            // No tick() call - assumes animation data is already baked into USD
            pxr::UsdTimeCode time_code(frame * delta_time);
            stage->set_render_time(time_code);
            render_params.frame = time_code;

            // Render the scene with multiple samples
            UsdPrim root = stage->get_usd_stage()->GetPseudoRoot();

            // Start timing (will be set after first sample)
            auto render_start = std::chrono::high_resolution_clock::now();
            long long total_sample_time = 0;
            int timed_samples = 0;

            for (int sample = 0; sample < spp; ++sample) {
                auto sample_start = std::chrono::high_resolution_clock::now();

                renderer->Render(root, render_params);
                RHI::get_device()->waitForIdle();
                RHI::get_device()->runGarbageCollection();
                renderer->StopRenderer();

                auto sample_end = std::chrono::high_resolution_clock::now();
                auto sample_duration =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        sample_end - sample_start)
                        .count();

                // Skip first sample for timing (shader compilation, etc.)
                if (sample == 0 && frame == 0) {
                    render_start = std::chrono::high_resolution_clock::now();
                    printf(
                        "Sample 1/%d completed in %.2fs (warmup)\n",
                        spp,
                        sample_duration / 1000.0);
                    fflush(stdout);
                    continue;
                }

                total_sample_time += sample_duration;
                timed_samples++;

                // Calculate progress and ETA (based on samples after warmup)
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
                if (is_sequence) {
                    printf(
                        "\r[Frame %d/%d] [%s] %3d%% (%d/%d) Sample: %.4fs Avg: "
                        "%.4fs ETA: %dm%02ds",
                        frame + 1,
                        frames_to_render,
                        bar,
                        progress_percent,
                        sample + 1,
                        spp,
                        sample_duration / 1000.0,
                        avg_time_per_sample / 1000.0,
                        eta_minutes,
                        eta_secs);
                }
                else {
                    printf(
                        "\r[%s] %3d%% (%d/%d) Sample: %.4fs Avg: %.4fs ETA: "
                        "%dm%02ds",
                        bar,
                        progress_percent,
                        sample + 1,
                        spp,
                        sample_duration / 1000.0,
                        avg_time_per_sample / 1000.0,
                        eta_minutes,
                        eta_secs);
                }
                fflush(stdout);
            }
            printf("\n");

            auto render_end = std::chrono::high_resolution_clock::now();
            auto total_duration =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    render_end - render_start)
                    .count();

            if (!is_sequence) {
                // For single frame, print summary
                printf("Rendering completed in %.2fs", total_duration / 1000.0);
                if (timed_samples > 0) {
                    printf(
                        ", Avg per sample: %.2fs",
                        total_sample_time / (double)timed_samples / 1000.0);
                }
                printf("\n");
                fflush(stdout);
            }

            // Read back texture data
            std::vector<uint8_t> texture_data;
            bool success = ReadTextureDirectly(
                renderer.get(), width, height, texture_data);

            if (!success) {
                success = ReadTextureCPU(
                    renderer.get(), hgi, width, height, texture_data);
            }

            if (!success) {
                throw std::runtime_error("Failed to read rendered texture");
            }

            // Generate output filename for this frame
            std::string frame_output =
                GenerateSequenceFilename(output_image, frame, frames_to_render);

            // Launch async save task (capture by value to avoid data races)
            previous_save_task = std::async(
                std::launch::async,
                [frame_output, width, height, texture_data, skip_save]() {
                    auto save_start = std::chrono::high_resolution_clock::now();

                    bool save_success = SaveImageToFile(
                        frame_output, width, height, texture_data, skip_save);

                    auto save_end = std::chrono::high_resolution_clock::now();
                    auto save_duration =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            save_end - save_start)
                            .count();

                    if (!skip_save) {
                        if (save_success) {
                            spdlog::info(
                                "Saved image to {} ({:.2f}s)",
                                frame_output,
                                save_duration / 1000.0);
                        }
                        else {
                            spdlog::error(
                                "Failed to save image to {}", frame_output);
                        }
                    }
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
            printf("Render completed successfully!\n");
        }
        fflush(stdout);

        // Cleanup
        renderer.reset();
        hgi.reset();
        stage.reset();
        unregister_cpp_type();

        // Shutdown RHI at the end
        printf("Successfully finished all operations.\n");
        return 0;
    }
    catch (const std::exception& e) {
        log_exception_with_context("rz_render failed", e);
        return 1;
    }
}
