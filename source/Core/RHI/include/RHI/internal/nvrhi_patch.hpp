#pragma once
#include <RHI/api.h>
#include <nvrhi/nvrhi.h>

#include <memory>

namespace nvrhi {
using CommandListDesc = nvrhi::CommandListParameters;
typedef static_vector<BindingLayoutDesc, c_MaxBindingLayouts>
    BindingLayoutDescVector;

struct StagingTextureDesc : public nvrhi::TextureDesc { };

struct RHI_API CPUBuffer {
    void* data;

    ~CPUBuffer()
    {
        delete[] static_cast<unsigned char*>(data);
    }
};

struct CPUBufferDesc {
    size_t size;
};

using CPUBufferHandle = std::shared_ptr<CPUBuffer>;
}  // namespace nvrhi
