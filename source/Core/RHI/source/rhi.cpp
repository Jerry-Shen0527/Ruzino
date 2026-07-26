
#include <nvrhi/nvrhi.h>
#include <spdlog/spdlog.h>

#include <RHI/internal/nvrhi_equality.hpp>
#include <RHI/rhi.hpp>
#include <algorithm>
#include <cstring>
#include <memory>

#include "RHI/DeviceManager/DeviceManager.h"
#include "nvrhi/utils.h"

#if RUZINO_WITH_OPENUSD
#include "pxr/imaging/garch/glApi.h"
#endif

#ifdef _WIN32
#include <gl/GL.h>
#include <windows.h>
// WGL_ARB_create_context entry point + attribute tokens (defined locally so we
// don't depend on GL/wglext.h, which isn't in every Windows SDK).
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
#endif
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <iostream>

#if RUZINO_WITH_VULKAN
#include "vulkan/vulkan.hpp"
#endif

RUZINO_NAMESPACE_OPEN_SCOPE
namespace RHI {

std::unique_ptr<DeviceManager> device_manager = nullptr;
std::map<std::string, nvrhi_image> rhi_images{};
static int reference_count = 0;
static std::weak_ptr<spdlog::logger> cached_logger;

int init(bool with_window, bool use_dx12)
{
    // Cache the logger on first access
    if (!cached_logger.lock()) {
        cached_logger = spdlog::default_logger();
    }
    if (device_manager) {
        reference_count++;
        if (auto logger = cached_logger.lock()) {
            logger->info(
                "RHI already initialized, reference count: {}",
                reference_count);
        }
        return 0;
    }

    auto api =
        use_dx12 ? nvrhi::GraphicsAPI::D3D12 : nvrhi::GraphicsAPI::VULKAN;
    device_manager = std::unique_ptr<DeviceManager>(DeviceManager::Create(api));

    DeviceCreationParameters params;

    params.enableRayTracingExtensions = true;
    params.enableComputeQueue = true;
    params.enableCopyQueue = true;
// params.adapterIndex = 0;
#if RUZINO_WITH_VULKAN
    params.optionalVulkanInstanceExtensions = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME
    };
    params.optionalVulkanDeviceExtensions = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,
        VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_NV_RAY_TRACING_INVOCATION_REORDER_EXTENSION_NAME,
        VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME,
        VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME,
#ifdef _WIN32
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME
#else
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME
#endif

    };
#endif

    params.swapChainFormat = nvrhi::Format::RGBA8_UNORM;
#if RUZINO_WITH_DX11 || RUZINO_WITH_DX12
    params.featureLevel = D3D_FEATURE_LEVEL_12_2;
#endif
#ifdef _DEBUG
    // params.enableNvrhiValidationLayer = true;
    params.enableDebugRuntime = true;
#endif
    //    params.enableDebugRuntime = true;

    if (with_window) {
        auto ret =
            !device_manager->CreateWindowDeviceAndSwapChain(params, "Ruzino");

        device_manager->m_callbacks.afterPresent = [](DeviceManager& manager) {
            manager.SetInformativeWindowTitle("Ruzino");
        };

        if (ret == 0) {
            reference_count = 1;
            cached_logger = spdlog::default_logger();
        }
        return ret;
    }
    else {
        if (device_manager->CreateHeadlessDevice(params)) {
            reference_count = 1;
            cached_logger = spdlog::default_logger();
            return 0;
        }
    }
    return 1;
}

nvrhi::IDevice* get_device()
{
    if (!device_manager) {
        init();
        // Compensate for the init()'s reference_count++ so that
        // the auto-init doesn't leak a reference that shutdown() won't clean
        // up.
        reference_count--;
    }
    return device_manager->GetDevice();
}

nvrhi::GraphicsAPI get_backend()
{
    return get_device()->getGraphicsAPI();
}
size_t calculate_bytes_per_pixel(nvrhi::Format format)
{
    nvrhi::FormatInfo formatInfo = getFormatInfo(format);
    return formatInfo.bytesPerBlock * formatInfo.blockSize;
}

void write_texture(
    nvrhi::ITexture* texture,
    nvrhi::IStagingTexture* staging,
    const void* data,
    nvrhi::ICommandList* command_list)
{
    nvrhi::IDevice* device = get_device();
    size_t rowPitch;
    void* mappedData = device->mapStagingTexture(
        staging, {}, nvrhi::CpuAccessMode::Write, &rowPitch);
    if (mappedData) {
        const uint8_t* srcData = static_cast<const uint8_t*>(data);
        uint8_t* dstData = static_cast<uint8_t*>(mappedData);

        for (uint32_t y = 0; y < texture->getDesc().height; ++y) {
            auto bytesPerPixel =
                calculate_bytes_per_pixel(texture->getDesc().format);
            memcpy(dstData, srcData, texture->getDesc().width * bytesPerPixel);
            srcData += texture->getDesc().width * bytesPerPixel;
            dstData += rowPitch;
        }

        device->unmapStagingTexture(staging);
    }

    nvrhi::CommandListHandle command_list_handle = nullptr;
    if (!command_list) {
        command_list_handle = device->createCommandList();
        command_list = command_list_handle.Get();
    }
    command_list->open();
    command_list->copyTexture(texture, {}, staging, {});
    command_list->close();
    device->executeCommandList(command_list);
}

std::tuple<nvrhi::TextureHandle, nvrhi::StagingTextureHandle> load_texture(
    const nvrhi::TextureDesc& desc,
    const void* data,
    nvrhi::ICommandList* command_list)
{
    nvrhi::IDevice* device = get_device();
    auto texture = device->createTexture(desc);
    // Create a staging texture for uploading data
    nvrhi::TextureDesc stagingDesc = desc;
    stagingDesc.isRenderTarget = false;
    stagingDesc.isUAV = false;
    stagingDesc.initialState = nvrhi::ResourceStates::CopyDest;
    stagingDesc.keepInitialState = true;
    stagingDesc.debugName = "StagingTexture";

    auto stagingTexture =
        device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Write);

    write_texture(texture, stagingTexture, data, command_list);
    assert(texture);
    return std::make_tuple(texture, stagingTexture);
}

void copy_from_texture(
    nvrhi::TextureHandle& texture,
    nvrhi::ITexture* source,
    nvrhi::ICommandList* command_list)
{
    nvrhi::IDevice* device = get_device();
    nvrhi::TextureDesc desc = source->getDesc();
    if (!texture || texture->getDesc() != source->getDesc()) {
        texture = device->createTexture(desc);
    }

    command_list->open();
    command_list->copyTexture(texture, {}, source, {});
    command_list->close();
    device->executeCommandList(command_list);
}
#if RUZINO_WITH_OPENUSD && RUZINO_WITH_VULKAN

nvrhi::TextureHandle load_ogl_texture(
    const nvrhi::TextureDesc& desc,
    unsigned gl_texture)
{
    auto device = RHI::get_device();
    vk::Device vk_device =
        VkDevice(device->getNativeObject(nvrhi::ObjectTypes::VK_Device));
    vk::PhysicalDevice vk_physical_device = VkPhysicalDevice(
        device->getNativeObject(nvrhi::ObjectTypes::VK_PhysicalDevice));

    // Get the OpenGL texture handle
    GLuint64 glHandle = glGetTextureHandleARB(gl_texture);
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        std::cerr << "OpenGL error: " << gluErrorString(error) << std::endl;
        return nullptr;
    }

    // Create Vulkan image with external memory
    vk::ImageCreateInfo imageCreateInfo = {};
    imageCreateInfo.imageType = vk::ImageType::e2D;
    imageCreateInfo.format = vk::Format::eR8G8B8A8Unorm;
    imageCreateInfo.extent.width = desc.width;
    imageCreateInfo.extent.height = desc.height;
    imageCreateInfo.extent.depth = 1;
    imageCreateInfo.mipLevels = desc.mipLevels;
    imageCreateInfo.arrayLayers = desc.arraySize;
    imageCreateInfo.samples = vk::SampleCountFlagBits::e1;
    imageCreateInfo.tiling = vk::ImageTiling::eOptimal;
    imageCreateInfo.usage = vk::ImageUsageFlagBits::eSampled;
    imageCreateInfo.sharingMode = vk::SharingMode::eExclusive;
    imageCreateInfo.initialLayout = vk::ImageLayout::eUndefined;

    // Specify external memory handle types
    vk::ExternalMemoryImageCreateInfo externalMemoryInfo = {};
    externalMemoryInfo.handleTypes =
        vk::ExternalMemoryHandleTypeFlagBits::eOpaqueWin32;

    imageCreateInfo.pNext = &externalMemoryInfo;

    // Create the Vulkan image
    vk::Image vkImage = vk_device.createImage(imageCreateInfo);

    // Get memory requirements
    vk::MemoryRequirements memRequirements =
        vk_device.getImageMemoryRequirements(vkImage);

    // Set up memory allocation info with imported handle
    vk::MemoryAllocateInfo memoryAllocateInfo = {};
    memoryAllocateInfo.allocationSize = memRequirements.size;

    uint32_t memoryTypeIndex = 0;
    vk::PhysicalDeviceMemoryProperties memoryProperties =
        vk_physical_device.getMemoryProperties();
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((memRequirements.memoryTypeBits & (1 << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags &
             vk::MemoryPropertyFlagBits::eDeviceLocal)) {
            memoryTypeIndex = i;
            break;
        }
    }
    memoryAllocateInfo.memoryTypeIndex = memoryTypeIndex;

#if defined(_WIN32)
    vk::ImportMemoryWin32HandleInfoKHR importMemoryInfo = {};
    importMemoryInfo.handleType =
        vk::ExternalMemoryHandleTypeFlagBits::eOpaqueWin32;
    importMemoryInfo.handle = reinterpret_cast<HANDLE>(glHandle);

    memoryAllocateInfo.pNext = &importMemoryInfo;
#else
    vk::ImportMemoryFdInfoKHR importMemoryInfo = {};
    importMemoryInfo.handleType =
        vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;
    importMemoryInfo.fd = static_cast<int>(glHandle);

    memoryAllocateInfo.pNext = &importMemoryInfo;
#endif

    // Allocate memory
    vk::DeviceMemory vkMemory = vk_device.allocateMemory(memoryAllocateInfo);

    // Bind memory to the image
    vk_device.bindImageMemory(vkImage, vkMemory, 0);

    // Create NVRHI texture handle
    nvrhi::TextureHandle texture = device->createHandleForNativeTexture(
        nvrhi::ObjectTypes::VK_Image, static_cast<VkImage>(vkImage), desc);

    return texture;
}
#endif
DeviceManager* internal::get_device_manager()
{
    return device_manager.get();
}

bool ensure_gl_driver_loaded()
{
#if RUZINO_WITH_OPENUSD
    // HgiGL requires a current, hardware-accelerated OpenGL >=4.5 context for
    // its whole lifetime (it queries glGetString(GL_VERSION) + many
    // glGetIntegerv in HgiGLCapabilities at construction, and HgiGL_ScopedState
    // Holder captures/restores a broad set of GL state each blit submit).
    //
    // Strategy: mirror USD's own reference context creation
    // (pxr/imaging/garch/glPlatformDebugWindowWindows.cpp: GarchGLDebugWindow):
    //   - glfwInit() so the GPU's OpenGL ICD is associated with the process
    //     (the GUI path already did this via
    //     RHI::init(true)->CreateWindowDevice AndSwapChain; the headless path
    //     skips glfwInit, so we do it here).
    //   - host the GL context on a real window (the main GLFW window's HWND if
    //     we have one, else a hidden dedicated window). The main GLFW window is
    //     created with GLFW_CLIENT_API=GLFW_NO_API (DeviceManager.cpp), so it
    //     never had its pixel format set and can safely host a WGL context.
    //   - plain wglCreateContext (NOT wglCreateContextAttribsARB+CORE_PROFILE).
    //     This yields the driver's highest COMPATIBILITY-profile context
    //     (typically 4.6 on modern GPUs). A core-profile context was the cause
    //     of pervasive GL_INVALID_ENUM: HgiGL_ScopedStateHolder restores
    //     GL_POINT_SMOOTH / GL_POINT_SPRITE / GL_MULTISAMPLE, whose enums are
    //     removed in core profile, so every restore produced GL_INVALID_ENUM,
    //     surfaced as a TF_RUNTIME_ERROR on every blit submit (crippling
    //     RelWithDebInfo and breaking the Storm renderer). Compatibility
    //     profile keeps those legacy enums valid.
    // Idempotent; the window/context leak intentionally so the context stays
    // current for downstream HgiGL/UsdImagingGL.
    static bool initialized = false;
    static bool ok = false;
    if (initialized)
        return ok;
    initialized = true;

#ifdef _WIN32
    auto logger = cached_logger.lock();

    // 1. Make sure the GPU OpenGL ICD is loaded for this process. glfwInit is
    //    idempotent and a no-op if RHI::init(true) already ran.
    if (!glfwInit()) {
        if (logger)
            logger->error("ensure_gl_driver_loaded: glfwInit failed");
        return ok = false;
    }

    // 2. Choose the host window HWND.
    HWND hwnd = nullptr;
    HDC hdc = nullptr;
    bool owns_window = false;

    // 2a. GUI path: reuse the main GLFW window's native HWND. It was created
    //     with GLFW_NO_API, so it has no GL context/pixel-format yet — we can
    //     attach one without conflict, and we avoid a second window that could
    //     desync driver state.
    if (auto* mgr = internal::get_device_manager()) {
        if (GLFWwindow* glfwWin = mgr->GetWindow()) {
            hwnd = glfwGetWin32Window(glfwWin);
        }
    }

    // 2b. Headless path (no main window): create a hidden dedicated window,
    //     matching GarchGLDebugWindow's approach but never shown.
    if (!hwnd) {
        WNDCLASSA wc = {};
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = "RuzinoGLContext";
        RegisterClassA(&wc);
        hwnd = CreateWindowExA(
            0,
            "RuzinoGLContext",
            "",
            0,
            0,
            0,
            1,
            1,
            nullptr,
            nullptr,
            GetModuleHandle(nullptr),
            nullptr);
        if (!hwnd) {
            if (logger)
                logger->error("ensure_gl_driver_loaded: CreateWindowEx failed");
            return ok = false;
        }
        owns_window = true;
    }

    hdc = GetDC(hwnd);
    if (!hdc) {
        if (logger)
            logger->error("ensure_gl_driver_loaded: GetDC failed");
        return ok = false;
    }

    // 3. Pixel format — matches GarchGLDebugWindow (RGBA8 + depth24 +
    //    stencil8 + doublebuffer).
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cRedBits = 8;
    pfd.cGreenBits = 8;
    pfd.cBlueBits = 8;
    pfd.cAlphaBits = 8;
    pfd.cColorBits = 24;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;

    int pixelFormat = ChoosePixelFormat(hdc, &pfd);
    if (pixelFormat == 0) {
        if (logger)
            logger->error(
                "ensure_gl_driver_loaded: ChoosePixelFormat returned 0; "
                "no hardware OpenGL ICD is loaded (GL will be MS software 1.1 "
                "and HgiGL will reject it)");
        return ok = false;
    }
    SetPixelFormat(hdc, pixelFormat, &pfd);

    // 4. Plain wglCreateContext -> compatibility profile (NOT core). See the
    //    block comment at the top of this function for why core profile broke
    //    HgiGL_ScopedStateHolder's state restore.
    HGLRC ctx = wglCreateContext(hdc);
    if (!ctx) {
        if (logger)
            logger->error(
                "ensure_gl_driver_loaded: wglCreateContext failed (err={})",
                GetLastError());
        return ok = false;
    }
    wglMakeCurrent(hdc, ctx);

    // 5. Verify we actually got a hardware context at OpenGL >=4.5 (HgiGL's
    //    hard requirement). A software context reports "1.1.0 Microsoft" and
    //    must be treated as failure.
    //    NB: resolve glGetString straight from opengl32.dll via GetProcAddress.
    //    We can't call glGetString() directly here because garch/glApi.h
    //    (included for HgiGL interop) re-declares it as a pxr::internal::GLApi
    //    function-pointer that is only populated by GarchGLApiLoad() — which
    //    the caller runs AFTER this function. Going through GetProcAddress
    //    sidesteps both the garch pointer (still null) and the link-time
    //    dependency on the garch symbol, and works on any GL 1.1+ context.
    using PFNGLGETSTRING = const unsigned char*(APIENTRY*)(unsigned int);
    PFNGLGETSTRING pfnGlGetString = reinterpret_cast<PFNGLGETSTRING>(
        GetProcAddress(GetModuleHandleA("opengl32.dll"), "glGetString"));
    const char* glVer =
        pfnGlGetString
            ? reinterpret_cast<const char*>(pfnGlGetString(GL_VERSION))
            : nullptr;
    const char* glVendor =
        pfnGlGetString
            ? reinterpret_cast<const char*>(pfnGlGetString(GL_VENDOR))
            : nullptr;
    const char* glRenderer =
        pfnGlGetString
            ? reinterpret_cast<const char*>(pfnGlGetString(GL_RENDERER))
            : nullptr;

    int major = 0, minor = 0;
    if (glVer) {
        // GL_VERSION looks like "4.6.0 <vendor> <version>" or "4.1
        // <vendor-...>"
        const char* dot = strchr(glVer, '.');
        if (dot && dot != glVer) {
            major = std::max(0, std::min(9, *(dot - 1) - '0'));
            minor = std::max(0, std::min(9, *(dot + 1) - '0'));
        }
    }
    int glVersionCode = major * 100 + minor * 10;

    if (logger) {
        logger->info(
            "ensure_gl_driver_loaded: GL {} | vendor '{}' | renderer '{}'",
            glVer ? glVer : "(null)",
            glVendor ? glVendor : "(null)",
            glRenderer ? glRenderer : "(null)");
    }

    if (glVersionCode < 450) {
        if (logger)
            logger->error(
                "ensure_gl_driver_loaded: OpenGL {}.{} is below HgiGL's 4.5 "
                "minimum; Storm/HgiGL rendering will fail. "
                "(owns_window={})",
                major,
                minor,
                owns_window);
        // Leave the context current anyway — HgiGL's own check will warn too,
        // and at least the process won't crash on a null context.
    }

    // The window/context intentionally leak: HgiGL needs the context to remain
    // current for its entire lifetime (it captures/restores GL state on every
    // submit). Destroying the window or releasing the DC would invalidate it.
    return ok = (glVersionCode >= 450);
#else
    return ok = false;
#endif
#else
    return false;
#endif
}

int shutdown()
{
    if (!device_manager) {
        if (auto logger = cached_logger.lock()) {
            logger->warn("RHI is not initialized, cannot shutdown");
        }
        return -1;
    }

    reference_count--;
    if (auto logger = cached_logger.lock()) {
        logger->info(
            "RHI shutdown called, reference count: {}", reference_count);
    }

    if (reference_count > 0) {
        return 0;
    }

    std::map<std::string, nvrhi_image>().swap(rhi_images);
    device_manager->Shutdown();
    device_manager.reset();
    reference_count = 0;
    cached_logger.reset();
    return device_manager == nullptr ? 0 : -1;
}
}  // namespace RHI
RUZINO_NAMESPACE_CLOSE_SCOPE
