#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <iostream>

#include "nodes/core/logging.hpp"
#include "stage/animation.h"
#include "stage/stage.hpp"

using namespace Ruzino;

int main(int argc, char* argv[])
{
    initialize_framework_logging("clear_time_samples", spdlog::level::info);

    // Check command line arguments
    if (argc < 2) {
        spdlog::error("Usage: {} <usd_file_path>", argv[0]);
        spdlog::info("Example: {} Assets/stage.usdc", argv[0]);
        return 1;
    }

    std::string usd_path = argv[1];

    // Check if file exists
    if (!std::filesystem::exists(usd_path)) {
        spdlog::error("USD file does not exist: {}", usd_path);
        return 1;
    }

    spdlog::info("Opening USD file: {}", usd_path);

    // Create stage
    auto stage = create_custom_global_stage(usd_path);
    if (!stage || !stage->get_usd_stage()) {
        spdlog::error("Failed to open USD stage");
        return 1;
    }

    auto usd_stage = stage->get_usd_stage();
    spdlog::info("Successfully opened stage");

    // Traverse all prims and clear time samples for animatable ones
    int total_prims = 0;
    int animatable_count = 0;

    for (auto prim : usd_stage->Traverse()) {
        total_prims++;

        if (animation::WithDynamicLogicPrim::is_animatable(prim)) {
            animatable_count++;
            spdlog::info(
                "Clearing time samples for animatable prim: {}",
                prim.GetPath().GetString());

            // Create a temporary WithDynamicLogicPrim to call
            // clear_time_samples
            animation::WithDynamicLogicPrim logic_prim(prim, stage.get());
            logic_prim.clear_time_samples(prim);
        }
    }

    spdlog::info(
        "Traversed {} prims, found {} animatable prims",
        total_prims,
        animatable_count);

    // Save the stage
    spdlog::info("Saving stage...");
    stage->Save();
    spdlog::info("Stage saved successfully");

    return 0;
}
