#include "GPUContext/compute_context.hpp"
#include "hd_RUZINO/render_node_base.h"
#include "nodes/core/def/node_def.hpp"
#include "nvrhi/utils.h"
#include "utils/math.h"
NODE_DEF_OPEN_SCOPE
struct RNGBufferStorage {
    static constexpr bool has_storage = false;
    nvrhi::BufferHandle random_number = nullptr;
};

// This texture is for repeated read and write.
NODE_DECLARATION_FUNCTION(rng_buffer)
{
    b.add_output<nvrhi::BufferHandle>("Random Number");
}

NODE_EXECUTION_FUNCTION(rng_buffer)
{
    auto size = get_size(params);
    auto length = size[0] * size[1];

    nvrhi::BufferDesc output_desc;
    output_desc.debugName = "Random Number Buffer";
    output_desc.structStride = sizeof(uint32_t);
    output_desc.byteSize = length * sizeof(uint32_t);
    output_desc.format = nvrhi::Format::R32_UINT;
    output_desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    output_desc.keepInitialState = true;
    output_desc.canHaveUAVs = true;

    auto& stored_rng = params.get_storage<RNGBufferStorage&>();
    bool first_attempt = stored_rng.random_number == nullptr ||
                         output_desc != stored_rng.random_number->getDesc();
    if (first_attempt) {
        resource_allocator.destroy(stored_rng.random_number);
        stored_rng.random_number = resource_allocator.create(output_desc);

        ProgramDesc program_desc;
        ProgramHandle compute_shader;

        program_desc.set_path("utils/random_init_buffer.slang")
            .set_entry_name("main")
            .shaderType = nvrhi::ShaderType::Compute;

        compute_shader = resource_allocator.create(program_desc);
        MARK_DESTROY_NVRHI_RESOURCE(compute_shader);
        CHECK_PROGRAM_ERROR(compute_shader);

        ProgramVars vars(resource_allocator, compute_shader);
        vars["inout_random"] = stored_rng.random_number;
        vars.finish_setting_vars();

        ComputeContext compute_context(resource_allocator, vars);

        compute_context.finish_setting_pso();
        compute_context.begin();
        compute_context.dispatch({}, vars, length, 128);
        compute_context.finish();
    }

    params.set_output("Random Number", stored_rng.random_number);
    return true;
}

NODE_DECLARATION_UI(rng_buffer);
NODE_DEF_CLOSE_SCOPE
