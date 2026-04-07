#pragma once
#if RUZINO_WITH_CUDA

#include <RHI/api.h>
#include <cuda.h>
#include <cuda_runtime.h>
#include <nvrhi/nvrhi.h>
#include <thrust/host_vector.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <typeindex>
#include <utility>

#include "cuda_extension_utils.h"
#include "optix/ShaderNameAbbre.h"
#include "optix/WorkQueue.cuh"
#include "optix/optix.h"
RUZINO_NAMESPACE_OPEN_SCOPE

namespace cuda {
RHI_API int cuda_init();
RHI_API int optix_init();
RHI_API int cuda_shutdown();

using OptixTraversableHandle = unsigned long long;
class OptiXTraversableDesc;

class IOptiXTraversable : public nvrhi::IResource {
   public:
    [[nodiscard]] virtual const OptiXTraversableDesc& getDesc() const = 0;

    virtual OptixTraversableHandle getOptiXTraversable() const = 0;
};

using OptiXTraversableHandle = nvrhi::RefCountPtr<IOptiXTraversable>;

class OptiXTraversableDesc {
   public:
    OptixBuildInput buildInput = {};
    OptixAccelBuildOptions buildOptions = {};

    std::vector<OptiXTraversableHandle> handles;
};

RHI_API OptiXTraversableHandle
create_optix_traversable(const OptiXTraversableDesc& d);

RHI_API OptiXTraversableHandle create_linear_curve_optix_traversable(
    std::vector<CUdeviceptr> vertexBuffer,
    unsigned int numVertices,
    std::vector<CUdeviceptr> widthBuffer,
    CUdeviceptr indexBuffer,
    unsigned int numPrimitives,
    bool rebuilding = false,
    OptixPrimitiveType primitive_type = OPTIX_PRIMITIVE_TYPE_ROUND_LINEAR);

/**
 *
 * @param vertexBuffer The size of the vector is for key frames
 * @param numVertices
 * @param vertexBufferStride
 * @param indexBuffer
 * @param numPrimitives
 * @param rebuilding
 * @return
 */
RHI_API OptiXTraversableHandle create_mesh_optix_traversable(
    std::vector<CUdeviceptr> vertexBuffer,
    unsigned int numVertices,
    unsigned int vertexBufferStride,
    CUdeviceptr indexBuffer,
    unsigned int numPrimitives,
    bool rebuilding = false);

struct CUDALinearBufferDesc {
    unsigned element_count;
    unsigned element_size;

    CUDALinearBufferDesc(unsigned element_count = 0, unsigned element_size = 0)
        : element_size(element_size),
          element_count(element_count)
    {
    }

    friend class CUDALinearBuffer;
};

class ICUDALinearBuffer : public nvrhi::IResource {
   public:
    virtual ~ICUDALinearBuffer() = default;
    [[nodiscard]] virtual const CUDALinearBufferDesc& getDesc() const = 0;
    virtual CUdeviceptr get_device_ptr() = 0;

    // Smart device pointer that returns typed pointer
    template<typename T>
    T* get_device_ptr()
    {
        return reinterpret_cast<T*>(get_device_ptr());
    }

    // Const version
    template<typename T>
    const T* get_device_ptr() const
    {
        return reinterpret_cast<const T*>(
            const_cast<ICUDALinearBuffer*>(this)->get_device_ptr());
    }
    template<typename T>
    std::vector<T> get_host_vector()
    {
        auto host_data = get_host_data();
        auto data_ptr = host_data.data();
        auto count = getDesc().element_count;

        auto ret = std::vector<T>(
            reinterpret_cast<T*>(data_ptr),
            reinterpret_cast<T*>(data_ptr) + count);
        return ret;
    }

    template<typename T>
    T get_host_value()
    {
        auto host_data = get_host_data();
        auto data_ptr = host_data.data();
        return *reinterpret_cast<T*>(data_ptr);
    }

    template<typename T>
    void assign_host_value(const T& data)
    {
        auto host_data = thrust::host_vector<uint8_t>(
            reinterpret_cast<const uint8_t*>(&data),
            reinterpret_cast<const uint8_t*>(&data + 1));
        assign_host_data(host_data);
    }

    template<typename T>
    void assign_host_vector(const std::vector<T>& data)
    {
        auto host_data = thrust::host_vector<uint8_t>(
            reinterpret_cast<const uint8_t*>(data.data()),
            reinterpret_cast<const uint8_t*>(data.data() + data.size()));
        assign_host_data(host_data);
    }

    // Device-to-device copy from another buffer
    virtual void copy_from_device(ICUDALinearBuffer* src) = 0;

   protected:
    virtual thrust::host_vector<uint8_t> get_host_data() = 0;
    virtual void assign_host_data(const thrust::host_vector<uint8_t>& data) = 0;
};
using CUDALinearBufferHandle = nvrhi::RefCountPtr<ICUDALinearBuffer>;
RHI_API CUDALinearBufferHandle create_cuda_linear_buffer(
    const CUDALinearBufferDesc& d,
    void* init_data = nullptr);

// Create buffer with specified count (no default to avoid ambiguity)
template<typename T>
CUDALinearBufferHandle create_cuda_linear_buffer(size_t count)
{
    CUDALinearBufferDesc desc(count, sizeof(T));
    auto ret = create_cuda_linear_buffer(desc);
    return ret;
}

// Create buffer with initial value and count
template<typename T>
CUDALinearBufferHandle create_cuda_linear_buffer_with_value(
    const T& init_value,
    size_t count)
{
    CUDALinearBufferDesc desc(count, sizeof(T));
    auto ret = create_cuda_linear_buffer(desc);
    ret->assign_host_vector(std::vector(count, init_value));
    return ret;
}

// Create buffer from vector
template<typename T>
CUDALinearBufferHandle create_cuda_linear_buffer(const std::vector<T>& d)
{
    CUDALinearBufferDesc desc(d.size(), sizeof(T));
    auto ret = create_cuda_linear_buffer(desc);
    ret->assign_host_vector(d);
    return ret;
}

RHI_API CUDALinearBufferHandle
borrow_cuda_linear_buffer(const CUDALinearBufferDesc& desc, void* cuda_ptr);

class RHI_API OptiXProgramGroupDesc {
   public:
    OptixProgramGroupOptions program_group_options = {};
    OptiXProgramGroupDesc& set_program_group_kind(OptixProgramGroupKind kind);
    OptiXProgramGroupDesc& set_entry_name(const char* name);
    OptiXProgramGroupDesc&
    set_entry_name(const char* is, const char* ahs, const char* chs);

   protected:
    OptixProgramGroupDesc prog_group_desc;

    friend class OptiXProgramGroup;
};

class OptiXPipelineDesc {
   public:
    OptixPipelineCompileOptions pipeline_compile_options;
    OptixPipelineLinkOptions pipeline_link_options;
};

class OptiXModuleDesc {
   public:
    OptixModuleCompileOptions module_compile_options;
    OptixPipelineCompileOptions pipeline_compile_options;
    OptixBuiltinISOptions builtinISOptions;

    std::string file_name;
};

class IOptiXProgramGroup : public nvrhi::IResource {
   public:
    [[nodiscard]] virtual const OptiXProgramGroupDesc& getDesc() const = 0;

    virtual OptixProgramGroupKind getKind() const = 0;

   protected:
    virtual OptixProgramGroup getProgramGroup() const = 0;
    friend class OptiXPipeline;
};

class IOptiXModule : public nvrhi::IResource {
   public:
    [[nodiscard]] virtual const OptiXModuleDesc& getDesc() const = 0;

   protected:
    virtual OptixModule getModule() const = 0;
    friend class OptiXProgramGroup;
};

class IOptiXPipeline : public nvrhi::IResource {
   public:
    [[nodiscard]] virtual const OptiXPipelineDesc& getDesc() const = 0;

    virtual OptixPipeline getPipeline() const = 0;
    virtual OptixShaderBindingTable getSbt() const = 0;
};
using OptiXModuleHandle = nvrhi::RefCountPtr<IOptiXModule>;
using OptiXPipelineHandle = nvrhi::RefCountPtr<IOptiXPipeline>;
using OptiXProgramGroupHandle = nvrhi::RefCountPtr<IOptiXProgramGroup>;

RHI_API void add_extra_relative_include_dir_for_optix(const std::string& dir);

RHI_API const char* get_ptx_string_from_cu(
    const char* filename,
    const char** log = nullptr);

RHI_API OptiXModuleHandle create_optix_module(const OptiXModuleDesc& d);
RHI_API OptiXModuleHandle create_optix_module(
    const std::string& file_path,
    const char* param_name = "params");

RHI_API OptiXModuleHandle get_builtin_module(
    OptixPrimitiveType primitive_type,
    const char* param_name = "params");

RHI_API OptiXProgramGroupHandle create_optix_program_group(
    const OptiXProgramGroupDesc& d,
    OptiXModuleHandle module);

RHI_API OptiXProgramGroupHandle create_optix_program_group(
    const OptiXProgramGroupDesc& d,
    std::tuple<OptiXModuleHandle, OptiXModuleHandle, OptiXModuleHandle>
        modules);

RHI_API OptiXProgramGroupHandle create_optix_raygen(
    const std::string& file_path,
    const char* entry_name,
    const char* param_name = "params");

RHI_API OptiXProgramGroupHandle create_optix_miss(
    const std::string& file_path,
    const char* entry_name,
    const char* param_name = "params");

RHI_API OptiXPipelineHandle create_optix_pipeline(
    const OptiXPipelineDesc& d,
    std::vector<OptiXProgramGroupHandle> program_groups = {});

RHI_API OptiXPipelineHandle create_optix_pipeline(
    std::vector<OptiXProgramGroupHandle> program_groups = {},
    const char* param_name = "params");

RHI_API cudaStream_t get_optix_stream();
RHI_API int optix_trace_ray(
    OptiXTraversableHandle traversable,
    OptiXPipelineHandle handle,
    CUdeviceptr launch_params,
    unsigned launch_params_size,
    int x,
    int y,
    int z);

template<typename OptixLaunchParams>
inline int optix_trace_ray(
    OptiXTraversableHandle traversable,
    OptiXPipelineHandle handle,
    CUdeviceptr launch_params,
    int x,
    int y,
    int z)
{
    return optix_trace_ray(
        traversable, handle, launch_params, sizeof(OptixLaunchParams), x, y, z);
}

template<typename T>
struct AppendStructuredBuffer {
    AppendStructuredBuffer() = default;
    AppendStructuredBuffer(unsigned max_size)
    {
        workqueue_buffer = create_cuda_linear_buffer<T>(max_size);
        d_workqueue = create_cuda_linear_buffer<WorkQueue<T>>();
        d_workqueue->assign_host_value(
            WorkQueue{
                reinterpret_cast<T*>(workqueue_buffer->get_device_ptr()) });
    }

    void reset()
    {
        d_workqueue->assign_host_value(
            WorkQueue{
                reinterpret_cast<T*>(workqueue_buffer->get_device_ptr()) });
    }

    WorkQueue<T>* get_device_queue_ptr()
    {
        return reinterpret_cast<WorkQueue<T>*>(d_workqueue->get_device_ptr());
    }

    CUdeviceptr get_underlying_buffer_ptr()
    {
        return workqueue_buffer->get_device_ptr();
    }

    size_t get_size()
    {
        return d_workqueue->get_host_value<WorkQueue<T>>().size;
    }

   private:
    CUDALinearBufferHandle d_workqueue;
    CUDALinearBufferHandle workqueue_buffer;
};

#define HOST_DEVICE __host__ __device__

template<typename F>
inline int GetBlockSize(const char* description, F kernel)
{
    // Note: this isn't reentrant, but that's fine for our purposes...
    static std::map<std::type_index, int> kernelBlockSizes;

    auto index = std::type_index(typeid(F));

    auto iter = kernelBlockSizes.find(index);
    if (iter != kernelBlockSizes.end())
        return iter->second;

    int minGridSize, blockSize;
    CUDA_CHECK(cudaOccupancyMaxPotentialBlockSize(
        &minGridSize, &blockSize, kernel, 0, 0));
    kernelBlockSizes[index] = blockSize;

    return blockSize;
}

template<typename F>
void GPUParallelFor(const char* description, int nItems, F func);

template<typename F>
void GPUParallelFor2D(const char* description, int2 resolution, F func);
#ifdef __CUDACC__

template<typename F>
__global__ void Kernel(F func, int nItems)
{
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= nItems)
        return;

    func(tid);
}

template<typename F>
__global__ void Kernel2D(F func, int2 resolution)
{
    int tid = blockIdx.x * blockDim.x + threadIdx.x;

    int i = tid / resolution.x;  // row num
    int j = tid % resolution.x;  // collum num

    if (i >= resolution.y)
        return;

    func(i, j);
}

#include <iostream>

template<typename F>
void GPUParallelFor(const char* description, int nItems, F func)
{
#ifdef NVTX
    nvtxRangePush(description);
#endif

    if (nItems > 0) {
        auto kernel = &Kernel<F>;

        int blockSize = GetBlockSize(description, kernel);

        int gridSize = (nItems + blockSize - 1) / blockSize;
        kernel<<<gridSize, blockSize>>>(func, nItems);
        CUDA_SYNC_CHECK();
    }

#ifdef NVTX
    nvtxRangePop();
#endif
}

template<typename F>
void GPUParallelFor2D(const char* description, int2 resolution, F func)
{
#ifdef NVTX
    nvtxRangePush(description);
#endif

    auto kernel = &Kernel2D<F>;

    int blockSize = GetBlockSize(description, kernel);

    int gridSize = (resolution.x * resolution.y + blockSize - 1) / blockSize;
    kernel<<<gridSize, blockSize>>>(func, resolution);
    CUDA_SYNC_CHECK();

#ifdef NVTX
    nvtxRangePop();
#endif
}

#endif

#define GPU_LAMBDA(...)    [ =, *this ] __device__(__VA_ARGS__) mutable
#define GPU_LAMBDA_Ex(...) [=] __device__(__VA_ARGS__) mutable

RHI_API nvrhi::TextureHandle cuda_linear_buffer_to_nvrhi_texture(
    nvrhi::IDevice* device,
    CUDALinearBufferHandle buffer,
    nvrhi::TextureDesc desc);

RHI_API CUDALinearBufferHandle nvrhi_texture_to_cuda_linear_buffer(
    nvrhi::IDevice* device,
    nvrhi::ITexture* texture,
    uint32_t element_size);

// External memory management structure
struct ExternalMemoryResources {
    cudaExternalMemory_t externalMemory = nullptr;
    CUsurfObject surface = 0;

    ~ExternalMemoryResources()
    {
        cleanup();
    }

    void cleanup()
    {
        if (surface != 0) {
            cudaDestroySurfaceObject(surface);
            surface = 0;
        }
        if (externalMemory != nullptr) {
            cudaDestroyExternalMemory(externalMemory);
            externalMemory = nullptr;
        }
    }
};

using ExternalMemoryResourcesHandle = std::unique_ptr<ExternalMemoryResources>;

void copy_linear_buffer_to_surface(
    CUdeviceptr src_ptr,
    CUsurfObject surface,
    uint32_t width,
    uint32_t height,
    uint32_t element_size,
    uint32_t row_pitch);

void copy_surface_to_linear_buffer(
    CUsurfObject surface,
    CUdeviceptr dst_ptr,
    uint32_t width,
    uint32_t height,
    uint32_t element_size,
    uint32_t row_pitch);

// Enhanced functions that return external memory resources for proper cleanup
RHI_API ExternalMemoryResourcesHandle create_external_memory_surface(
    nvrhi::IDevice* device,
    nvrhi::ITexture* texture,
    uint32_t cudaUsageFlags = 0);

RHI_API void copy_linear_buffer_to_texture_with_cleanup(
    nvrhi::IDevice* device,
    CUDALinearBufferHandle buffer,
    nvrhi::ITexture* texture);

RHI_API CUDALinearBufferHandle copy_texture_to_linear_buffer_with_cleanup(
    nvrhi::IDevice* device,
    nvrhi::ITexture* texture,
    uint32_t element_size);

}  // namespace cuda

RUZINO_NAMESPACE_CLOSE_SCOPE

#endif
