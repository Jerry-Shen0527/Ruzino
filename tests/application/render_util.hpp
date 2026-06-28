#pragma once

#include <spdlog/fwd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "RHI/rhi.hpp"
#include "pxr/base/gf/camera.h"
#include "pxr/base/gf/frustum.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hgi/blitCmdsOps.h"
#include "pxr/imaging/hgi/tokens.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/camera.h"
#include "pxr/usdImaging/usdImagingGL/engine.h"

// Image saving
#include "pxr/imaging/garch/glApi.h"
#include "stb_image_write.h"

// USD Hio for HDR/EXR support
#include "pxr/imaging/hio/image.h"

RUZINO_NAMESPACE_OPEN_SCOPE
class DescriptorTableManager;
RUZINO_NAMESPACE_CLOSE_SCOPE
#include "pxr/imaging/hio/types.h"

// NVRHI includes
#include "nvrhi/nvrhi.h"

#ifdef _WIN32
#include <gl/GL.h>
#include <windows.h>
#endif

namespace RenderUtil {

using namespace pxr;

// USD utilities
inline UsdGeomCamera GetCamera(
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
inline void CreateGLContext()
{
    // Create a hardware-accelerated OpenGL 4.5 context via GLFW and make it
    // current. (The old wglCreateContext-on-the-console-window approach only
    // yields software GL 1.1, which Hydra rejects.) GarchGLApiLoad must run
    // after a context is current to bind the GL function pointers.
    // (RHI lives in the Ruzino namespace; this helper is in global RenderUtil.)
    if (!::Ruzino::RHI::ensure_gl_driver_loaded()) {
        std::cerr << "Error: failed to create OpenGL 4.5 context" << std::endl;
    }
}

// Image utilities
inline std::string GetFileExtension(const std::string& filename)
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

inline std::string GenerateSequenceFilename(
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

// Helper function to get bytes per pixel for HgiFormat
inline size_t GetBytesPerPixel(HgiFormat format)
{
    switch (format) {
        // UNorm8 formats
        case HgiFormatUNorm8: return 1;
        case HgiFormatUNorm8Vec2: return 2;
        case HgiFormatUNorm8Vec4: return 4;
        case HgiFormatUNorm8Vec4srgb: return 4;

        // SNorm8 formats
        case HgiFormatSNorm8: return 1;
        case HgiFormatSNorm8Vec2: return 2;
        case HgiFormatSNorm8Vec4: return 4;

        // Float16 formats
        case HgiFormatFloat16: return 2;
        case HgiFormatFloat16Vec2: return 4;
        case HgiFormatFloat16Vec3: return 6;
        case HgiFormatFloat16Vec4: return 8;

        // Float32 formats
        case HgiFormatFloat32: return 4;
        case HgiFormatFloat32Vec2: return 8;
        case HgiFormatFloat32Vec3: return 12;
        case HgiFormatFloat32Vec4: return 16;

        // Int16 formats
        case HgiFormatInt16: return 2;
        case HgiFormatInt16Vec2: return 4;
        case HgiFormatInt16Vec3: return 6;
        case HgiFormatInt16Vec4: return 8;

        // UInt16 formats
        case HgiFormatUInt16: return 2;
        case HgiFormatUInt16Vec2: return 4;
        case HgiFormatUInt16Vec3: return 6;
        case HgiFormatUInt16Vec4: return 8;

        // Int32 formats
        case HgiFormatInt32: return 4;
        case HgiFormatInt32Vec2: return 8;
        case HgiFormatInt32Vec3: return 12;
        case HgiFormatInt32Vec4: return 16;

        // Packed format
        case HgiFormatPackedInt1010102: return 4;

        // Depth stencil
        case HgiFormatFloat32UInt8: return 8;

        default:
            spdlog::warn(
                "Unknown HgiFormat: {}, defaulting to 16 bytes per pixel",
                static_cast<int>(format));
            return 16;  // Default to Float32Vec4
    }
}

inline bool SaveImageToFile(
    const std::string& filename,
    int width,
    int height,
    const std::vector<uint8_t>& data,
    HgiFormat source_format = HgiFormatFloat32Vec4,
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

    std::string ext = GetFileExtension(filename);

    // Convert source data to float RGBA based on format
    std::vector<float> rgba_float(width * height * 4);
    const uint8_t* src_data = data.data();

    // Parse based on source format
    if (source_format == HgiFormatFloat32Vec4) {
        const float* float_data = reinterpret_cast<const float*>(src_data);
        std::copy(
            float_data, float_data + width * height * 4, rgba_float.begin());
    }
    else if (source_format == HgiFormatFloat32Vec3) {
        const float* float_data = reinterpret_cast<const float*>(src_data);
        for (int i = 0; i < width * height; ++i) {
            rgba_float[i * 4 + 0] = float_data[i * 3 + 0];
            rgba_float[i * 4 + 1] = float_data[i * 3 + 1];
            rgba_float[i * 4 + 2] = float_data[i * 3 + 2];
            rgba_float[i * 4 + 3] = 1.0f;
        }
    }
    else if (source_format == HgiFormatFloat16Vec4) {
        const uint16_t* half_data = reinterpret_cast<const uint16_t*>(src_data);
        for (int i = 0; i < width * height * 4; ++i) {
            // Simple half to float conversion
            uint16_t h = half_data[i];
            uint32_t sign = (h & 0x8000) << 16;
            uint32_t exp = (h & 0x7C00) >> 10;
            uint32_t mant = (h & 0x03FF) << 13;

            if (exp == 0) {
                rgba_float[i] = 0.0f;
            }
            else if (exp == 31) {
                uint32_t f_bits = sign | 0x7F800000 | mant;
                rgba_float[i] = *reinterpret_cast<float*>(&f_bits);
            }
            else {
                uint32_t f_bits = sign | ((exp + 112) << 23) | mant;
                rgba_float[i] = *reinterpret_cast<float*>(&f_bits);
            }
        }
    }
    else if (
        source_format == HgiFormatUNorm8Vec4 ||
        source_format == HgiFormatUNorm8Vec4srgb) {
        for (int i = 0; i < width * height * 4; ++i) {
            rgba_float[i] = src_data[i] / 255.0f;
        }
    }
    else {
        spdlog::warn(
            "Unsupported source format: {}, assuming Float32Vec4",
            static_cast<int>(source_format));
        const float* float_data = reinterpret_cast<const float*>(src_data);
        std::copy(
            float_data, float_data + width * height * 4, rgba_float.begin());
    }

    // Check if it's a HDR/EXR format
    if (ext == "exr" || ext == "hdr") {
        spdlog::info("Saving as HDR format: {}", ext);

        // Create flipped float data
        std::vector<float> flipped_data(width * height * 4);
        for (int y = 0; y < height; ++y) {
            int flipped_y = height - 1 - y;
            for (int x = 0; x < width; ++x) {
                for (int c = 0; c < 4; ++c) {
                    int src_idx = (y * width + x) * 4 + c;
                    int dst_idx = (flipped_y * width + x) * 4 + c;
                    flipped_data[dst_idx] = rgba_float[src_idx];
                }
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
                for (int c = 0; c < 4; ++c) {
                    int src_idx = (y * width + x) * 4 + c;
                    int dst_idx = (flipped_y * width + x) * 4 + c;
                    rgba_data[dst_idx] = static_cast<uint8_t>(
                        std::clamp(rgba_float[src_idx] * 255.0f, 0.0f, 255.0f));
                }
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
inline bool ReadTextureDirectly(
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
        command_list = Ruzino::RHI::get_device()->createCommandList();
    }

    if (!staging_texture || staging_texture->getDesc().width != width ||
        staging_texture->getDesc().height != height) {
        nvrhi::TextureDesc staging_desc;
        staging_desc.debugName = "headless_staging";
        staging_desc.width = width;
        staging_desc.height = height;
        staging_desc.format = texture->getDesc().format;
        staging_desc.initialState = nvrhi::ResourceStates::CopyDest;

        staging_texture = Ruzino::RHI::get_device()->createStagingTexture(
            staging_desc, nvrhi::CpuAccessMode::Read);
    }

    // Single command list operation
    command_list->open();
    command_list->copyTexture(staging_texture, {}, texture, {});
    command_list->close();
    Ruzino::RHI::get_device()->executeCommandList(command_list.Get());
    Ruzino::RHI::get_device()->waitForIdle();

    // Direct memory copy without row-by-row iteration
    size_t pitch;
    auto mapped = Ruzino::RHI::get_device()->mapStagingTexture(
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

    Ruzino::RHI::get_device()->unmapStagingTexture(staging_texture);
    spdlog::info("Direct texture copy completed successfully");
    return true;
}

inline bool ReadTextureCPU(
    UsdImagingGLEngine* renderer,
    HgiUniquePtr& hgi,
    int width,
    int height,
    std::vector<uint8_t>& texture_data)
{
    spdlog::info("Using CPU readback method...");

    auto hgi_texture = renderer->GetAovTexture(HdAovTokens->color);
    if (!hgi_texture) {
        std::cerr << "Error: Failed to get rendered texture" << std::endl;
        return false;
    }

    auto tex_desc = hgi_texture->GetDescriptor();
    size_t bytes_per_pixel = GetBytesPerPixel(tex_desc.format);
    size_t buffer_size = width * height * bytes_per_pixel;

    spdlog::info(
        "Texture format: {}, bytes per pixel: {}, buffer size: {}",
        static_cast<int>(tex_desc.format),
        bytes_per_pixel,
        buffer_size);

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

inline std::string LoadJSONScript(const std::string& filename)
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

inline nvrhi::rt::IAccelStruct* GetTLAS(UsdImagingGLEngine* renderer)
{
    auto tlas_handle = renderer->GetRendererSetting(pxr::TfToken("VulkanTLAS"));
    if (!tlas_handle.IsHolding<const void*>()) {
        spdlog::warn("Failed to get TLAS from renderer");
        return nullptr;
    }

    spdlog::info("Successfully retrieved TLAS handle from renderer");

    auto bare_pointer = tlas_handle.Get<const void*>();
    auto tlas_ptr =
        static_cast<nvrhi::rt::IAccelStruct*>(const_cast<void*>(bare_pointer));

    return tlas_ptr;
}

inline nvrhi::IBuffer* GetInstanceDescBuffer(UsdImagingGLEngine* renderer)
{
    auto handle =
        renderer->GetRendererSetting(pxr::TfToken("VulkanInstanceDescBuffer"));
    if (!handle.IsHolding<const void*>()) {
        spdlog::warn("Failed to get InstanceDescBuffer from renderer");
        return nullptr;
    }

    auto bare_pointer = handle.Get<const void*>();
    return static_cast<nvrhi::IBuffer*>(const_cast<void*>(bare_pointer));
}

inline nvrhi::IBuffer* GetMeshDescBuffer(UsdImagingGLEngine* renderer)
{
    auto handle =
        renderer->GetRendererSetting(pxr::TfToken("VulkanMeshDescBuffer"));
    if (!handle.IsHolding<const void*>()) {
        spdlog::warn("Failed to get MeshDescBuffer from renderer");
        return nullptr;
    }

    auto bare_pointer = handle.Get<const void*>();
    return static_cast<nvrhi::IBuffer*>(const_cast<void*>(bare_pointer));
}

inline Ruzino::DescriptorTableManager* GetBindlessBufferTable(
    UsdImagingGLEngine* renderer)
{
    auto handle =
        renderer->GetRendererSetting(pxr::TfToken("VulkanBindlessBufferTable"));
    if (!handle.IsHolding<const void*>()) {
        spdlog::warn("Failed to get BindlessBufferTable from renderer");
        return nullptr;
    }

    auto bare_pointer = handle.Get<const void*>();
    return static_cast<Ruzino::DescriptorTableManager*>(
        const_cast<void*>(bare_pointer));
}

inline nvrhi::IBindingLayout* GetBindlessBufferLayout(
    UsdImagingGLEngine* renderer)
{
    auto handle = renderer->GetRendererSetting(
        pxr::TfToken("VulkanBindlessBufferLayout"));
    if (!handle.IsHolding<const void*>()) {
        spdlog::warn("Failed to get BindlessBufferLayout from renderer");
        return nullptr;
    }

    auto bare_pointer = handle.Get<const void*>();
    return static_cast<nvrhi::IBindingLayout*>(const_cast<void*>(bare_pointer));
}

}  // namespace RenderUtil
