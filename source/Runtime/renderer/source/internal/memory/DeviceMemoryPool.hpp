#pragma once
#include <spdlog/spdlog.h>

#include <RHI/rhi.hpp>
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <sstream>

#include "../../api.h"
RUZINO_NAMESPACE_OPEN_SCOPE

HD_RUZINO_API extern std::mutex execution_launch_mutex;

template<typename T>
class DeviceMemoryPool {
   public:
    struct MemoryHandleData {
        static std::shared_ptr<MemoryHandleData> create();
        size_t offset = INVALID;
        size_t size = 0;

        void write_data(const void* data);
        void write_data(const void* data, size_t bias_count);

        size_t index() const
        {
            return offset / sizeof(T);
        }

        size_t count() const
        {
            return size / sizeof(T);
        }

        ~MemoryHandleData();

        // True while the owning pool is alive. Handles may outlive the pool
        // (nothing structurally prevents it); the token lets ~MemoryHandleData
        // detect that and skip free-list bookkeeping instead of dereferencing
        // the dead pool.
        std::shared_ptr<std::atomic<bool>> pool_alive;

        nvrhi::BindingSetItem get_descriptor(
            nvrhi::ResourceType type =
                nvrhi::ResourceType::StructuredBuffer_UAV) const;

        nvrhi::IBuffer* get_device_buffer() const;
        void read_data(void* data);

       private:
        static constexpr size_t INVALID = -1;
        DeviceMemoryPool* pool;
        friend class DeviceMemoryPool;
    };

    using MemoryHandle = std::shared_ptr<MemoryHandleData>;

    void Initialize();
    DeviceMemoryPool();

    std::vector<T> get_content() const;

    explicit DeviceMemoryPool(const nvrhi::BufferDesc& buffer_desc);

    DeviceMemoryPool(const DeviceMemoryPool&) = delete;
    // Deliberately non-movable: every live MemoryHandleData carries a raw
    // `pool` back-pointer, so moving the pool would strand all handles at
    // the old address. In-place compaction is compress(), which swaps
    // members while preserving object identity.
    DeviceMemoryPool(DeviceMemoryPool&&) = delete;
    DeviceMemoryPool& operator=(const DeviceMemoryPool&) = delete;
    DeviceMemoryPool& operator=(DeviceMemoryPool&&) = delete;

    void clear();
    void destroy();
    ~DeviceMemoryPool();

    bool compress();
    void reserve(size_t size);
    MemoryHandle allocate(size_t count = 1);

    nvrhi::IBuffer* get_device_buffer() const;
    size_t max_memory_offset() const;
    size_t pool_size() const;
    size_t count() const;

    std::string info(bool free_list = true) const;

    // For debugging
    bool sanitize();

    /// Read back exactly the LIVE entries, in slot order, as one dense
    /// array (staging readback + map; debug/sync-point use only, never
    /// per-frame hot path). This is the non-moving alternative to
    /// compress(): consumers needing a contiguous view (TLAS build input)
    /// gather into their own staging buffer instead of relocating pool
    /// data and invalidating every stored slot reference.
    std::vector<T> gather_active();

    /// True once, then resets: entries were freed since the last call.
    /// Replaces the "did compress() do work" signal that rebuild_tlas used
    /// to poll for — same wake-up semantics without any data movement.
    bool take_pending_changes();

   private:
    std::mutex buffer_write_mutex_;

    void relocate_buffer();
    void erase(MemoryHandleData* handle);

    // Assume the command list (of the other one) is already opened
    void adopt(MemoryHandleData* handle_from_another_pool);
    std::vector<MemoryHandleData*> handles_allocated;

    nvrhi::BufferHandle device_buffer;
    std::vector<std::pair<size_t, size_t>> h_free_list;  // offset, size
    size_t max_count = 1;
    size_t targeted_max_count = 1;
    bool deferred_rebuild_ = false;  // set by erase(), drained by
                                     // take_pending_changes()
    size_t current_count = 0;
    size_t current_max_memory_offset = 0;
    nvrhi::CommandListHandle commandList;

    // Handed to every allocated handle (see MemoryHandleData::pool_alive);
    // destroy() releases it before tearing down GPU resources.
    std::shared_ptr<std::atomic<bool>> alive_token_ =
        std::make_shared<std::atomic<bool>>(true);

    // Utility functions

    template<typename U>
    nvrhi::BufferDesc buffer_desc() const;

    nvrhi::BufferDesc base_desc_;
};

template<typename T>
void DeviceMemoryPool<T>::reserve(size_t size)
{
    while (size > targeted_max_count) {
        targeted_max_count *= 2;
    }

    relocate_buffer();
}

template<typename T>
std::shared_ptr<typename DeviceMemoryPool<T>::MemoryHandleData>
DeviceMemoryPool<T>::MemoryHandleData::create()
{
    return std::make_shared<MemoryHandleData>();
}

template<typename T>
void DeviceMemoryPool<T>::MemoryHandleData::write_data(const void* data)
{
    std::lock_guard lock(execution_launch_mutex);
    auto device_buffer = pool->get_device_buffer();

    pool->commandList->open();
    pool->commandList->writeBuffer(device_buffer, data, size, offset);

    pool->commandList->close();

    RHI::get_device()->executeCommandList(
        pool->commandList, nvrhi::CommandQueue::Copy);
}

template<typename T>
void DeviceMemoryPool<T>::MemoryHandleData::write_data(
    const void* data,
    size_t bias_count)
{
    std::lock_guard lock(execution_launch_mutex);
    auto device_buffer = pool->get_device_buffer();

    pool->commandList->open();
    pool->commandList->writeBuffer(
        device_buffer, data, sizeof(T), offset + bias_count * sizeof(T));

    pool->commandList->close();

    RHI::get_device()->executeCommandList(
        pool->commandList, nvrhi::CommandQueue::Copy);
}

template<typename T>
DeviceMemoryPool<T>::MemoryHandleData::~MemoryHandleData()
{
    if (pool_alive && pool_alive->load(std::memory_order_acquire)) {
        pool->erase(this);
    }
    else if (pool_alive) {
        spdlog::warn(
            "DeviceMemoryPool: handle outlived its pool, "
            "skipping free-list bookkeeping");
    }
}

template<typename T>
nvrhi::BindingSetItem DeviceMemoryPool<T>::MemoryHandleData::get_descriptor(
    nvrhi::ResourceType type) const
{
    nvrhi::BindingSetItem item;
    item.resourceHandle = pool->device_buffer;
    item.range = nvrhi::BufferRange{
        offset,
        size,
    };
    item.type = type;
    return item;
}

template<typename T>
nvrhi::IBuffer* DeviceMemoryPool<T>::MemoryHandleData::get_device_buffer() const
{
    return pool->device_buffer;
}

template<typename T>
void DeviceMemoryPool<T>::MemoryHandleData::read_data(void* data)
{
    std::lock_guard lock(execution_launch_mutex);

    // Create a staging buffer with CPU read access
    nvrhi::BufferDesc stagingDesc;
    stagingDesc.byteSize = size;
    stagingDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
    stagingDesc.debugName = "StagingReadbackBuffer";

    nvrhi::IDevice* device = RHI::get_device();
    nvrhi::BufferHandle stagingBuffer = device->createBuffer(stagingDesc);

    // Copy data from device buffer to staging buffer
    pool->commandList->open();
    pool->commandList->copyBuffer(
        stagingBuffer, 0, pool->device_buffer, offset, size);
    pool->commandList->close();

    // Execute the command list and wait for completion
    device->executeCommandList(pool->commandList, nvrhi::CommandQueue::Copy);
    device->waitForIdle();

    // Map the staging buffer to read the data
    void* mappedData =
        device->mapBuffer(stagingBuffer, nvrhi::CpuAccessMode::Read);
    if (mappedData) {
        memcpy(data, mappedData, size);
        device->unmapBuffer(stagingBuffer);
    }
}

template<typename T>
void DeviceMemoryPool<T>::Initialize()
{
    nvrhi::IDevice* device = RHI::get_device();
    nvrhi::CommandListParameters cmd_desc;
    cmd_desc.enableImmediateExecution = false;
    // NOTE (2026-09-20): keep this on the Copy queue. Moving uploads to the
    // Graphics queue to fix the cross-queue read race (see
    // test_raster_mutation_consistency) instead triggers device removal:
    // consumers barrier the buffers to ShaderResource on the graphics
    // queue, and unlike the copy queue there is no implicit decay back to
    // COMMON, so a subsequent writeBuffer (needs COPY_DEST) is invalid.
    // The proper fix is an upload fence in the barrier-tracking layer.
    cmd_desc.queueType = nvrhi::CommandQueue::Copy;
    commandList = device->createCommandList(cmd_desc);

    // Initialize device buffer and valid mask
    nvrhi::BufferDesc bufferDesc = buffer_desc<T>();
    bufferDesc.debugName =
        "DeviceObjectPoolBuffer " + std::string(typeid(T).name());
    device_buffer = device->createBuffer(bufferDesc);
}

template<typename T>
DeviceMemoryPool<T>::DeviceMemoryPool()
{
    Initialize();
}

template<typename T>
std::vector<T> DeviceMemoryPool<T>::get_content() const
{
    std::vector<T> content;
    std::lock_guard<std::mutex> lock(execution_launch_mutex);

    // Create a staging buffer to read data from the device
    nvrhi::BufferDesc stagingDesc;
    stagingDesc.byteSize = current_max_memory_offset;
    stagingDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
    stagingDesc.debugName = "ContentReadbackBuffer";

    nvrhi::IDevice* device = RHI::get_device();
    nvrhi::BufferHandle stagingBuffer = device->createBuffer(stagingDesc);

    // Copy data from device buffer to staging buffer
    commandList->open();
    commandList->copyBuffer(
        stagingBuffer, 0, device_buffer, 0, current_max_memory_offset);
    commandList->close();

    // Execute and wait for completion
    device->executeCommandList(commandList, nvrhi::CommandQueue::Copy);
    device->waitForIdle();

    // Map the buffer to read data
    void* mappedData =
        device->mapBuffer(stagingBuffer, nvrhi::CpuAccessMode::Read);
    if (mappedData) {
        content.resize(current_max_memory_offset / sizeof(T));
        memcpy(content.data(), mappedData, current_max_memory_offset);
        device->unmapBuffer(stagingBuffer);
    }

    return content;
}

template<typename T>
DeviceMemoryPool<T>::DeviceMemoryPool(const nvrhi::BufferDesc& buffer_desc)
    : base_desc_(buffer_desc)
{
    Initialize();
}

template<typename T>
void DeviceMemoryPool<T>::destroy()
{
    // Detach first so handles dying later skip erase() instead of touching
    // the mutex and lists being torn down here.
    alive_token_->store(false, std::memory_order_release);
    clear();
    commandList = nullptr;
    device_buffer = nullptr;
}

template<typename T>
DeviceMemoryPool<T>::~DeviceMemoryPool()
{
    destroy();
}

template<typename T>
typename DeviceMemoryPool<T>::MemoryHandle DeviceMemoryPool<T>::allocate(
    size_t count)
{
    MemoryHandle handle = MemoryHandleData::create();

    auto size = count * sizeof(T);
    handle->size = size;
    handle->pool = this;
    handle->pool_alive = alive_token_;

    std::lock_guard lock(buffer_write_mutex_);

    for (auto free_handle = h_free_list.begin();
         free_handle != h_free_list.end();
         ++free_handle) {
        if (std::get<1>(*free_handle) >= size) {
            handle->offset = std::get<0>(*free_handle);

            if (std::get<1>(*free_handle) > size) {
                std::get<0>(*free_handle) += size;
                std::get<1>(*free_handle) -= size;
            }
            else {
                h_free_list.erase(free_handle);
            }
            break;
        }
    }

    if (handle->offset == MemoryHandleData::INVALID) {
        handle->offset = current_max_memory_offset;
        current_max_memory_offset += size;
    }

    handles_allocated.push_back(handle.get());
    current_count += count;

    while (targeted_max_count < current_max_memory_offset / sizeof(T)) {
        targeted_max_count *= 2;
    }

    relocate_buffer();

    return handle;
}

template<typename T>
void DeviceMemoryPool<T>::erase(MemoryHandleData* handle)
{
    std::lock_guard lock(buffer_write_mutex_);

    h_free_list.push_back(std::make_pair(handle->offset, handle->size));
    deferred_rebuild_ = true;
    current_count -= handle->size / sizeof(T);
    handles_allocated.erase(
        std::remove(handles_allocated.begin(), handles_allocated.end(), handle),
        handles_allocated.end());
}

template<typename T>
void DeviceMemoryPool<T>::adopt(MemoryHandleData* handle_from_another_pool)
{
    auto old_pool = handle_from_another_pool->pool;

    old_pool->commandList->copyBuffer(  // Copy data from another pool
        device_buffer,
        current_max_memory_offset,
        handle_from_another_pool->pool->get_device_buffer(),
        handle_from_another_pool->offset,
        handle_from_another_pool->size);

    current_count += handle_from_another_pool->size / sizeof(T);
    handles_allocated.push_back(handle_from_another_pool);
    handle_from_another_pool->offset = current_max_memory_offset;
    current_max_memory_offset += handle_from_another_pool->size;
}

template<typename T>
template<typename U>
nvrhi::BufferDesc DeviceMemoryPool<T>::buffer_desc() const
{
    nvrhi::BufferDesc desc = this->base_desc_;
    desc.structStride = sizeof(U);
    desc.byteSize = max_count * sizeof(U);
    desc.setCanHaveUAVs(true);
    desc.keepInitialState = true;
    return desc;
}

template<typename T>
void DeviceMemoryPool<T>::clear()
{
    h_free_list.clear();
    current_count = 0;
    current_max_memory_offset = 0;
}

template<typename T>
bool DeviceMemoryPool<T>::compress()
{
    std::lock_guard lock(buffer_write_mutex_);

    if (h_free_list.empty()) {
        return false;
    }

    // Create another DeviceMemoryPool and copy into it...
    DeviceMemoryPool<T> new_pool(this->base_desc_);
    new_pool.reserve(max_count);

    {
        std::lock_guard launch_lock(execution_launch_mutex);

        commandList->open();
        for (auto handle : handles_allocated) {
            new_pool.adopt(handle);
        }
        commandList->close();

        auto device = RHI::get_device();
        device->executeCommandList(commandList, nvrhi::CommandQueue::Copy);
        device->waitForIdle();
        device->runGarbageCollection();
    }

    // Swap members in place instead of move-assigning: object identity is
    // preserved, so every live handle's `pool` back-pointer stays valid.
    std::swap(device_buffer, new_pool.device_buffer);
    std::swap(h_free_list, new_pool.h_free_list);
    std::swap(max_count, new_pool.max_count);
    std::swap(targeted_max_count, new_pool.targeted_max_count);
    std::swap(current_count, new_pool.current_count);
    std::swap(current_max_memory_offset, new_pool.current_max_memory_offset);
    std::swap(handles_allocated, new_pool.handles_allocated);
    std::swap(base_desc_, new_pool.base_desc_);

    // new_pool is left holding the old fragmented buffer and releases it.
    return true;
}

template<typename T>
std::string DeviceMemoryPool<T>::info(bool free_list) const
{
    std::stringstream ss;

    ss << "[size]: " << current_count << std::endl;
    ss << "[max size]: " << max_count << std::endl;
    ss << "[max memory offset]: " << current_max_memory_offset << std::endl;

    if (free_list) {
        ss << "[Free list]: " << std::endl;
        for (int i = 0; i < h_free_list.size(); ++i) {
            ss << "  " << i << ": " << h_free_list[i].first / sizeof(T) << " - "
               << (h_free_list[i].first + h_free_list[i].second) / sizeof(T)
               << std::endl;
        }
    }
    return ss.str();
}

template<typename T>
bool DeviceMemoryPool<T>::sanitize()
{
    size_t current_offset = 0;
    std::sort(
        handles_allocated.begin(),
        handles_allocated.end(),
        [](MemoryHandleData* a, MemoryHandleData* b) {
            return a->offset < b->offset;
        });

    for (auto handle : handles_allocated) {
        if (handle->offset != current_offset) {
            return false;
        }
        current_offset += handle->size;
    }
    return true;
}

template<typename T>
bool DeviceMemoryPool<T>::take_pending_changes()
{
    std::lock_guard lock(buffer_write_mutex_);
    bool pending = deferred_rebuild_;
    deferred_rebuild_ = false;
    return pending;
}

template<typename T>
std::vector<T> DeviceMemoryPool<T>::gather_active()
{
    std::lock_guard lock(buffer_write_mutex_);
    if (handles_allocated.empty() || current_max_memory_offset == 0) {
        return {};
    }

    nvrhi::IDevice* device = RHI::get_device();
    nvrhi::BufferDesc staging_desc;
    staging_desc.byteSize = current_max_memory_offset;
    staging_desc.cpuAccess = nvrhi::CpuAccessMode::Read;
    staging_desc.debugName = "PoolGatherReadback";
    nvrhi::BufferHandle staging = device->createBuffer(staging_desc);

    {
        // Take the launch lock for the whole readback (same nesting order
        // as compress: buffer_write_mutex_ -> execution_launch_mutex).
        std::lock_guard launch_lock(execution_launch_mutex);
        commandList->open();
        commandList->copyBuffer(
            staging, 0, device_buffer, 0, current_max_memory_offset);
        commandList->close();
        device->executeCommandList(commandList, nvrhi::CommandQueue::Copy);
    }
    device->waitForIdle();

    std::vector<T> out;
    out.reserve(handles_allocated.size());
    void* mapped = device->mapBuffer(staging, nvrhi::CpuAccessMode::Read);
    if (mapped) {
        const T* src = static_cast<const T*>(mapped);
        for (const MemoryHandleData* handle : handles_allocated) {
            out.push_back(src[handle->offset / sizeof(T)]);
        }
        device->unmapBuffer(staging);
    }
    return out;
}

template<typename T>
nvrhi::IBuffer* DeviceMemoryPool<T>::get_device_buffer() const
{
    return device_buffer;
}

template<typename T>
size_t DeviceMemoryPool<T>::max_memory_offset() const
{
    return current_max_memory_offset;
}

template<typename T>
size_t DeviceMemoryPool<T>::pool_size() const
{
    return max_count;
}

template<typename T>
size_t DeviceMemoryPool<T>::count() const
{
    return current_count;
}

template<typename T>
void DeviceMemoryPool<T>::relocate_buffer()
{
    if (max_count != targeted_max_count) {
        nvrhi::BufferDesc bufferDesc = buffer_desc<T>();
        bufferDesc.byteSize = targeted_max_count * sizeof(T);
        bufferDesc.debugName = "DeviceObjectPoolBuffer";
        auto new_device_buffer = RHI::get_device()->createBuffer(bufferDesc);

        std::lock_guard lock(execution_launch_mutex);

        commandList->open();
        commandList->copyBuffer(
            new_device_buffer, 0, device_buffer, 0, max_count * sizeof(T));

        commandList->close();
        RHI::get_device()->executeCommandList(
            commandList, nvrhi::CommandQueue::Copy);
        // RHI::get_device()->waitForIdle();
        // RHI::get_device()->runGarbageCollection();

        max_count = targeted_max_count;

        device_buffer = new_device_buffer;
    }
}

RUZINO_NAMESPACE_CLOSE_SCOPE
