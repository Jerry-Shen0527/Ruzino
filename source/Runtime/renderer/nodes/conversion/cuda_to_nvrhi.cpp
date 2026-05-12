#include <cstring>

#include "hd_RUZINO/render_node_base.h"
#include "nodes/core/def/node_def.hpp"

#if RUZINO_WITH_CUDA
#include "RHI/internal/cuda_extension.hpp"
#include "hd_RUZINO/render_global_payload.hpp"

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(cuda_to_nvrhi)
{
    b.add_input<Ruzino::cuda::CUDALinearBufferHandle>("Buffer");
    b.add_input<int>("Width").default_val(1920);
    b.add_input<int>("Height").default_val(1080);
    b.add_output<nvrhi::TextureHandle>("Texture");
}

NODE_EXECUTION_FUNCTION(cuda_to_nvrhi)
{
    auto buffer =
        params.get_input<Ruzino::cuda::CUDALinearBufferHandle>("Buffer");
    int width = params.get_input<int>("Width");
    int height = params.get_input<int>("Height");

    if (!buffer) {
        throw std::runtime_error("Invalid input buffer");
    }

    auto device = global_payload.nvrhi_device;

    // Create texture descriptor
    nvrhi::TextureDesc desc =
        nvrhi::TextureDesc{}
            .setWidth(width)
            .setHeight(height)
            .setFormat(nvrhi::Format::RGBA32_FLOAT)
            .setInitialState(nvrhi::ResourceStates::ShaderResource)
            .setKeepInitialState(true)
            .setIsUAV(true);

    // Convert CUDA buffer to NVRHI texture
    auto texture =
        Ruzino::cuda::cuda_linear_buffer_to_nvrhi_texture(device, buffer, desc);

    params.set_output("Texture", texture);
    return true;
}

NODE_DEF_CLOSE_SCOPE
#endif  // RUZINO_WITH_CUDA
