#pragma once

#include "api.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

RUZINO_NAMESPACE_OPEN_SCOPE

// Per-node timing for the render graph executor, enabled with the
// RZ_NODE_PROFILE=1 environment variable. Each execute_tree pass (= one
// render() call) emits one single-line JSON record on stdout:
//
//   [RZ_NODE_PROFILE] {"frame":3,"nodes":[{"node":"PathTracer","cpu_ms":1.2,"drain_ms":45.3},...]}
//
// cpu_ms is the executor-side wall time of the node's execute() (it includes
// any in-node readback waits); drain_ms is the post-submit wait for the GPU
// to finish everything the node enqueued (see the EventQuery markers in
// EagerNodeTreeExecutorRender::execute_tree). The serialized cost of a node
// is cpu_ms + drain_ms: GPU-bound nodes show a large drain, CPU-bound nodes
// a large cpu with drain near zero.
struct NodeProfileSample {
    std::string node;
    double cpu_ms = 0.0;
    double drain_ms = 0.0;
};

class NodeProfiler {
   public:
    NodeProfiler()
    {
        const char* env = std::getenv("RZ_NODE_PROFILE");
        enabled = (env != nullptr && std::string(env) == "1");
    }

    bool enabled = false;

    void begin_frame() { current.clear(); }

    void record(std::string node, double cpu_ms, double drain_ms)
    {
        current.push_back(
            NodeProfileSample{std::move(node), cpu_ms, drain_ms});
    }

    void end_frame()
    {
        if (!enabled) {
            return;
        }
        std::string json = "{\"frame\":" + std::to_string(frame) +
                           ",\"nodes\":[";
        for (size_t i = 0; i < current.size(); ++i) {
            if (i != 0) {
                json += ",";
            }
            json += "{\"node\":\"" + current[i].node +
                    "\",\"cpu_ms\":" + std::to_string(current[i].cpu_ms) +
                    ",\"drain_ms\":" + std::to_string(current[i].drain_ms) +
                    "}";
        }
        json += "]}";
        spdlog::info("[RZ_NODE_PROFILE] {}", json);
        ++frame;
    }

   private:
    std::vector<NodeProfileSample> current;
    uint64_t frame = 0;
};

RUZINO_NAMESPACE_CLOSE_SCOPE
