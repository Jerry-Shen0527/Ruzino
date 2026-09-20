#pragma once

// SharedGPUBufferRegistry — a generic, process-global lookup table for sharing
// GPU buffers across module boundaries (e.g. a simulation module produces a
// buffer, a render module consumes it) without a GPU→CPU→USD→CPU→GPU
// round-trip.
//
// The registry is deliberately *semantics-free*: it maps a string key to a
// (buffer handle, byte size, version) triple. It does not know what the buffer
// contains — callers on both sides agree on the data layout out-of-band.
//
// Thread-safety: all operations are mutex-guarded. Producers (register) and
// consumers (lookup) may run on different threads.
//
// Lifetime: the producer owns the buffer. It should unregister when the buffer
// is destroyed to avoid dangling handles. Overwriting an existing key (same
// buffer re-registered each frame) is the common pattern and bumps version.

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "RHI/api.h"

RUZINO_NAMESPACE_OPEN_SCOPE

class SharedGPUBufferRegistry {
   public:
    struct Entry {
        nvrhi::BufferHandle buffer;
        size_t byteSize = 0;
        uint64_t version =
            0;  // bumped on each register_buffer call for this key
        std::vector<uint8_t>
            metadata;  // opaque blob — producer/consumer agree on layout
    };

    /// Meyers singleton — lives in the RHI DLL (rhi.cpp), so all modules share
    /// one instance.
    RHI_API static SharedGPUBufferRegistry& get();

    /// Register (or overwrite) a buffer under `key`. Bumps the version so
    /// consumers can detect content updates. `metadata` is an opaque blob the
    /// consumer can read back via lookup — its layout is agreed upon
    /// out-of-band (the registry does not interpret it). Safe to call every
    /// frame.
    RHI_API void register_buffer(
        const std::string& key,
        nvrhi::BufferHandle buf,
        size_t bytes,
        const void* metadata = nullptr,
        size_t metadata_bytes = 0);

    /// Look up a buffer by key. Returns false if the key is unknown (caller
    /// should fall back to its normal data path). On success, fills out params.
    RHI_API bool lookup(
        const std::string& key,
        nvrhi::BufferHandle& out_buf,
        size_t& out_bytes,
        uint64_t& out_version,
        const void** out_metadata = nullptr,
        size_t* out_metadata_bytes = nullptr);

    /// Remove a key. Call when the producer's buffer is being destroyed.
    RHI_API void unregister(const std::string& key);

    /// Snapshot of (key, version) pairs currently registered, optionally
    /// filtered by key prefix. Lets a consumer poll a whole family of keys
    /// (e.g. every "grid_field/" buffer, see RHI/grid_field_desc.hpp) for
    /// content updates without hard-coding individual key names.
    RHI_API std::vector<std::pair<std::string, uint64_t>> list_versions(
        const std::string& prefix = "");

   private:
    SharedGPUBufferRegistry() = default;

    std::mutex mutex_;
    std::unordered_map<std::string, Entry> map_;
};

RUZINO_NAMESPACE_CLOSE_SCOPE
