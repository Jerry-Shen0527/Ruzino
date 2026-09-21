#include "node_exec_eager_render.hpp"

#include <chrono>
#include <set>

#include "hd_RUZINO/render_global_payload.hpp"
#include "nodes/core/node_exec.hpp"
#include "nodes/core/node_exec_eager.hpp"

RUZINO_NAMESPACE_OPEN_SCOPE

namespace {

using profiler_clock = std::chrono::steady_clock;

inline double elapsed_ms(
    profiler_clock::time_point start,
    profiler_clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}

}  // namespace

bool EagerNodeTreeExecutorRender::execute_node(NodeTree* tree, Node* node)
{
    if (EagerNodeTreeExecutor::execute_node(tree, node)) {
        for (auto&& input : node->get_inputs()) {
            auto& input_state = input_states[index_cache[input]];
            if (!node->typeinfo->ALWAYS_REQUIRED && input_state.is_last_used) {
                if (input_state.value && !input_state.keep_alive)
                    resource_allocator().destroy(input_state.value);
                input_state.is_last_used = false;
            }
        }
        return true;
    }
    for (auto&& output : node->get_outputs()) {
        {
            if (output_states[index_cache[output]].value)
                resource_allocator().destroy(
                    output_states[index_cache[output]].value);
        }
    }
    return false;
}

void EagerNodeTreeExecutorRender::execute_tree(NodeTree* tree)
{
    // Node profiling (RZ_NODE_PROFILE=1): per-node CPU submit time plus a
    // serialized GPU-drain wait. Nodes submit GPU work asynchronously during
    // execute(), so without the drain wait all GPU time would be misattributed
    // to whichever node blocks first (or to render()'s final waitForIdle).
    // After each node, EventQuery markers on the Graphics and Copy queues
    // resolve once everything submitted so far has completed; waiting on them
    // bounds the node's GPU cost. This removes CPU/GPU pipelining overlap, so
    // profiled frame totals are an upper bound — compare against an unprofiled
    // run for the true frame time.
    NodeProfiler& profiler =
        global_payload.cast<RenderGlobalPayload&>().node_profiler;
    nvrhi::IDevice* device =
        profiler.enabled
            ? global_payload.cast<RenderGlobalPayload&>().nvrhi_device
            : nullptr;    nvrhi::EventQueryHandle graphics_marker;
    nvrhi::EventQueryHandle copy_marker;
    if (profiler.enabled) {
        profiler.begin_frame();
        graphics_marker = device->createEventQuery();
        copy_marker = device->createEventQuery();
    }

    // Execute all nodes without caching - always execute dirty nodes
    for (int i = 0; i < nodes_to_execute_count; ++i) {
        auto node = nodes_to_execute[i];

        const auto cpu_start = profiler_clock::now();

        // Execute node without cache checks
        auto result = execute_node(tree, node);
        if (result) {
            forward_output_to_input(node);
            // Don't mark as clean or set cache flags - no caching in render
        }

        if (profiler.enabled) {
            double cpu_ms = elapsed_ms(cpu_start, profiler_clock::now());

            device->setEventQuery(
                graphics_marker, nvrhi::CommandQueue::Graphics);
            device->setEventQuery(copy_marker, nvrhi::CommandQueue::Copy);
            const auto wait_start = profiler_clock::now();
            device->waitEventQuery(graphics_marker);
            device->waitEventQuery(copy_marker);
            double drain_ms = elapsed_ms(wait_start, profiler_clock::now());

            std::string node_name = node->ui_name.empty()
                                        ? std::string(node->typeinfo->id_name)
                                        : std::string(node->ui_name);
            profiler.record(std::move(node_name), cpu_ms, drain_ms);
        }
    }

    if (profiler.enabled) {
        const auto storage_start = profiler_clock::now();
        try_storage();
        profiler.record(
            "__post_storage__",
            elapsed_ms(storage_start, profiler_clock::now()),
            0.0);
        profiler.end_frame();
    }
    else {
        try_storage();
    }

    // Don't save to persistent cache in render version
    // This ensures fresh execution every time
    dirty_nodes.clear();
}

void EagerNodeTreeExecutorRender::try_storage()
{
    for (auto&& value : storage) {
        resource_allocator().destroy(value.second);
    }
    EagerNodeTreeExecutor::try_storage();
}

void EagerNodeTreeExecutorRender::remove_storage(
    const std::set<std::string>::value_type& key)
{
    resource_allocator().destroy(storage[key]);
    EagerNodeTreeExecutor::remove_storage(key);
}

void EagerNodeTreeExecutorRender::finalize(NodeTree* tree)
{
    for (int i = 0; i < input_states.size(); ++i) {
        if (input_states[i].is_last_used && !input_states[i].keep_alive) {
            resource_allocator().destroy(input_states[i].value);
            input_states[i].is_last_used = false;
        }
    }

    for (int i = 0; i < output_states.size(); ++i) {
        if (output_states[i].is_last_used) {
            resource_allocator().destroy(output_states[i].value);
            output_states[i].is_last_used = false;
        }
    }
}

void EagerNodeTreeExecutorRender::reset_allocator()
{
    for (auto&& value : storage) {
        resource_allocator().destroy(value.second);
    }
    resource_allocator().terminate();
    storage.clear();
}

RUZINO_NAMESPACE_CLOSE_SCOPE
