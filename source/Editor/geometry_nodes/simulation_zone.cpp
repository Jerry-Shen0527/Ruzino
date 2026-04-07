#include "geom_node_base.h"

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
        inputs = params.get_input_group("Simulation In");
        std::vector<entt::meta_any> outputs;

        for (auto& input : inputs) {
            outputs.push_back((*input));
        }

        params.set_output_group("Simulation Out", (outputs));
    }
    else {
        auto& outputs = params.get_storage<SimulationStorage&>().data;
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

NODE_DEF_CLOSE_SCOPE