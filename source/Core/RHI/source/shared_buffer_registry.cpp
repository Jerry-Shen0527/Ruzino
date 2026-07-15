// SharedGPUBufferRegistry implementation — lives in the RHI DLL so all modules
// (sim, render, etc.) share one process-global instance.
#include "RHI/shared_buffer_registry.hpp"

RUZINO_NAMESPACE_OPEN_SCOPE

SharedGPUBufferRegistry& SharedGPUBufferRegistry::get()
{
    // Meyers singleton — thread-safe in C++11+.
    static SharedGPUBufferRegistry instance;
    return instance;
}

void SharedGPUBufferRegistry::register_buffer(const std::string& key,
                                              nvrhi::BufferHandle buf,
                                              size_t bytes,
                                              const void* metadata,
                                              size_t metadata_bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Entry e;
    e.buffer = buf;
    e.byteSize = bytes;
    if (metadata && metadata_bytes > 0) {
        e.metadata.assign(
            static_cast<const uint8_t*>(metadata),
            static_cast<const uint8_t*>(metadata) + metadata_bytes);
    }
    auto it = map_.find(key);
    if (it != map_.end()) {
        e.version = it->second.version + 1;
        it->second = std::move(e);
    } else {
        e.version = 1;
        map_[key] = std::move(e);
    }
}

bool SharedGPUBufferRegistry::lookup(const std::string& key,
                                     nvrhi::BufferHandle& out_buf,
                                     size_t& out_bytes,
                                     uint64_t& out_version,
                                     const void** out_metadata,
                                     size_t* out_metadata_bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it == map_.end()) return false;
    out_buf = it->second.buffer;
    out_bytes = it->second.byteSize;
    out_version = it->second.version;
    if (out_metadata) *out_metadata = it->second.metadata.data();
    if (out_metadata_bytes) *out_metadata_bytes = it->second.metadata.size();
    return true;
}

void SharedGPUBufferRegistry::unregister(const std::string& key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    map_.erase(key);
}

RUZINO_NAMESPACE_CLOSE_SCOPE
