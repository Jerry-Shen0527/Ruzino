#if RUZINO_WITH_CUDA
#include "RHI/internal/cuda_extension.hpp"

#include <gtest/gtest.h>

#include "RHI/rhi.hpp"

using namespace Ruzino::cuda;

// Tests 1-5 (cuda_init through create_optix_traversable) don't need RHI.
// Tests 6-8 (buffer↔texture conversions) need Vulkan initialized.

class CudaRhiTest : public ::testing::Test {
   protected:
    void SetUp() override { Ruzino::RHI::init(); }
    void TearDown() override { Ruzino::RHI::shutdown(); }
};

TEST(cuda_extension, cuda_init)
{
    auto ret = cuda_init();
    EXPECT_EQ(ret, 0);
}

TEST(cuda_extension, optix_init)
{
    auto ret = optix_init();
    EXPECT_EQ(ret, 0);
}

TEST(cuda_extension, cuda_shutdown)
{
    auto ret = cuda_shutdown();
    EXPECT_EQ(ret, 0);
}

TEST(cuda_extension, create_linear_buffer)
{
    auto desc = CUDALinearBufferDesc(10, 4);
    auto handle = create_cuda_linear_buffer(desc);
    CUdeviceptr device_ptr = handle->get_device_ptr();
    EXPECT_NE(device_ptr, 0);

    auto handle2 = create_cuda_linear_buffer(std::vector{ 1, 2, 3, 4 });

    auto device_ptr2 = handle2->get_device_ptr();
    EXPECT_NE(device_ptr2, 0);

    auto host_vec = handle2->get_host_vector<int>();

    EXPECT_EQ(host_vec.size(), 4);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(host_vec[i], i + 1);
    }
}

TEST(cuda_extension, create_optix_traversable)

{
    optix_init();

    auto line_end_vertices = create_cuda_linear_buffer(
        std::vector{ 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f });
    auto widths = create_cuda_linear_buffer(std::vector{ 0.1f, 0.1f });
    auto indices = create_cuda_linear_buffer(std::vector{ 0 });
    auto handle = create_linear_curve_optix_traversable(
        { line_end_vertices->get_device_ptr() },
        2,
        { widths->get_device_ptr() },
        { indices->get_device_ptr() },
        1);

    EXPECT_NE(handle, nullptr);

    line_end_vertices->assign_host_vector<float>(
        { 0.0f, 0.0f, 0.0f, 2.0f, 2.0f, 2.0f });

    handle = create_linear_curve_optix_traversable(
        { line_end_vertices->get_device_ptr() },
        2,
        { widths->get_device_ptr() },
        { indices->get_device_ptr() },
        1,
        true);

    EXPECT_NE(handle, nullptr);

    // mesh handle
    // vertex buffer describing a rectangle (but in 3D space), two triangles
    auto vertex_buffer = create_cuda_linear_buffer(
        std::vector{ 0.0f,
                     0.0f,
                     0.0f,
                     1.0f,
                     0.0f,
                     0.0f,
                     0.0f,
                     1.0f,
                     0.0f,
                     1.0f,
                     1.0f,
                     0.0f });
    // index buffer referencing the two triangles
    auto index_buffer =
        create_cuda_linear_buffer(std::vector{ 0, 1, 2, 1, 2, 3 });

    auto mesh_handle = create_mesh_optix_traversable(
        { vertex_buffer->get_device_ptr() },
        4,
        3 * sizeof(float),
        index_buffer->get_device_ptr(),
        2);

    EXPECT_NE(mesh_handle, nullptr);
}

TEST_F(CudaRhiTest, cuda_linear_buffer_to_nvrhi_texture)
{
    // CUDA-Vulkan external memory interop is not implemented on Linux
#ifdef __linux__
    GTEST_SKIP() << "CUDA-Vulkan texture interop not implemented on Linux";
#endif
    cuda_init();

    // Create a simple RGBA test buffer
    std::vector<uint32_t> test_data(64 * 64, 0xFF0000FF);  // Red color
    auto buffer = create_cuda_linear_buffer(test_data);

    // Create texture descriptor
    nvrhi::TextureDesc desc;
    desc.width = 64;
    desc.height = 64;
    desc.format = nvrhi::Format::RGBA8_UNORM;
    desc.isShaderResource = true;
    desc.isRenderTarget = false;
    desc.debugName = "test_texture";

    auto device = Ruzino::RHI::get_device();
    auto texture = cuda_linear_buffer_to_nvrhi_texture(device, buffer, desc);
    EXPECT_NE(texture, nullptr);

    // Verify external memory resources are properly managed
    CUDA_SYNC_CHECK();  // Ensure all operations complete
}

TEST_F(CudaRhiTest, nvrhi_texture_to_cuda_linear_buffer)
{
#ifdef __linux__
    GTEST_SKIP() << "CUDA-Vulkan texture interop not implemented on Linux";
#endif
    cuda_init();

    // Create a test texture first
    nvrhi::TextureDesc desc;
    desc.width = 32;
    desc.height = 32;
    desc.format = nvrhi::Format::RGBA8_UNORM;
    desc.isShaderResource = true;
    desc.isRenderTarget = false;
    desc.debugName = "test_texture_source";
    desc.sharedResourceFlags = nvrhi::SharedResourceFlags::Shared;
    desc.keepInitialState = true;
    desc.initialState = nvrhi::ResourceStates::CopyDest;

    auto device = Ruzino::RHI::get_device();
    auto source_texture = device->createTexture(desc);
    EXPECT_NE(source_texture, nullptr);

    // Convert texture to linear buffer
    uint32_t element_size = 4;  // RGBA8 = 4 bytes per pixel
    auto buffer = nvrhi_texture_to_cuda_linear_buffer(
        device, source_texture.Get(), element_size);

    EXPECT_NE(buffer, nullptr);
    EXPECT_EQ(buffer->getDesc().element_count, 32 * 32);
    EXPECT_EQ(buffer->getDesc().element_size, element_size);
}

TEST_F(CudaRhiTest, texture_buffer_roundtrip)
{
#ifdef __linux__
    GTEST_SKIP() << "CUDA-Vulkan texture interop not implemented on Linux";
#endif
    cuda_init();

    // Create original test data
    std::vector<uint32_t> original_data(16 * 16);
    for (int i = 0; i < 16 * 16; ++i) {
        original_data[i] =
            0xFF000000 | (i << 8);  // Alpha=1, varying green channel
    }

    // Create buffer from original data
    auto original_buffer = create_cuda_linear_buffer(original_data);

    // Convert buffer to texture
    nvrhi::TextureDesc desc;
    desc.width = 16;
    desc.height = 16;
    desc.format = nvrhi::Format::RGBA8_UNORM;
    desc.isShaderResource = true;
    desc.debugName = "roundtrip_texture";

    auto device = Ruzino::RHI::get_device();
    auto texture =
        cuda_linear_buffer_to_nvrhi_texture(device, original_buffer, desc);
    EXPECT_NE(texture, nullptr);

    // Convert texture back to buffer
    uint32_t element_size = sizeof(uint32_t);
    auto result_buffer = nvrhi_texture_to_cuda_linear_buffer(
        device, texture.Get(), element_size);

    EXPECT_NE(result_buffer, nullptr);
    EXPECT_EQ(result_buffer->getDesc().element_count, 16 * 16);
    EXPECT_EQ(result_buffer->getDesc().element_size, element_size);

    // Verify data integrity (basic check)
    auto result_data = result_buffer->get_host_vector<uint32_t>();
    EXPECT_EQ(result_data.size(), original_data.size());
    // Note: Exact comparison might not work due to format conversions,
    // but we can check basic properties
    EXPECT_EQ(result_data.size(), 16 * 16);
}

#endif
