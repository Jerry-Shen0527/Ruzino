#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <entt/meta/meta.hpp>

#include "RHI/rhi.hpp"
#include "hd_RUZINO/render_global_payload.hpp"
#include "nodes/core/api.hpp"
#include "nodes/core/node_exec_eager.hpp"
#include "nodes/system/node_system.hpp"
#include "nodes/system/node_system_dl.hpp"
#include "pxr/base/vt/array.h"
#include "renderTLAS.h"

// USD Imaging includes for Hydra rendering
#include "pxr/base/gf/camera.h"
#include "pxr/base/gf/frustum.h"
#include "pxr/base/tf/token.h"
#include "pxr/imaging/garch/glApi.h"
#include "pxr/imaging/hd/driver.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdx/tokens.h"
#include "pxr/imaging/hgi/hgi.h"
#include "pxr/imaging/hgi/tokens.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/camera.h"
#include "pxr/usdImaging/usdImagingGL/engine.h"
#include "spdlog/spdlog.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <EGL/egl.h>
#include <GL/gl.h>
#endif

#if RUZINO_WITH_CUDA
#include <cuda_runtime.h>

#include "RHI/internal/cuda_extension.hpp"

#endif

namespace nb = nanobind;
using namespace Ruzino;

// OpenGL 4.5+ context creation
// Windows: Two-step WGL process (legacy context -> wglCreateContextAttribsARB)
// Linux: EGL headless context (no X server required)

#ifdef _WIN32
// WGL_ARB_create_context constants (not in standard Windows headers)
#ifndef WGL_CONTEXT_MAJOR_VERSION_ARB
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#endif
#ifndef WGL_CONTEXT_MINOR_VERSION_ARB
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#endif
#ifndef WGL_CONTEXT_PROFILE_MASK_ARB
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#endif
#ifndef WGL_CONTEXT_CORE_PROFILE_BIT_ARB
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#endif

typedef HGLRC(WINAPI* PFNWGLCREATECONTEXTATTRIBSARB)(
    HDC, HGLRC, const int*);
#endif

// Track GL context resources for cleanup
#ifdef _WIN32
static HWND g_glHwnd = nullptr;
static HDC g_glHdc = nullptr;
#endif

static void CreateGLContext()
{
#ifdef _WIN32
    // Create a dedicated window for GL context (console window may not work
    // well with pixel format selection on some GPU drivers)
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "RuzinoGLContext";
    RegisterClassA(&wc);

    g_glHwnd = CreateWindowExA(
        0, "RuzinoGLContext", "", 0, 0, 0, 1, 1,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    g_glHdc = GetDC(g_glHwnd);

    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;

    int pixelFormat = ChoosePixelFormat(g_glHdc, &pfd);
    SetPixelFormat(g_glHdc, pixelFormat, &pfd);

    // Step 1: Create legacy context to bootstrap extensions
    HGLRC legacyCtx = wglCreateContext(g_glHdc);
    wglMakeCurrent(g_glHdc, legacyCtx);

    // Step 2: Get wglCreateContextAttribsARB
    auto wglCreateContextAttribsARB =
        (PFNWGLCREATECONTEXTATTRIBSARB)wglGetProcAddress(
            "wglCreateContextAttribsARB");

    if (wglCreateContextAttribsARB) {
        // Step 3: Create OpenGL 4.5 core profile context
        int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
            WGL_CONTEXT_MINOR_VERSION_ARB, 5,
            WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0};
        HGLRC modernCtx = wglCreateContextAttribsARB(g_glHdc, nullptr, attribs);
        if (modernCtx) {
            wglMakeCurrent(g_glHdc, modernCtx);
            wglDeleteContext(legacyCtx);
            return;
        }
        // Fallback: try 4.4, 4.3, 4.2, 4.1, 4.0
        for (int minor = 4; minor >= 0; minor--) {
            attribs[3] = minor;
            modernCtx = wglCreateContextAttribsARB(g_glHdc, nullptr, attribs);
            if (modernCtx) {
                wglMakeCurrent(g_glHdc, modernCtx);
                wglDeleteContext(legacyCtx);
                return;
            }
        }
    }

    // If we get here, we're stuck with the legacy context (may not work with
    // HgiGL)
#elif defined(__linux__)
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        spdlog::error("Failed to get EGL display");
        return;
    }

    if (!eglInitialize(display, nullptr, nullptr)) {
        spdlog::error("Failed to initialize EGL");
        return;
    }

    EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      24,
        EGL_STENCIL_SIZE,    8,
        EGL_NONE
    };

    EGLConfig config;
    EGLint num_config;
    if (!eglChooseConfig(display, config_attribs, &config, 1, &num_config) ||
        num_config == 0) {
        spdlog::error("Failed to choose EGL config");
        eglTerminate(display);
        return;
    }

    eglBindAPI(EGL_OPENGL_API);

    EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION,       4,
        EGL_CONTEXT_MINOR_VERSION,       5,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };

    EGLContext context =
        eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
    if (!context) {
        for (int minor = 4; minor >= 0; minor--) {
            context_attribs[3] = minor;
            context =
                eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
            if (context) break;
        }
    }

    if (!context) {
        spdlog::error("Failed to create EGL OpenGL context");
        eglTerminate(display);
        return;
    }

    EGLint pbuffer_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface surface =
        eglCreatePbufferSurface(display, config, pbuffer_attribs);
    if (surface == EGL_NO_SURFACE) {
        spdlog::warn("Failed to create EGL pbuffer, trying without surface");
    }

    eglMakeCurrent(display, surface, surface, context);
#endif
}

// Wrapper class for USD Imaging rendering with node system access
class HydraRenderer {
   public:
    HydraRenderer(const std::string& usd_file, int width, int height)
        : width_(width),
          height_(height)
    {
        // Initialize OpenGL context (critical for MaterialX shader generation)
        CreateGLContext();
        GarchGLApiLoad();
        RHI::init(false, true);

        // Load USD stage
        stage_ = pxr::UsdStage::Open(usd_file);
        if (!stage_) {
            throw std::runtime_error("Failed to load USD stage: " + usd_file);
        }

        // Create HGI and driver
        hgi_ = pxr::Hgi::CreatePlatformDefaultHgi();
        pxr::HdDriver driver;
        driver.name = pxr::HgiTokens->renderDriver;
        driver.driver = pxr::VtValue(hgi_.get());

        // Create rendering engine
        pxr::UsdImagingGLEngine::Parameters params;
        params.driver = driver;
        params.allowAsynchronousSceneProcessing = false;

        engine_ = std::make_unique<pxr::UsdImagingGLEngine>(params);
        engine_->SetRendererPlugin(pxr::TfToken("Hd_RUZINO_RendererPlugin"));
        engine_->SetEnablePresentation(false);
        engine_->SetRenderBufferSize(pxr::GfVec2i(width, height));
        engine_->SetRenderViewport(pxr::GfVec4d(0, 0, width, height));

        // Pre-create event query for GPU sync in get_output_texture
        event_query_ = RHI::get_device()->createEventQuery();

        // Find camera
        for (const auto& prim : stage_->Traverse()) {
            if (prim.IsA<pxr::UsdGeomCamera>()) {
                camera_ = pxr::UsdGeomCamera(prim);
                break;
            }
        }

        if (!camera_) {
            throw std::runtime_error("No camera found in USD stage");
        }

        // Setup camera
        auto gf_camera = camera_.GetCamera(pxr::UsdTimeCode::Default());
        auto frustum = gf_camera.GetFrustum();
        engine_->SetCameraState(
            frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix());

        // Set AOV
        engine_->SetRendererAov(pxr::HdAovTokens->color);
    }

    // Get the node system from the render delegate (returns raw pointer for
    // Python binding)
    NodeSystem* get_node_system()
    {
        auto value =
            engine_->GetRendererSetting(pxr::TfToken("RenderNodeSystem"));
        if (!value.IsHolding<const void*>()) {
            throw std::runtime_error("Failed to get node system from renderer");
        }

        auto ptr = value.Get<const void*>();
        auto node_system_ptr =
            *static_cast<const std::shared_ptr<NodeSystem>*>(ptr);
        return node_system_ptr
            .get();  // Return raw pointer for cross-module compatibility
    }

    // Render one frame (does not stop renderer between calls)
    void render()
    {
        pxr::UsdImagingGLRenderParams params;
        params.enableLighting = true;
        params.enableSceneMaterials = true;
        params.showRender = true;
        params.frame = pxr::UsdTimeCode(0);
        params.drawMode = pxr::UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
        params.colorCorrectionMode = pxr::HdxColorCorrectionTokens->disabled;
        params.clearColor = pxr::GfVec4f(0.2f, 0.2f, 0.2f, 1.0f);

        pxr::UsdPrim root = stage_->GetPseudoRoot();
        engine_->Render(root, params);
    }

    // Stop the render thread and wait for completion
    void stop() { engine_->StopRenderer(); }

    // Get output texture data (legacy CPU copy)
    // name: optional texture name (uses default if empty)
    std::vector<float> get_output_texture(const std::string& name = "")
    {
        // Ensure render thread has finished before reading
        engine_->StopRenderer();
        pxr::TfToken token_key;

        if (name.empty()) {
            // Legacy: get default texture
            token_key = pxr::TfToken("VulkanColorAov");
        }
        else {
            // New: get named texture
            token_key = pxr::TfToken("VulkanColorAov:" + name);
        }

        auto hacked_handle = engine_->GetRendererSetting(token_key);
        if (!hacked_handle.IsHolding<const void*>()) {
            if (!name.empty()) {
                spdlog::error(
                    "Failed to get texture '{}' - handle not holding void*",
                    name);
                throw std::runtime_error(
                    "Failed to get output texture '" + name + "'");
            }
            throw std::runtime_error("Failed to get output texture");
        }

        auto bare_pointer = hacked_handle.Get<const void*>();
        auto texture =
            *static_cast<nvrhi::ITexture**>(const_cast<void*>(bare_pointer));

        // Create staging texture and copy
        nvrhi::TextureDesc staging_desc;
        staging_desc.width = width_;
        staging_desc.height = height_;
        staging_desc.format = texture->getDesc().format;
        staging_desc.initialState = nvrhi::ResourceStates::CopyDest;

        auto staging_texture = RHI::get_device()->createStagingTexture(
            staging_desc, nvrhi::CpuAccessMode::Read);

        // Flush all pending GPU work before copying (ensures render commands complete)
        RHI::get_device()->waitForIdle();

        auto cmd_list = RHI::get_device()->createCommandList();
        cmd_list->open();
        cmd_list->copyTexture(staging_texture, {}, texture, {});
        cmd_list->close();
        RHI::get_device()->executeCommandList(cmd_list.Get());

        RHI::get_device()->setEventQuery(event_query_.Get(), nvrhi::CommandQueue::Graphics);
        RHI::get_device()->waitEventQuery(event_query_.Get());

        // Read data
        size_t pitch;
        auto mapped = RHI::get_device()->mapStagingTexture(
            staging_texture, {}, nvrhi::CpuAccessMode::Read, &pitch);

        std::vector<float> result(width_ * height_ * 4);
        size_t row_size = width_ * 4 * sizeof(float);

        if (pitch == row_size) {
            memcpy(result.data(), mapped, height_ * row_size);
        }
        else {
            auto src_ptr = static_cast<uint8_t*>(mapped);
            auto dst_ptr = reinterpret_cast<uint8_t*>(result.data());
            for (int i = 0; i < height_; ++i) {
                memcpy(dst_ptr, src_ptr, row_size);
                src_ptr += pitch;
                dst_ptr += row_size;
            }
        }

        RHI::get_device()->unmapStagingTexture(staging_texture);
        return result;
    }

#if RUZINO_WITH_CUDA
    // Get output texture as CUDA buffer (zero-copy to GPU)
    // name: optional texture name (uses default if empty)
    Ruzino::cuda::CUDALinearBufferHandle get_output_cuda_buffer(
        const std::string& name = "")
    {
        // Ensure render thread has finished before reading
        engine_->StopRenderer();

        pxr::TfToken token_key;

        if (name.empty()) {
            // Legacy: get default texture
            token_key = pxr::TfToken("VulkanColorAov");
        }
        else {
            // New: get named texture
            token_key = pxr::TfToken("VulkanColorAov:" + name);
        }

        auto hacked_handle = engine_->GetRendererSetting(token_key);
        if (!hacked_handle.IsHolding<const void*>()) {
            if (!name.empty()) {
                throw std::runtime_error(
                    "Failed to get output texture '" + name + "'");
            }
            throw std::runtime_error("Failed to get output texture");
        }

        auto bare_pointer = hacked_handle.Get<const void*>();
        auto texture =
            *static_cast<nvrhi::ITexture**>(const_cast<void*>(bare_pointer));

        // Determine element size based on format
        uint32_t element_size = 0;
        const auto& desc = texture->getDesc();
        switch (desc.format) {
            case nvrhi::Format::RGBA32_FLOAT: element_size = 16; break;
            case nvrhi::Format::RGB32_FLOAT: element_size = 12; break;
            case nvrhi::Format::RG32_FLOAT: element_size = 8; break;
            case nvrhi::Format::R32_FLOAT: element_size = 4; break;
            default:
                element_size = 16;  // Default to RGBA32
                break;
        }

        // Use existing CUDA texture conversion
        return Ruzino::cuda::copy_texture_to_linear_buffer_with_cleanup(
            RHI::get_device(), texture, element_size);
    }
#endif

    int width() const
    {
        return width_;
    }
    int height() const
    {
        return height_;
    }

    ~HydraRenderer()
    {
        engine_.reset();
        hgi_.reset();
#ifdef _WIN32
        if (g_glHwnd) {
            if (g_glHdc) ReleaseDC(g_glHwnd, g_glHdc);
            DestroyWindow(g_glHwnd);
            g_glHwnd = nullptr;
            g_glHdc = nullptr;
        }
#endif
    }

   private:
    pxr::UsdStageRefPtr stage_;
    pxr::UsdGeomCamera camera_;
    std::unique_ptr<pxr::Hgi> hgi_;
    std::unique_ptr<pxr::UsdImagingGLEngine> engine_;
    nvrhi::EventQueryHandle event_query_;
    int width_;
    int height_;
};

NB_MODULE(hd_RUZINO_py, m)
{
    m.doc() =
        "Ruzino Renderer Python bindings - Render graph support with USD/Hydra";

    // Import NodeSystem type from nodes_system_py module for cross-module
    // compatibility
    nb::module_::import_("nodes_system_py");

    // HydraRenderer class - provides USD + Hydra + Node System integration
    nb::class_<HydraRenderer>(m, "HydraRenderer")
        .def(nb::init<const char*, int, int>(),
             nb::arg("usd_file"),
             nb::arg("width") = 1920,
             nb::arg("height") = 1080,
             "Create a Hydra renderer for a USD stage")
        .def(
            "get_node_system",
            &HydraRenderer::get_node_system,
            nb::rv_policy::reference,
            "Get the NodeSystem from the Hydra render delegate")
        .def("render", &HydraRenderer::render, "Render one frame")
        .def("stop", &HydraRenderer::stop,
             "Stop the render thread and wait for completion")
        .def(
            "get_output_texture",
            [](HydraRenderer& self) -> std::vector<float> {
                return self.get_output_texture();
            },
            "Get the rendered texture as a float array (RGBA, row-major).")
        .def(
            "get_output_texture",
            [](HydraRenderer& self, const std::string& name) -> std::vector<float> {
                return self.get_output_texture(name);
            },
            nb::arg("name"),
            "Get a named texture from present nodes as a float array.")
#if RUZINO_WITH_CUDA
        .def(
            "get_output_cuda_buffer",
            [](HydraRenderer& self) -> Ruzino::cuda::CUDALinearBufferHandle {
                return self.get_output_cuda_buffer();
            },
            "Get the rendered texture as CUDA buffer (GPU memory, zero-copy).")
        .def(
            "get_output_cuda_buffer",
            [](HydraRenderer& self, const std::string& name) -> Ruzino::cuda::CUDALinearBufferHandle {
                return self.get_output_cuda_buffer(name);
            },
            nb::arg("name"),
            "Get a named texture from present nodes as CUDA buffer.")
#endif
        .def_prop_ro("width", &HydraRenderer::width)
        .def_prop_ro("height", &HydraRenderer::height);

    // Helper function to create render system (just returns NodeSystem)
    m.def(
        "create_render_system",
        []() { return create_dynamic_loading_system(); },
        "Create a new render node system");

    // Helper to create render executor (just returns standard
    // EagerNodeTreeExecutor) The render-specific logic is in the C++ nodes
    // themselves
    m.def(
        "create_render_executor",
        []() { return std::make_shared<EagerNodeTreeExecutor>(); },
        "Create a new node tree executor");

    // Create a basic RenderGlobalPayload wrapped in meta_any for standalone
    // Python rendering This is a minimal version without USD integration,
    // suitable for testing
    m.def(
        "create_render_global_payload",
        []() -> entt::meta_any {
            // Create empty arrays for cameras, lights, and materials (use
            // static to keep them alive)
            nvrhi::IDevice* device = RHI::get_device();

            static VtArray<Hd_RUZINO_Camera*> cameras;
            static VtArray<Hd_RUZINO_Light*> lights;
            static TfHashMap<SdfPath, Hd_RUZINO_Material*, TfHash> materials;
            static std::unique_ptr<Hd_RUZINO_RenderInstanceCollection>
                instance_collection;

            //// Initialize on first call
            // if (!instance_collection) {
            //     instance_collection =
            //         std::make_unique<Hd_RUZINO_RenderInstanceCollection>();
            // }

            //// Create the payload
            // RenderGlobalPayload payload(&cameras, &lights, &materials,
            // device); payload.InstanceCollection = instance_collection.get();
            // payload.InstanceCollection = nullptr;
            // payload.lens_system = nullptr;  // Not needed for basic rendering

            //// Register type and wrap in meta_any
            // Ruzino::register_cpp_type<RenderGlobalPayload>();
            auto& ctx = Ruzino::get_entt_ctx();
            return entt::meta_any{ ctx, 1 };
        },
        "Create a basic RenderGlobalPayload wrapped in meta_any for standalone "
        "rendering");
    //
    //    // Helper to wrap RenderGlobalPayload in meta_any for node system
    //    (kept for
    //    // compatibility)
    //    m.def(
    //        "create_meta_any_from_render_payload",
    //        [](const RenderGlobalPayload& payload) -> entt::meta_any {
    //            // Ensure type is registered with the correct context
    //            Ruzino::register_cpp_type<RenderGlobalPayload>();
    //
    //            // Create meta_any with the RenderGlobalPayload using the same
    //            // context CRITICAL: Must use the same entt context as the
    //            node
    //            // system!
    //            auto& ctx = Ruzino::get_entt_ctx();
    //            auto type = entt::resolve<RenderGlobalPayload>(ctx);
    //
    //            if (!type) {
    //                throw std::runtime_error(
    //                    "RenderGlobalPayload type not registered in entt meta
    //                    " "system");
    //            }
    //
    //            // Create meta_any with explicit context
    //            return entt::meta_any{ ctx, payload };
    //        },
    //        nb::arg("payload"),
    //        "Wrap RenderGlobalPayload in meta_any for node system use");
}
