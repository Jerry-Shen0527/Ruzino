#include "geom_node_base.h"
#include "spdlog/spdlog.h"

// Node Simulation Zone
// For the first run, the simulation_in node will take in the data from the
// input. After several nodes, the simulation_out node will receive the data and
// store it in its storage. In the execution system, the storage data will be
// passed back to the simulation_in node's storage. In the next run, the
// simulation_in node will use the storage data as its output.

struct SimulationStorage {
    std::vector<entt::meta_any> data;
    static constexpr bool has_storage = false;
};

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(simulation_in)
{
    b.add_input_group("Simulation In");
    b.add_output_group("Simulation Out");
}

NODE_EXECUTION_FUNCTION(simulation_in)
{
    auto& global_payload = params.get_global_payload<GeomPayload&>();
    global_payload.has_simulation = true;

    std::vector<entt::meta_any*> inputs;
    if (!global_payload.is_simulating) {
        // Init frame: forward the upstream inputs straight through so the zone
        // interior cooks this frame, and simulation_out captures the result.
        inputs = params.get_input_group("Simulation In");
        std::vector<entt::meta_any> outputs;

        for (auto& input : inputs) {
            outputs.push_back((*input));
        }

        params.set_output_group("Simulation Out", (outputs));
    }
    else {
        // Advance frame: replay the storage that simulation_out wrote last
        // cook (moved here by the eager executor's feedback move).
        auto& storage = params.get_storage<SimulationStorage&>();
        auto& outputs = storage.data;
        if (outputs.empty()) {
            spdlog::warn(
                "[sim_in] advancing with empty feedback storage -- "
                "simulation_out did not run last frame");
        }
        params.set_output_group("Simulation Out", (outputs));
    }

    return true;
}

NODE_DECLARATION_ALWAYS_DIRTY(simulation_in);

NODE_DECLARATION_FUNCTION(simulation_out)
{
    b.add_input_group("Simulation In");
    b.add_output_group("Simulation Out");
}

NODE_EXECUTION_FUNCTION(simulation_out)
{
    // Capture this frame's result into storage. The eager executor's
    // forward_output_to_input moves our storage into simulation_in's storage
    // after this cook, so the next frame's simulation_in can replay it.
    auto inputs = params.get_input_group("Simulation In");

    std::vector<entt::meta_any> outputs;

    for (auto& input : inputs) {
        outputs.push_back((*input));
    }
    params.get_storage<SimulationStorage&>().data = outputs;
    params.set_output_group("Simulation Out", (outputs));
    return true;
}

NODE_DECLARATION_ALWAYS_DIRTY(simulation_out);
NODE_DECLARATION_REQUIRED(simulation_out);

NODE_DEF_CLOSE_SCOPE