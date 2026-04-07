#define FMT_UNICODE 0
#include <spdlog/spdlog.h>
#include <thrust/device_vector.h>
#include <thrust/host_vector.h>

#include <RHI/internal/cuda_extension.hpp>

RUZINO_NAMESPACE_OPEN_SCOPE
namespace cuda {
class CUDALinearBuffer : public nvrhi::RefCounter<cuda::ICUDALinearBuffer> {
   public:
    CUDALinearBuffer(const cuda::CUDALinearBufferDesc& in_desc);
    ~CUDALinearBuffer() override;

    const CUDALinearBufferDesc& getDesc() const override
    {
        return desc;
    }

    CUdeviceptr get_device_ptr() override;

   protected:
    thrust::host_vector<uint8_t> get_host_data() override;
    void assign_host_data(const thrust::host_vector<uint8_t>& data) override;
    void copy_from_device(ICUDALinearBuffer* src) override;

    const cuda::CUDALinearBufferDesc desc;
    thrust::device_vector<uint8_t> d_vec;

    friend CUDALinearBufferHandle create_cuda_linear_buffer(
        const CUDALinearBufferDesc& d,
        void* init_data);
};

CUDALinearBuffer::CUDALinearBuffer(const cuda::CUDALinearBufferDesc& in_desc)
    : desc(in_desc)
{
    d_vec.resize(desc.element_size * desc.element_count);
}

CUDALinearBuffer::~CUDALinearBuffer()
{
    d_vec.clear();
}

CUdeviceptr CUDALinearBuffer::get_device_ptr()
{
    return reinterpret_cast<CUdeviceptr>(d_vec.data().get());
}

thrust::host_vector<uint8_t> CUDALinearBuffer::get_host_data()
{
    thrust::host_vector<uint8_t> h_vec = d_vec;
    return h_vec;
}

void CUDALinearBuffer::assign_host_data(
    const thrust::host_vector<uint8_t>& data)
{
    d_vec = data;
}

void CUDALinearBuffer::copy_from_device(ICUDALinearBuffer* src)
{
    auto src_desc = src->getDesc();

    // Ensure buffers have compatible sizes
    size_t src_bytes = src_desc.element_count * src_desc.element_size;
    size_t dst_bytes = desc.element_count * desc.element_size;
    size_t copy_bytes = std::min(src_bytes, dst_bytes);

    cudaMemcpy(
        reinterpret_cast<void*>(get_device_ptr()),
        reinterpret_cast<const void*>(src->get_device_ptr()),
        copy_bytes,
        cudaMemcpyDeviceToDevice);
}

CUDALinearBufferHandle create_cuda_linear_buffer(
    const CUDALinearBufferDesc& d,
    void* init_data)
{
    auto buffer = new CUDALinearBuffer(d);

    if (init_data)
        buffer->assign_host_data(
            thrust::host_vector<uint8_t>(
                static_cast<uint8_t*>(init_data),
                static_cast<uint8_t*>(init_data) +
                    d.element_size * d.element_count));

    return CUDALinearBufferHandle::Create(buffer);
}

void copy_linear_buffer_to_surface(
    CUdeviceptr src_ptr,
    CUsurfObject surface,
    uint32_t width,
    uint32_t height,
    uint32_t element_size,
    uint32_t row_pitch)
{
#ifdef __CUDACC__
    GPUParallelFor2D(
        "copy_linear_buffer_to_surface",
        make_int2(width, height),
        GPU_LAMBDA_Ex(int y, int x) {
            if (x >= width || y >= height)
                return;

            // Calculate source offset
            uint32_t src_offset = y * row_pitch + x * element_size;
            const uint8_t* src_data = reinterpret_cast<const uint8_t*>(src_ptr);

            // Copy data based on element size
            if (element_size == 1) {
                uint8_t value = src_data[src_offset];
                surf2Dwrite(value, surface, x * sizeof(uint8_t), y);
            }
            else if (element_size == 2) {
                uint16_t value =
                    *reinterpret_cast<const uint16_t*>(&src_data[src_offset]);
                surf2Dwrite(value, surface, x * sizeof(uint16_t), y);
            }
            else if (element_size == 4) {
                uint32_t value =
                    *reinterpret_cast<const uint32_t*>(&src_data[src_offset]);
                surf2Dwrite(value, surface, x * sizeof(uint32_t), y);
            }
            else if (element_size == 8) {
                uint64_t value =
                    *reinterpret_cast<const uint64_t*>(&src_data[src_offset]);
                surf2Dwrite(value, surface, x * sizeof(uint64_t), y);
            }
            else if (element_size == 12) {
                // Handle 3-component data (RGB float, etc.)
                // Write as 3 consecutive 32-bit values
                const uint32_t* src_data32 =
                    reinterpret_cast<const uint32_t*>(&src_data[src_offset]);
                surf2Dwrite(
                    src_data32[0], surface, (x * 3 + 0) * sizeof(uint32_t), y);
                surf2Dwrite(
                    src_data32[1], surface, (x * 3 + 1) * sizeof(uint32_t), y);
                surf2Dwrite(
                    src_data32[2], surface, (x * 3 + 2) * sizeof(uint32_t), y);
            }
            else if (element_size == 16) {
                // Handle 4-component data (RGBA float, etc.)
                // Write as 4 consecutive 32-bit values or as uchar4 for RGBA8
                const uint32_t* src_data32 =
                    reinterpret_cast<const uint32_t*>(&src_data[src_offset]);
                surf2Dwrite(
                    src_data32[0], surface, (x * 4 + 0) * sizeof(uint32_t), y);
                surf2Dwrite(
                    src_data32[1], surface, (x * 4 + 1) * sizeof(uint32_t), y);
                surf2Dwrite(
                    src_data32[2], surface, (x * 4 + 2) * sizeof(uint32_t), y);
                surf2Dwrite(
                    src_data32[3], surface, (x * 4 + 3) * sizeof(uint32_t), y);
            }
        });
#else
    throw std::runtime_error(
        "CUDA compilation required for copy_linear_buffer_to_surface");
#endif
}

void copy_surface_to_linear_buffer(
    CUsurfObject surface,
    CUdeviceptr dst_ptr,
    uint32_t width,
    uint32_t height,
    uint32_t element_size,
    uint32_t row_pitch)
{
#ifdef __CUDACC__
    GPUParallelFor2D(
        "copy_surface_to_linear_buffer",
        make_int2(width, height),
        GPU_LAMBDA_Ex(int y, int x) {
            if (x >= width || y >= height)
                return;

            // Calculate destination offset
            uint32_t dst_offset = y * row_pitch + x * element_size;
            uint8_t* dst_data = reinterpret_cast<uint8_t*>(dst_ptr);

            // Read data based on element size
            if (element_size == 1) {
                uint8_t value;
                surf2Dread(&value, surface, x * sizeof(uint8_t), y);
                dst_data[dst_offset] = value;
            }
            else if (element_size == 2) {
                uint16_t value;
                surf2Dread(&value, surface, x * sizeof(uint16_t), y);
                *reinterpret_cast<uint16_t*>(&dst_data[dst_offset]) = value;
            }
            else if (element_size == 4) {
                uint32_t value;
                surf2Dread(&value, surface, x * sizeof(uint32_t), y);
                *reinterpret_cast<uint32_t*>(&dst_data[dst_offset]) = value;
            }
            else if (element_size == 8) {
                uint64_t value;
                surf2Dread(&value, surface, x * sizeof(uint64_t), y);
                *reinterpret_cast<uint64_t*>(&dst_data[dst_offset]) = value;
            }
            else if (element_size == 12) {
                // Handle 3-component data (RGB float, etc.)
                // Read as 3 consecutive 32-bit values
                uint32_t* dst_data32 =
                    reinterpret_cast<uint32_t*>(&dst_data[dst_offset]);
                surf2Dread(&dst_data32[0], surface, (x * 3 + 0) * sizeof(uint32_t), y);
                surf2Dread(&dst_data32[1], surface, (x * 3 + 1) * sizeof(uint32_t), y);
                surf2Dread(&dst_data32[2], surface, (x * 3 + 2) * sizeof(uint32_t), y);
            }
            else if (element_size == 16) {
                // Handle 4-component data (RGBA float, etc.)
                // Read as 4 consecutive 32-bit values
                uint32_t* dst_data32 =
                    reinterpret_cast<uint32_t*>(&dst_data[dst_offset]);
                surf2Dread(&dst_data32[0], surface, (x * 4 + 0) * sizeof(uint32_t), y);
                surf2Dread(&dst_data32[1], surface, (x * 4 + 1) * sizeof(uint32_t), y);
                surf2Dread(&dst_data32[2], surface, (x * 4 + 2) * sizeof(uint32_t), y);
                surf2Dread(&dst_data32[3], surface, (x * 4 + 3) * sizeof(uint32_t), y);
            }
        });
#else
    throw std::runtime_error(
        "CUDA compilation required for copy_surface_to_linear_buffer");
#endif
}
}  // namespace cuda

RUZINO_NAMESPACE_CLOSE_SCOPE
