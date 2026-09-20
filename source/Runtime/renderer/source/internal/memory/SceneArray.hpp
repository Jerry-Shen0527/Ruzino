#pragma once
#include <spdlog/spdlog.h>

#include <RHI/rhi.hpp>
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "../../api.h"
RUZINO_NAMESPACE_OPEN_SCOPE

HD_RUZINO_API extern std::mutex execution_launch_mutex;

/// SceneArray<T>: CPU-authoritative scene table with a GPU mirror.
///
/// Design (SceneStore rework, 2026-09-20):
///   - CPU side is the single source of truth. Data lives in STABLE SLOTS
///     that are never moved; the dense iteration order is a separate index
///     list. Removing an entry is swap-remove on the index list only.
///   - GPU side is a pure mirror, refreshed by flush() on the copy queue.
///     No GPU-side free lists, no compaction, no buffer-identity churn
///     beyond 2^N capacity growth.
///   - Handles are RAII: destroying the last reference removes the entry.
///     handle->slot is a PERMANENT index for the entry's lifetime, so other
///     GPU data may store it as a reference (e.g. rect lights store
///     GeometryInstanceData.geometryID = slot; ClosestHit reads
///     lightBuffer[slot] directly).
///
/// Two output layouts, selected by kDenseOutput:
///   - false (default, "referenced" tables like lights): GPU gets TWO
///     buffers — the slot-layout data (slot ids are stable references) and
///     a dense uint index list. Dense readers iterate
///         for (i < count()) data = slots[indices[i]]
///     which is always hole-free without any compaction step.
///   - true ("pure leaf" tables like draw-indirect args): GPU gets ONE
///     buffer holding the data re-packed densely at flush time. Only valid
///     when nothing stores references to entry slots (the entries are
///     self-contained). Consumers requiring a contiguous array (e.g.
///     ExecuteIndirect) read it directly from offset 0.
///
/// Thread model: add/update/remove are called from parallel Hydra sync
/// workers and are serialized by an internal mutex. flush() is called from
/// the render thread before binding. The upload takes the global
/// execution_launch_mutex, matching DeviceMemoryPool::write_data.
template<typename T, bool kDenseOutput = false>
class SceneArray {
   public:
    struct HandleData {
       private:
        friend class SceneArray;
        uint32_t slot_ = INVALID_SLOT;
        uint32_t generation_ = 0;
        SceneArray* pool_ = nullptr;
        // Handed out by the pool; cleared when the pool is destroyed so a
        // handle outliving its pool skips bookkeeping instead of dangling.
        std::shared_ptr<std::atomic<bool>> pool_alive_;

       public:
        uint32_t slot() const
        {
            return slot_;
        }

        ~HandleData();
    };
    using Handle = std::shared_ptr<HandleData>;

    /// base_desc carries extra buffer flags for the GPU mirror (e.g.
    /// setIsDrawIndirectArgs(true) for the draw-args table). Stride and
    /// size are managed by the table itself.
    explicit SceneArray(const nvrhi::BufferDesc& base_desc = {})
        : base_desc_(base_desc)
    {
    }
    ~SceneArray()
    {
        // Detach handles first so any handle dying later skips bookkeeping
        // instead of touching the mutex and vectors being torn down here.
        alive_token_->store(false, std::memory_order_release);
    }
    SceneArray(const SceneArray&) = delete;
    SceneArray& operator=(const SceneArray&) = delete;

    /// Add a new entry; returns an owning handle. Removing happens when the
    /// handle's last copy dies.
    Handle add(const T& data);

    /// Overwrite an entry. Returns false (and logs) if the handle is stale.
    bool update(const Handle& handle, const T& data);

    uint32_t count() const;
    bool dirty() const;
    uint64_t version() const;

    /// Upload modified CPU state to the GPU mirrors. Safe to call when
    /// clean (no-op). Call from the render thread before binding.
    void flush();

    /// Slot-layout mirror of all entries. Indexed by handle->slot().
    /// (kDenseOutput = false only.)
    nvrhi::IBuffer* get_slots_buffer() const;
    /// Dense list of active slot ids. Iterate [0..count()) via
    /// slots[indices[i]]. Never contains holes. (kDenseOutput = false only.)
    nvrhi::IBuffer* get_indices_buffer() const;
    /// Single dense data buffer (entries re-packed at flush time).
    /// (kDenseOutput = true only.) Consumers read [0..count()) directly.
    nvrhi::IBuffer* get_dense_buffer() const;

    nvrhi::BindingSetItem get_slots_descriptor(
        nvrhi::ResourceType type =
            nvrhi::ResourceType::StructuredBuffer_SRV) const;
    nvrhi::BindingSetItem get_indices_descriptor(
        nvrhi::ResourceType type =
            nvrhi::ResourceType::StructuredBuffer_SRV) const;

    /// Debug: read back the dense view exactly as a shader loop would see
    /// it (slots[indices[i]], i < count()). Lazily flushes if needed.
    /// Staging + map is a debug path; never call this per frame.
    std::vector<T> get_dense_content();

   private:
    static constexpr uint32_t INVALID_SLOT = 0xFFFFFFFFu;

    void remove(HandleData* handle);
    void ensure_gpu_capacity_locked(size_t needed_slots);
    void upload_locked();

    mutable std::mutex mutex_;

    // CPU truth. slots_ keeps stale data in freed slots (never read through
    // the dense path); indices_ is always hole-free.
    std::vector<T> slots_;
    std::vector<uint32_t> generations_;
    std::vector<uint32_t> dense_positions_;  // slot -> index into indices_
    std::vector<uint32_t> free_slots_;
    std::vector<uint32_t> indices_;  // dense rows: slot ids

    bool dirty_ = false;
    uint64_t version_ = 0;

    // GPU mirrors (2^N capacity, grow-only, rebuilt whole on flush).
    nvrhi::BufferHandle slots_buffer_;    // kDenseOutput = false
    nvrhi::BufferHandle indices_buffer_;  // kDenseOutput = false
    nvrhi::BufferHandle dense_buffer_;    // kDenseOutput = true
    std::vector<T> dense_scratch_;        // kDenseOutput repack scratch
    size_t gpu_capacity_ = 0;
    nvrhi::BufferDesc base_desc_;
    // mutable: lazily created by const debug helpers (get_dense_content)
    mutable nvrhi::CommandListHandle command_list_;
    std::shared_ptr<std::atomic<bool>> alive_token_ =
        std::make_shared<std::atomic<bool>>(true);
};

template<typename T, bool kDenseOutput>
typename SceneArray<T, kDenseOutput>::Handle SceneArray<T, kDenseOutput>::add(
    const T& data)
{
    std::lock_guard lock(mutex_);

    uint32_t slot;
    if (!free_slots_.empty()) {
        slot = free_slots_.back();
        free_slots_.pop_back();
    }
    else {
        slot = static_cast<uint32_t>(slots_.size());
        slots_.emplace_back();
        generations_.push_back(0);
        dense_positions_.push_back(0);
    }

    slots_[slot] = data;
    dense_positions_[slot] = static_cast<uint32_t>(indices_.size());
    indices_.push_back(slot);

    auto handle = std::make_shared<HandleData>();
    handle->slot_ = slot;
    handle->generation_ = generations_[slot];
    handle->pool_ = this;
    handle->pool_alive_ = alive_token_;

    dirty_ = true;
    return handle;
}

template<typename T, bool kDenseOutput>
bool SceneArray<T, kDenseOutput>::update(const Handle& handle, const T& data)
{
    if (!handle)
        return false;
    std::lock_guard lock(mutex_);
    if (handle->slot_ >= slots_.size() ||
        handle->generation_ != generations_[handle->slot_]) {
        spdlog::warn(
            "SceneArray::update: stale handle (slot {}, generation {} vs {})",
            handle->slot_,
            handle->generation_,
            handle->slot_ < generations_.size() ? generations_[handle->slot_]
                                                : 0xFFFFFFFFu);
        return false;
    }
    slots_[handle->slot_] = data;
    dirty_ = true;
    return true;
}

template<typename T, bool kDenseOutput>
uint32_t SceneArray<T, kDenseOutput>::count() const
{
    std::lock_guard lock(mutex_);
    return static_cast<uint32_t>(indices_.size());
}

template<typename T, bool kDenseOutput>
bool SceneArray<T, kDenseOutput>::dirty() const
{
    std::lock_guard lock(mutex_);
    return dirty_;
}

template<typename T, bool kDenseOutput>
uint64_t SceneArray<T, kDenseOutput>::version() const
{
    std::lock_guard lock(mutex_);
    return version_;
}

template<typename T, bool kDenseOutput>
void SceneArray<T, kDenseOutput>::remove(HandleData* handle)
{
    std::lock_guard lock(mutex_);
    const uint32_t slot = handle->slot_;
    if (slot >= slots_.size() || handle->generation_ != generations_[slot]) {
        return;  // stale handle (slot already recycled) — nothing to do
    }

    const uint32_t dense_pos = dense_positions_[slot];
    const uint32_t moved_slot = indices_.back();
    indices_[dense_pos] = moved_slot;
    dense_positions_[moved_slot] = dense_pos;
    indices_.pop_back();

    generations_[slot]++;  // invalidate every outstanding handle to this slot
    free_slots_.push_back(slot);
    dirty_ = true;
}

template<typename T, bool kDenseOutput>
void SceneArray<T, kDenseOutput>::ensure_gpu_capacity_locked(
    size_t needed_slots)
{
    if (needed_slots <= gpu_capacity_)
        return;

    size_t capacity = std::max<size_t>(gpu_capacity_, 16);
    while (capacity < needed_slots) {
        capacity *= 2;
    }

    auto device = RHI::get_device();

    if constexpr (kDenseOutput) {
        nvrhi::BufferDesc desc = base_desc_;
        desc.structStride = sizeof(T);
        desc.byteSize = capacity * sizeof(T);
        desc.setCanHaveUAVs(true);
        desc.keepInitialState = true;
        desc.debugName = "SceneArray_dense";
        dense_buffer_ = device->createBuffer(desc);
    }
    else {
        nvrhi::BufferDesc slots_desc = base_desc_;
        slots_desc.structStride = sizeof(T);
        slots_desc.byteSize = capacity * sizeof(T);
        slots_desc.setCanHaveUAVs(true);
        slots_desc.keepInitialState = true;
        slots_desc.debugName = "SceneArray_slots";
        slots_buffer_ = device->createBuffer(slots_desc);

        nvrhi::BufferDesc indices_desc;
        indices_desc.structStride = sizeof(uint32_t);
        indices_desc.byteSize = capacity * sizeof(uint32_t);
        indices_desc.setCanHaveUAVs(true);
        indices_desc.keepInitialState = true;
        indices_desc.debugName = "SceneArray_indices";
        indices_buffer_ = device->createBuffer(indices_desc);
    }

    gpu_capacity_ = capacity;
}

template<typename T, bool kDenseOutput>
void SceneArray<T, kDenseOutput>::upload_locked()
{
    if (!command_list_) {
        nvrhi::CommandListParameters cmd_desc;
        cmd_desc.enableImmediateExecution = false;
        cmd_desc.queueType = nvrhi::CommandQueue::Copy;
        command_list_ = RHI::get_device()->createCommandList(cmd_desc);
    }

    const size_t live_slots = slots_.size();
    ensure_gpu_capacity_locked(live_slots == 0 ? 16 : live_slots);

    // Whole-buffer upload: scene tables are small (lights: tens of KB at
    // most), so skip per-slot dirty tracking entirely.
    {
        std::lock_guard launch_lock(execution_launch_mutex);
        command_list_->open();
        if constexpr (kDenseOutput) {
            // Re-pack live entries into the contiguous output buffer.
            dense_scratch_.resize(indices_.size());
            for (uint32_t i = 0; i < indices_.size(); ++i) {
                dense_scratch_[i] = slots_[indices_[i]];
            }
            if (!dense_scratch_.empty()) {
                command_list_->writeBuffer(
                    dense_buffer_,
                    dense_scratch_.data(),
                    dense_scratch_.size() * sizeof(T),
                    0);
            }
        }
        else {
            if (live_slots > 0) {
                command_list_->writeBuffer(
                    slots_buffer_, slots_.data(), live_slots * sizeof(T), 0);
            }
            command_list_->writeBuffer(
                indices_buffer_,
                indices_.data(),
                indices_.size() * sizeof(uint32_t),
                0);
        }
        command_list_->close();
        RHI::get_device()->executeCommandList(
            command_list_, nvrhi::CommandQueue::Copy);
        // The Copy-queue upload must be globally visible before a graphics
        // queue consumes the mirrors in the same node execution: D3D12
        // gives us no automatic cross-queue ordering, and consuming without
        // this sync produced nondeterministic stale/zero reads (verified on
        // the rasterize path 2026-09-20; the old DeviceMemoryPool::compress
        // relied on the same waitForIdle for its copy). flush() only does
        // this when dirty, so steady-state frames pay nothing — the cost
        // equals the old compress() on scene-change frames only.
        RHI::get_device()->waitForIdle();
        RHI::get_device()->runGarbageCollection();
    }
}

template<typename T, bool kDenseOutput>
void SceneArray<T, kDenseOutput>::flush()
{
    std::lock_guard lock(mutex_);
    if (!dirty_)
        return;
    upload_locked();
    dirty_ = false;
    version_++;
}

template<typename T, bool kDenseOutput>
nvrhi::IBuffer* SceneArray<T, kDenseOutput>::get_slots_buffer() const
{
    static_assert(
        !kDenseOutput,
        "dense-output tables have no slot-layout GPU mirror; use "
        "get_dense_buffer()");
    std::lock_guard lock(mutex_);
    return slots_buffer_;
}

template<typename T, bool kDenseOutput>
nvrhi::IBuffer* SceneArray<T, kDenseOutput>::get_indices_buffer() const
{
    static_assert(
        !kDenseOutput,
        "dense-output tables have no index list; use "
        "get_dense_buffer()");
    std::lock_guard lock(mutex_);
    return indices_buffer_;
}

template<typename T, bool kDenseOutput>
nvrhi::IBuffer* SceneArray<T, kDenseOutput>::get_dense_buffer() const
{
    static_assert(
        kDenseOutput,
        "slot-layout tables are iterated via slots[indices[i]]; "
        "their data buffer is not contiguous");
    std::lock_guard lock(mutex_);
    return dense_buffer_;
}

template<typename T, bool kDenseOutput>
nvrhi::BindingSetItem SceneArray<T, kDenseOutput>::get_slots_descriptor(
    nvrhi::ResourceType type) const
{
    std::lock_guard lock(mutex_);
    nvrhi::BindingSetItem item;
    item.resourceHandle = slots_buffer_;
    item.type = type;
    return item;
}

template<typename T, bool kDenseOutput>
nvrhi::BindingSetItem SceneArray<T, kDenseOutput>::get_indices_descriptor(
    nvrhi::ResourceType type) const
{
    std::lock_guard lock(mutex_);
    nvrhi::BindingSetItem item;
    item.resourceHandle = indices_buffer_;
    item.type = type;
    return item;
}

template<typename T, bool kDenseOutput>
std::vector<T> SceneArray<T, kDenseOutput>::get_dense_content()
{
    // Read back the GPU mirror the way a shader loop sees it.
    std::lock_guard lock(mutex_);
    if (dirty_ || !slots_buffer_ || !indices_buffer_) {
        upload_locked();
        dirty_ = false;
        version_++;
    }
    std::vector<T> result;
    result.reserve(indices_.size());

    auto device = RHI::get_device();
    nvrhi::BufferDesc staging_desc;
    staging_desc.byteSize = std::max<size_t>(indices_.size(), 1) * sizeof(T);
    staging_desc.cpuAccess = nvrhi::CpuAccessMode::Read;
    staging_desc.debugName = "SceneArrayDenseReadback";

    nvrhi::BufferHandle staging = device->createBuffer(staging_desc);

    if (!command_list_) {
        nvrhi::CommandListParameters cmd_desc;
        cmd_desc.enableImmediateExecution = false;
        cmd_desc.queueType = nvrhi::CommandQueue::Copy;
        command_list_ = device->createCommandList(cmd_desc);
    }

    std::lock_guard launch_lock(execution_launch_mutex);
    if constexpr (kDenseOutput) {
        command_list_->open();
        command_list_->copyBuffer(
            staging, 0, dense_buffer_, 0, indices_.size() * sizeof(T));
        command_list_->close();
        device->executeCommandList(command_list_, nvrhi::CommandQueue::Copy);
    }
    else {
        for (uint32_t i = 0; i < indices_.size(); ++i) {
            command_list_->open();
            command_list_->copyBuffer(
                staging,
                i * sizeof(T),
                slots_buffer_,
                indices_[i] * sizeof(T),
                sizeof(T));
            command_list_->close();
            device->executeCommandList(
                command_list_, nvrhi::CommandQueue::Copy);
        }
    }
    device->waitForIdle();

    void* mapped = device->mapBuffer(staging, nvrhi::CpuAccessMode::Read);
    if (mapped) {
        T* src = static_cast<T*>(mapped);
        for (uint32_t i = 0; i < indices_.size(); ++i) {
            result.push_back(src[i]);
        }
        device->unmapBuffer(staging);
    }
    return result;
}

template<typename T, bool kDenseOutput>
SceneArray<T, kDenseOutput>::HandleData::~HandleData()
{
    if (pool_alive_ && pool_alive_->load(std::memory_order_acquire)) {
        pool_->remove(this);
    }
    else if (pool_alive_) {
        spdlog::warn(
            "SceneArray: handle outlived its pool, skipping bookkeeping");
    }
}

RUZINO_NAMESPACE_CLOSE_SCOPE
