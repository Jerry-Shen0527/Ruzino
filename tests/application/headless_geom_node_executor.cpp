#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "GCore/GOP.h"
#include "GCore/algorithms/intersection.h"
#include "GCore/geom_payload.hpp"
#include "nodes/core/api.hpp"
#include "nodes/core/logging.hpp"
#include "nodes/system/node_system.hpp"
#include "stage/stage.hpp"
using namespace Ruzino;

void print_usage(const char* program_name)
{
    std::cout << "Usage: " << program_name << " [input_json] [output_usd]"
              << std::endl;
    std::cout << "  input_json: Path to input JSON file (default: "
                 "scratch_design.json)"
              << std::endl;
    std::cout << "  output_usd: Path to output USD file (default: scratch.usdc)"
              << std::endl;
}

int main(int argc, char* argv[])
{
    initialize_framework_logging(
        "headless_geom_node_executor", spdlog::level::info);

    // Parse command line arguments
    std::string input_json = "scratch_design.json";
    std::string output_usd = "scratch.usdc";

    if (argc > 1) {
        if (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        input_json = argv[1];
    }

    if (argc > 2) {
        output_usd = argv[2];
    }

    auto system = create_dynamic_loading_system();
    /* Load the node system */
    auto loaded = system->load_configuration("geometry_nodes.json");
    loaded = system->load_configuration("basic_nodes.json");

    // iterate over path Plugin (not recursively), get all the json and
    // load them
    auto plugin_path = std::filesystem::path("./Plugins");

    if (exists(plugin_path))
        for (auto& p : std::filesystem::directory_iterator(plugin_path)) {
            if (p.path().extension() == ".json") {
                system->load_configuration(p.path().string());
            }
        }

    system->init();
    system->set_node_tree_executor(create_node_tree_executor({}));

    std::string nodes_json;

    std::ifstream file(input_json);
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            nodes_json += line + "\n";
        }
        file.close();
    }
    else {
        spdlog::error("Could not open input file: {}", input_json);
        return 1;
    }

    system->get_node_tree()->deserialize(nodes_json);

    // Create USD stage
    auto stage = pxr::UsdStage::CreateNew(output_usd);

    GeomPayload geom_global_params;
#ifdef GEOM_USD_EXTENSION
    geom_global_params.stage = stage;
    geom_global_params.prim_path = pxr::SdfPath("/geom");
#endif
    system->set_global_params(geom_global_params);

    system->execute();

    stage->GetRootLayer()->Export(output_usd);

    system.reset();
    unregister_cpp_type();

#ifdef GPU_GEOM_ALGORITHM
    deinit_gpu_geometry_algorithms();
#endif
    return 0;
}
