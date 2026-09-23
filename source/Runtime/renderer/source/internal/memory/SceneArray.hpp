#pragma once
#include <spdlog/spdlog.h>

#include <RHI/rhi.hpp>
#include <algorithm>
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
/// Lifetime (reworked 2026-09-23): all CPU bookkeeping lives in a shared
/// State block that every handle also references. A handle outliving the
/// table (or being destroyed concurrently with it) does its bookkeeping
/// under State::mutex on memory that is guaranteed alive — the table marks
/// the state dead under the same mutex in its destructor, and late handles
/// degrade to a warned no-op. No liveness token, no check-then-use race.
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
/// workers and are serialized by the State mutex. flush() is called from
/// the render thread before binding; it holds the State mutex across the
/// GPU upload. The upload takes the global execution_launch_mutex,
/// matching DeviceMemoryPool::write_data (lock order is always
/// State::mutex -> execution_launch_mutex).
template<typename T, bool kDenseOutput = false>
class SceneArray {
    struct State;  // defined below; handles reference it

   public:
    struct HandleData {
       private:
        friend class SceneArray;
        uint32_t slot_ = INVALID_SLOT;
        uint32_t generation_ = 0;
        // Shared with the table: keeps the bookkeeping alive past the
        // table's own destruction.
        std::shared_ptr<State> state_;

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
        // Mark the shared state dead so handles dying later (or racing
        // this destructor) skip bookkeeping under the same mutex instead
        // of touching the GPU members being torn down here.
        std::lock_guard lock(state_->mutex);
        state_->pool_dead = true;
    }
    SceneArray(const SceneArray&) = delete;
    SceneArray& operator=(const SceneArray&) = delete;

    /// Add a new entry; returns an owning handle. Removing happens when the
    /// handle's last copy dies.
    Handle add(const T& data);

    /// Overwrite an entry. Returns false (and logs) if the handle is stale
    /// or belongs to a different table.
    bool update(const Handle& handle, const T& data);

    uint32_t count() const;
    bool dirty() const;
    uint64_t version() const;

    /// Upload modified CPU state to the GPU mirrors. Safe to call when
    /// clean (no-op, except the very first call, which creates the GPU
    /// mirrors even for an empty table — consumers bind the buffers
    /// unconditionally, and a null structured-buffer binding crashes
    /// nvrhi's manual state-transition paths). Call from the render
    /// thread before binding. Returns the dense entry count consistent
    /// with what is now on the GPU, so callers can't race a concurrent
    /// add/remove between flush and count.
    uint32_t flush();

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

    // Everything a handle's destructor must touch to unregister itself.
    // Lives behind a shared_ptr held by both the table and every handle,
    // which is what makes handle teardown race-free against ~SceneArray.
    struct State {
        std::mutex mutex;

        // CPU truth. slots keeps stale data in freed slots (never read
        // through the dense path); indices is always hole-free.
        std::vector<T> slots;
        std::vector<uint32_t> generations;
        std::vector<uint32_t> dense_positions;  // slot -> index into indices
        std::vector<uint32_t> free_slots;
        std::vector<uint32_t> indices;  // dense rows: slot ids

        bool dirty = false;
        uint64_t version = 0;
        bool pool_dead = false;  // set under mutex by ~SceneArray
    };

    // Swap-remove bookkeeping. Caller holds st.mutex.
    static void remove_locked(State& st, uint32_t slot, uint32_t generation);

    void ensure_gpu_capacity_locked(size_t needed_slots);
    void upload_locked();  // caller holds state_->mutex

    std::shared_ptr<State> state_ = std::make_shared<State>();

    // GPU mirrors (2^N capacity, grow-only, rebuilt whole on flush).
    // Guarded by state_->mutex (flush holds it across the upload).
    nvrhi::BufferHandle slots_buffer_;    // kDenseOutput = false
    nvrhi::BufferHandle indices_buffer_;  // kDenseOutput = false
    nvrhi::BufferHandle dense_buffer_;    // kDenseOutput = true
    std::vector<T> dense_scratch_;        // kDenseOutput repack scratch
    size_t gpu_capacity_ = 0;
    nvrhi::BufferDesc base_desc_;
    // mutable: lazily created by const debug helpers (get_dense_content)
    mutable nvrhi::CommandListHandle command_list_;
};

template<typename T, bool kDenseOutput>
typename SceneArray<T, kDenseOutput>::Handle SceneArray<T, kDenseOutput>::add(
    const T& data)
{
    std::lock_guard lock(state_->mutex);
    State& st = *state_;

    uint32_t slot;
    if (!st.free_slots.empty()) {
        slot = st.free_slots.back();
        st.free_slots.pop_back();
    }
    else {
        slot = static_cast<uint32_t>(st.slots.size());
        st.slots.emplace_back();
        st.generations.push_back(0);
        st.dense_positions.push_back(0);
    }

    st.slots[slot] = data;
    st.dense_positions[slot] = static_cast<uint32_t>(st.indices.size());
    st.indices.push_back(slot);

    auto handle = std::make_shared<HandleData>();
    handle->slot_ = slot;
    handle->generation_ = st.generations[slot];
    handle->state_ = state_;

    st.dirty = true;
    return handle;
}

template<typename T, bool kDenseOutput>
bool SceneArray<T, kDenseOutput>::update(const Handle& handle, const T& data)
{
    if (!handle)
        return false;
    if (handle->state_ != state_) {
        spdlog::warn("SceneArray::update: handle belongs to a different table");
        return false;
    }
    std::lock_guard lock(state_->mutex);
    State& st = *state_;
    if (handle->slot_ >= st.slots.size() ||
        handle->generation_ != st.generations[handle->slot_]) {
        spdlog::warn(
            "SceneArray::update: stale handle (slot {}, generation {} vs {})",
            handle->slot_,
            handle->generation_,
            handle->slot_ < st.generations.size()
                ? st.generations[handle->slot_]
                : 0xFFFFFFFFu);
        return false;
    }
    st.slots[handle->slot_] = data;
    st.dirty = true;
    return true;
}

template<typename T, bool kDenseOutput>
uint32_t SceneArray<T, kDenseOutput>::count() const
{
    std::lock_guard lock(state_->mutex);
    return static_cast<uint32_t>(state_->indices.size());
}

template<typename T, bool kDenseOutput>
bool SceneArray<T, kDenseOutput>::dirty() const
{
    std::lock_guard lock(state_->mutex);
    return state_->dirty;
}

template<typename T, bool kDenseOutput>
uint64_t SceneArray<T, kDenseOutput>::version() const
{
    std::lock_guard lock(state_->mutex);
    return state_->version;
}

template<typename T, bool kDenseOutput>
void SceneArray<T, kDenseOutput>::remove_locked(
    State& st,
    uint32_t slot,
    uint32_t generation)
{
    if (slot >= st.slots.size() || generation != st.generations[slot]) {
        return;  // stale handle (slot already recycled) — nothing to do
    }

    const uint32_t dense_pos = st.dense_positions[slot];
    const uint32_t moved_slot = st.indices.back();
    st.indices[dense_pos] = moved_slot;
    st.dense_positions[moved_slot] = dense_pos;
    st.indices.pop_back();

    st.generations[slot]++;  // invalidate every outstanding handle to this slot
    st.free_slots.push_back(slot);
    st.dirty = true;
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

    const State& st = *state_;
    const size_t live_slots = st.slots.size();
    ensure_gpu_capacity_locked(live_slots == 0 ? 16 : live_slots);

    // Whole-buffer upload: scene tables are small (lights: tens of KB at
    // most), so skip per-slot dirty tracking entirely.
    {
        std::lock_guard launch_lock(execution_launch_mutex);
        command_list_->open();
        if constexpr (kDenseOutput) {
            // Re-pack live entries into the contiguous output buffer.
            dense_scratch_.resize(st.indices.size());
            for (uint32_t i = 0; i < st.indices.size(); ++i) {
                dense_scratch_[i] = st.slots[st.indices[i]];
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
                    slots_buffer_, st.slots.data(), live_slots * sizeof(T), 0);
            }
            if (!st.indices.empty()) {
                command_list_->writeBuffer(
                    indices_buffer_,
                    st.indices.data(),
                    st.indices.size() * sizeof(uint32_t),
                    0);
            }
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
uint32_t SceneArray<T, kDenseOutput>::flush()
{
    std::lock_guard lock(state_->mutex);
    // Skip only when clean AND the mirrors exist: a never-dirty table
    // (zero entries from birth) must still materialize its GPU buffers,
    // or the first consumer frame binds null buffers.
    if (state_->dirty || gpu_capacity_ == 0) {
        upload_locked();
        state_->dirty = false;
        state_->version++;
    }
    return static_cast<uint32_t>(state_->indices.size());
}

template<typename T, bool kDenseOutput>
nvrhi::IBuffer* SceneArray<T, kDenseOutput>::get_slots_buffer() const
{
    static_assert(
        !kDenseOutput,
        "dense-output tables have no slot-layout GPU mirror; use "
        "get_dense_buffer()");
    std::lock_guard lock(state_->mutex);
    return slots_buffer_;
}

template<typename T, bool kDenseOutput>
nvrhi::IBuffer* SceneArray<T, kDenseOutput>::get_indices_buffer() const
{
    static_assert(
        !kDenseOutput,
        "dense-output tables have no index list; use "
        "get_dense_buffer()");
    std::lock_guard lock(state_->mutex);
    return indices_buffer_;
}

template<typename T, bool kDenseOutput>
nvrhi::IBuffer* SceneArray<T, kDenseOutput>::get_dense_buffer() const
{
    static_assert(
        kDenseOutput,
        "slot-layout tables are iterated via slots[indices[i]]; "
        "their data buffer is not contiguous");
    std::lock_guard lock(state_->mutex);
    return dense_buffer_;
}

template<typename T, bool kDenseOutput>
nvrhi::BindingSetItem SceneArray<T, kDenseOutput>::get_slots_descriptor(
    nvrhi::ResourceType type) const
{
    std::lock_guard lock(state_->mutex);
    nvrhi::BindingSetItem item;
    item.resourceHandle = slots_buffer_;
    item.type = type;
    return item;
}

template<typename T, bool kDenseOutput>
nvrhi::BindingSetItem SceneArray<T, kDenseOutput>::get_indices_descriptor(
    nvrhi::ResourceType type) const
{
    std::lock_guard lock(state_->mutex);
    nvrhi::BindingSetItem item;
    item.resourceHandle = indices_buffer_;
    item.type = type;
    return item;
}

template<typename T, bool kDenseOutput>
std::vector<T> SceneArray<T, kDenseOutput>::get_dense_content()
{
    // Read back the GPU mirror the way a shader loop sees it.
    std::lock_guard lock(state_->mutex);
    const bool mirrors_exist =
        kDenseOutput ? (dense_buffer_ != nullptr)
                     : (slots_buffer_ != nullptr && indices_buffer_ != nullptr);
    if (state_->dirty || !mirrors_exist) {
        upload_locked();
        state_->dirty = false;
        state_->version++;
    }
    std::vector<T> result;
    result.reserve(state_->indices.size());

    auto device = RHI::get_device();
    nvrhi::BufferDesc staging_desc;
    staging_desc.byteSize =
        std::max<size_t>(state_->indices.size(), 1) * sizeof(T);
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
            staging, 0, dense_buffer_, 0, state_->indices.size() * sizeof(T));
        command_list_->close();
        device->executeCommandList(command_list_, nvrhi::CommandQueue::Copy);
    }
    else {
        for (uint32_t i = 0; i < state_->indices.size(); ++i) {
            command_list_->open();
            command_list_->copyBuffer(
                staging,
                i * sizeof(T),
                slots_buffer_,
                state_->indices[i] * sizeof(T),
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
        for (uint32_t i = 0; i < state_->indices.size(); ++i) {
            result.push_back(src[i]);
        }
        device->unmapBuffer(staging);
    }
    return result;
}

template<typename T, bool kDenseOutput>
SceneArray<T, kDenseOutput>::HandleData::~HandleData()
{
    if (!state_)
        return;
    // The state block is shared_ptr-kept alive by this very handle, so
    // locking it can never race the table's destruction: ~SceneArray
    // marks pool_dead under the same mutex before releasing anything.
    std::lock_guard lock(state_->mutex);
    if (state_->pool_dead) {
        spdlog::warn(
            "SceneArray: handle outlived its pool, skipping bookkeeping");
        return;
    }
    remove_locked(*state_, slot_, generation_);
}

RUZINO_NAMESPACE_CLOSE_SCOPE
