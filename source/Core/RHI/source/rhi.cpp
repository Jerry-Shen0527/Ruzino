
#include <nvrhi/nvrhi.h>
#include <spdlog/spdlog.h>

#include <RHI/internal/nvrhi_equality.hpp>
#include <RHI/rhi.hpp>
#include <memory>

#include "RHI/DeviceManager/DeviceManager.h"
#include "nvrhi/utils.h"

#if RUZINO_WITH_OPENUSD
#include "pxr/imaging/garch/glApi.h"
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

    params.enableRayTracingExtensions = false;
    params.enableComputeQueue = false;
    params.enableCopyQueue = false;
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
    params.featureLevel = D3D_FEATURE_LEVEL_12_0;
#ifdef _DEBUG
    // params.enableNvrhiValidationLayer = true;
    // params.enableDebugRuntime = true;
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

inline void copy_from_texture(
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
