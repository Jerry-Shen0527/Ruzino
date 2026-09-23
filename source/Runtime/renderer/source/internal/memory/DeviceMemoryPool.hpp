#pragma once
#include <spdlog/spdlog.h>

#include <RHI/rhi.hpp>
#include <algorithm>
#include <memory>
#include <mutex>
#include <sstream>

#include "../../api.h"
RUZINO_NAMESPACE_OPEN_SCOPE

HD_RUZINO_API extern std::mutex execution_launch_mutex;

/// Lifetime note (reworked 2026-09-23): all CPU bookkeeping (free list,
/// counters, handle registry) lives in a shared State block that every
/// MemoryHandleData also references. Handle teardown therefore locks
/// memory that is guaranteed alive even when the handle outlives the pool
/// or is destroyed concurrently with it; the pool marks the state dead
/// under the same mutex in destroy(), and late handle use degrades to a
/// warned no-op instead of a use-after-free. This replaces the old
/// pool_alive atomic token, which only narrowed (did not close) the
/// check-then-use race, and left write_data/read_data completely
/// unguarded.
template<typename T>
class DeviceMemoryPool {
    struct State;  // defined below; handles reference it

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

        // Shared with the pool: keeps the bookkeeping alive past the
        // pool's own destruction. Null only for handles that were created
        // but never passed through allocate().
        std::shared_ptr<State> state;

        nvrhi::BindingSetItem get_descriptor(
            nvrhi::ResourceType type =
                nvrhi::ResourceType::StructuredBuffer_UAV) const;

        nvrhi::IBuffer* get_device_buffer() const;
        void read_data(void* data);

       private:
        static constexpr size_t INVALID = -1;
        DeviceMemoryPool* pool = nullptr;
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
    /// data and invalidating every stored slot references.
    std::vector<T> gather_active();

    /// True once, then resets: entries were freed since the last call.
    /// Replaces the "did compress() do work" signal that rebuild_tlas used
    /// to poll for — same wake-up semantics without any data movement.
    bool take_pending_changes();

   private:
    // Everything a handle's destructor (and the handle upload paths) must
    // touch. Lives behind a shared_ptr held by both the pool and every
    // handle, which is what makes handle teardown race-free against
    // ~DeviceMemoryPool.
    struct State {
        std::mutex mutex;

        std::vector<std::pair<size_t, size_t>> h_free_list;  // offset, size
        size_t max_count = 1;
        size_t targeted_max_count = 1;
        bool deferred_rebuild = false;  // set by erase, drained by
                                        // take_pending_changes()
        size_t current_count = 0;
        size_t current_max_memory_offset = 0;
        std::vector<MemoryHandleData*> handles_allocated;
        bool pool_dead = false;  // set under mutex by destroy()
    };

    std::shared_ptr<State> state_ = std::make_shared<State>();

    // Free-list bookkeeping. Caller holds st.mutex.
    static void erase_locked(State& st, MemoryHandleData* handle);

    // Caller holds state_->mutex (the GPU members it touches are
    // themselves only mutated under that lock).
    void relocate_buffer();

    // Assume the command list (of the other one) is already opened
    // No locks: the destination pool is a thread-local object under
    // construction inside compress(), and the source pool's State lock is
    // held by the caller.
    void adopt(MemoryHandleData* handle_from_another_pool);

    nvrhi::BufferHandle device_buffer;
    nvrhi::CommandListHandle commandList;

    // Utility functions

    template<typename U>
    nvrhi::BufferDesc buffer_desc() const;

    nvrhi::BufferDesc base_desc_;
};

template<typename T>
void DeviceMemoryPool<T>::reserve(size_t size)
{
    std::lock_guard lock(state_->mutex);
    while (size > state_->targeted_max_count) {
        state_->targeted_max_count *= 2;
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
    if (!state) {
        spdlog::warn("DeviceMemoryPool: write_data on unallocated handle");
        return;
    }
    // Hold the state lock while touching the pool's GPU members so a
    // concurrent destroy() (which marks pool_dead under this mutex before
    // releasing them) cannot pull them out mid-upload.
    std::lock_guard lock(state->mutex);
    if (state->pool_dead || !pool) {
        spdlog::warn(
            "DeviceMemoryPool: write_data after pool destruction, skipping");
        return;
    }

    std::lock_guard launch_lock(execution_launch_mutex);

    pool->commandList->open();
    pool->commandList->writeBuffer(pool->device_buffer, data, size, offset);
    pool->commandList->close();

    RHI::get_device()->executeCommandList(
        pool->commandList, nvrhi::CommandQueue::Copy);
}

template<typename T>
void DeviceMemoryPool<T>::MemoryHandleData::write_data(
    const void* data,
    size_t bias_count)
{
    if (!state) {
        spdlog::warn("DeviceMemoryPool: write_data on unallocated handle");
        return;
    }
    std::lock_guard lock(state->mutex);
    if (state->pool_dead || !pool) {
        spdlog::warn(
            "DeviceMemoryPool: write_data after pool destruction, skipping");
        return;
    }

    std::lock_guard launch_lock(execution_launch_mutex);

    pool->commandList->open();
    pool->commandList->writeBuffer(
        pool->device_buffer, data, sizeof(T), offset + bias_count * sizeof(T));
    pool->commandList->close();

    RHI::get_device()->executeCommandList(
        pool->commandList, nvrhi::CommandQueue::Copy);
}

template<typename T>
DeviceMemoryPool<T>::MemoryHandleData::~MemoryHandleData()
{
    if (!state)
        return;
    // The state block is shared_ptr-kept alive by this very handle, so
    // locking it can never race the pool's destruction: destroy() marks
    // pool_dead under the same mutex before tearing anything down.
    std::lock_guard lock(state->mutex);
    if (state->pool_dead) {
        spdlog::warn(
            "DeviceMemoryPool: handle outlived its pool, "
            "skipping free-list bookkeeping");
        return;
    }
    DeviceMemoryPool<T>::erase_locked(*state, this);
}

template<typename T>
nvrhi::BindingSetItem DeviceMemoryPool<T>::MemoryHandleData::get_descriptor(
    nvrhi::ResourceType type) const
{
    nvrhi::BindingSetItem item;
    std::lock_guard lock(state->mutex);
    if (state->pool_dead || !pool) {
        item.resourceHandle = nullptr;
    }
    else {
        item.resourceHandle = pool->device_buffer;
    }
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
    std::lock_guard lock(state->mutex);
    if (state->pool_dead || !pool)
        return nullptr;
    return pool->device_buffer;
}

template<typename T>
void DeviceMemoryPool<T>::MemoryHandleData::read_data(void* data)
{
    if (!state)
        return;
    std::lock_guard lock(state->mutex);
    if (state->pool_dead || !pool) {
        spdlog::warn(
            "DeviceMemoryPool: read_data after pool destruction, skipping");
        return;
    }

    std::lock_guard launch_lock(execution_launch_mutex);

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
    std::lock_guard lock(state_->mutex);

    if (state_->current_max_memory_offset == 0) {
        return content;
    }

    std::lock_guard launch_lock(execution_launch_mutex);

    // Create a staging buffer to read data from the device
    nvrhi::BufferDesc stagingDesc;
    stagingDesc.byteSize = state_->current_max_memory_offset;
    stagingDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
    stagingDesc.debugName = "ContentReadbackBuffer";

    nvrhi::IDevice* device = RHI::get_device();
    nvrhi::BufferHandle stagingBuffer = device->createBuffer(stagingDesc);

    // Copy data from device buffer to staging buffer
    commandList->open();
    commandList->copyBuffer(
        stagingBuffer, 0, device_buffer, 0, state_->current_max_memory_offset);
    commandList->close();

    // Execute and wait for completion
    device->executeCommandList(commandList, nvrhi::CommandQueue::Copy);
    device->waitForIdle();

    // Map the buffer to read data
    void* mappedData =
        device->mapBuffer(stagingBuffer, nvrhi::CpuAccessMode::Read);
    if (mappedData) {
        content.resize(state_->current_max_memory_offset / sizeof(T));
        memcpy(content.data(), mappedData, state_->current_max_memory_offset);
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
    // Mark the shared state dead under its mutex: handles dying later (or
    // racing this destructor) serialize here and skip bookkeeping, and no
    // upload can be mid-flight past this point because write_data holds
    // the same mutex while using the GPU members released below.
    {
        std::lock_guard lock(state_->mutex);
        state_->pool_dead = true;
    }
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
    handle->state = state_;

    std::lock_guard lock(state_->mutex);
    State& st = *state_;

    for (auto free_handle = st.h_free_list.begin();
         free_handle != st.h_free_list.end();
         ++free_handle) {
        if (std::get<1>(*free_handle) >= size) {
            handle->offset = std::get<0>(*free_handle);

            if (std::get<1>(*free_handle) > size) {
                std::get<0>(*free_handle) += size;
                std::get<1>(*free_handle) -= size;
            }
            else {
                st.h_free_list.erase(free_handle);
            }
            break;
        }
    }

    if (handle->offset == MemoryHandleData::INVALID) {
        handle->offset = st.current_max_memory_offset;
        st.current_max_memory_offset += size;
    }

    st.handles_allocated.push_back(handle.get());
    st.current_count += count;

    while (st.targeted_max_count < st.current_max_memory_offset / sizeof(T)) {
        st.targeted_max_count *= 2;
    }

    relocate_buffer();

    return handle;
}

template<typename T>
void DeviceMemoryPool<T>::erase_locked(State& st, MemoryHandleData* handle)
{
    st.h_free_list.push_back(std::make_pair(handle->offset, handle->size));
    st.deferred_rebuild = true;
    st.current_count -= handle->size / sizeof(T);
    st.handles_allocated.erase(
        std::remove(
            st.handles_allocated.begin(), st.handles_allocated.end(), handle),
        st.handles_allocated.end());
}

template<typename T>
void DeviceMemoryPool<T>::adopt(MemoryHandleData* handle_from_another_pool)
{
    // See declaration: no locks here by contract.
    auto old_pool = handle_from_another_pool->pool;
    State& st = *state_;

    old_pool->commandList->copyBuffer(  // Copy data from another pool
        device_buffer,
        st.current_max_memory_offset,
        handle_from_another_pool->pool->get_device_buffer(),
        handle_from_another_pool->offset,
        handle_from_another_pool->size);

    st.current_count += handle_from_another_pool->size / sizeof(T);
    st.handles_allocated.push_back(handle_from_another_pool);
    handle_from_another_pool->offset = st.current_max_memory_offset;
    st.current_max_memory_offset += handle_from_another_pool->size;
}

template<typename T>
template<typename U>
nvrhi::BufferDesc DeviceMemoryPool<T>::buffer_desc() const
{
    nvrhi::BufferDesc desc = this->base_desc_;
    desc.structStride = sizeof(U);
    desc.byteSize = state_->max_count * sizeof(U);
    desc.setCanHaveUAVs(true);
    desc.keepInitialState = true;
    return desc;
}

template<typename T>
void DeviceMemoryPool<T>::clear()
{
    std::lock_guard lock(state_->mutex);
    state_->h_free_list.clear();
    state_->current_count = 0;
    state_->current_max_memory_offset = 0;
}

template<typename T>
bool DeviceMemoryPool<T>::compress()
{
    std::lock_guard lock(state_->mutex);
    State& st = *state_;

    if (st.h_free_list.empty()) {
        return false;
    }

    // Create another DeviceMemoryPool and copy into it...
    DeviceMemoryPool<T> new_pool(this->base_desc_);
    new_pool.reserve(st.max_count);

    {
        std::lock_guard launch_lock(execution_launch_mutex);

        commandList->open();
        for (auto handle : st.handles_allocated) {
            new_pool.adopt(handle);
        }
        commandList->close();

        auto device = RHI::get_device();
        device->executeCommandList(commandList, nvrhi::CommandQueue::Copy);
        device->waitForIdle();
        device->runGarbageCollection();
    }

    // Swap the STATE CONTENTS in place instead of move-assigning: the
    // State object's identity is what every live handle references (and
    // what their shared_ptrs keep alive), so it must not be replaced.
    // Object identity of the pool itself is likewise preserved, so every
    // live handle's `pool` back-pointer stays valid.
    State& nst = *new_pool.state_;
    std::swap(st.h_free_list, nst.h_free_list);
    std::swap(st.max_count, nst.max_count);
    std::swap(st.targeted_max_count, nst.targeted_max_count);
    std::swap(st.current_count, nst.current_count);
    std::swap(st.current_max_memory_offset, nst.current_max_memory_offset);
    std::swap(st.handles_allocated, nst.handles_allocated);

    std::swap(device_buffer, new_pool.device_buffer);

    // new_pool is left holding the old fragmented buffer and releases it.
    return true;
}

template<typename T>
std::string DeviceMemoryPool<T>::info(bool free_list) const
{
    std::lock_guard lock(state_->mutex);
    const State& st = *state_;

    std::stringstream ss;

    ss << "[size]: " << st.current_count << std::endl;
    ss << "[max size]: " << st.max_count << std::endl;
    ss << "[max memory offset]: " << st.current_max_memory_offset << std::endl;

    if (free_list) {
        ss << "[Free list]: " << std::endl;
        for (int i = 0; i < st.h_free_list.size(); ++i) {
            ss << "  " << i << ": " << st.h_free_list[i].first / sizeof(T)
               << " - "
               << (st.h_free_list[i].first + st.h_free_list[i].second) /
                      sizeof(T)
               << std::endl;
        }
    }
    return ss.str();
}

template<typename T>
bool DeviceMemoryPool<T>::sanitize()
{
    std::lock_guard lock(state_->mutex);
    State& st = *state_;

    size_t current_offset = 0;
    std::sort(
        st.handles_allocated.begin(),
        st.handles_allocated.end(),
        [](MemoryHandleData* a, MemoryHandleData* b) {
            return a->offset < b->offset;
        });

    for (auto handle : st.handles_allocated) {
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
    std::lock_guard lock(state_->mutex);
    bool pending = state_->deferred_rebuild;
    state_->deferred_rebuild = false;
    return pending;
}

template<typename T>
std::vector<T> DeviceMemoryPool<T>::gather_active()
{
    std::lock_guard lock(state_->mutex);
    const State& st = *state_;

    if (st.handles_allocated.empty() || st.current_max_memory_offset == 0) {
        return {};
    }

    nvrhi::IDevice* device = RHI::get_device();
    nvrhi::BufferDesc staging_desc;
    staging_desc.byteSize = st.current_max_memory_offset;
    staging_desc.cpuAccess = nvrhi::CpuAccessMode::Read;
    staging_desc.debugName = "PoolGatherReadback";
    nvrhi::BufferHandle staging = device->createBuffer(staging_desc);

    {
        // Take the launch lock for the whole readback (same nesting order
        // as everywhere: State::mutex -> execution_launch_mutex).
        std::lock_guard launch_lock(execution_launch_mutex);
        commandList->open();
        commandList->copyBuffer(
            staging, 0, device_buffer, 0, st.current_max_memory_offset);
        commandList->close();
        device->executeCommandList(commandList, nvrhi::CommandQueue::Copy);
    }
    device->waitForIdle();

    std::vector<T> out;
    out.reserve(st.handles_allocated.size());
    void* mapped = device->mapBuffer(staging, nvrhi::CpuAccessMode::Read);
    if (mapped) {
        const T* src = static_cast<const T*>(mapped);
        for (const MemoryHandleData* handle : st.handles_allocated) {
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
    std::lock_guard lock(state_->mutex);
    return state_->current_max_memory_offset;
}

template<typename T>
size_t DeviceMemoryPool<T>::pool_size() const
{
    std::lock_guard lock(state_->mutex);
    return state_->max_count;
}

template<typename T>
size_t DeviceMemoryPool<T>::count() const
{
    std::lock_guard lock(state_->mutex);
    return state_->current_count;
}

template<typename T>
void DeviceMemoryPool<T>::relocate_buffer()
{
    if (state_->max_count != state_->targeted_max_count) {
        nvrhi::BufferDesc bufferDesc = buffer_desc<T>();
        bufferDesc.byteSize = state_->targeted_max_count * sizeof(T);
        bufferDesc.debugName = "DeviceObjectPoolBuffer";
        auto new_device_buffer = RHI::get_device()->createBuffer(bufferDesc);

        std::lock_guard launch_lock(execution_launch_mutex);

        commandList->open();
        commandList->copyBuffer(
            new_device_buffer,
            0,
            device_buffer,
            0,
            state_->max_count * sizeof(T));

        commandList->close();
        RHI::get_device()->executeCommandList(
            commandList, nvrhi::CommandQueue::Copy);
        // RHI::get_device()->waitForIdle();
        // RHI::get_device()->runGarbageCollection();

        state_->max_count = state_->targeted_max_count;

        device_buffer = new_device_buffer;
    }
}

RUZINO_NAMESPACE_CLOSE_SCOPE
