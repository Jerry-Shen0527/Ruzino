//
// #include <filesystem>
// #include <memory>
// #include <regex>
//
// #include "GCore/GOP.h"
// #include "GCore/geom_payload.hpp"
// #include "GUI/window.h"
// #include <spdlog/spdlog.h>
// #include "cmdparser.hpp"
// #include "nodes/system/node_system.hpp"
// #include "nodes/ui/imgui.hpp"
// #include "polyscope_widget/polyscope_info_viewer.h"
// #include "polyscope_widget/polyscope_renderer.h"
// #include "pxr/usd/usd/stage.h"
// #include "pxr/usd/usdGeom/sphere.h"
// #include "stage/stage.hpp"
// #include "usd_nodejson.hpp"
// #include "widgets/usdtree/usd_fileviewer.h"
// #include "widgets/usdview/usdview_widget.hpp"
//
// using namespace Ruzino;
///**
// * I will add a cmd parser to the main function
// * because I need the ability to use custom stage.usdc file
// * */
// int main(int argc, char* argv[])
//{
//    // parse the arguments
//    cmdline::parser parser;
//    parser.add<std::string>("stage", 's', "Custom stage file", false, "");
//    parser.add<std::string>(
//        "gtest_color", 0x00, "Useless. Don't care", false, "");
//    parser.parse_check(argc, argv);
//    std::string stage_filename{ parser.get<std::string>("stage") };
//    // if the length == 0, use the default stage file
//
// #ifdef _DEBUG
//    log::SetMinSeverity(Severity::Debug);
// #endif
//    log::EnableOutputToConsole(true);
//
//    std::unique_ptr<Stage> stage;
//    // load the stage file
//    if (stage_filename.empty()) {
//        stage = create_global_stage();
//        spdlog::info("Use the default stage file!");
//    }
//    else {
//        stage = create_custom_global_stage(stage_filename);
//        spdlog::info("Use the custom stage file! %s", stage_filename.c_str());
//    }
//    init(stage.get());
//
//    // Polyscope need to be initialized before window, or it cannot load
//    opengl
//    // backend correctly.
//    auto polyscope_render = std::make_unique<PolyscopeRenderer>(stage.get());
//    auto polyscope_info_viewer = std::make_unique<PolyscopeInfoViewer>();
//
//    auto window = std::make_unique<Window>();
//
//    auto usd_file_viewer = std::make_unique<UsdFileViewer>(stage.get());
//
//    window->register_widget(std::move(usd_file_viewer));
//
//    window->register_widget(std::move(polyscope_render));
//    window->register_widget(std::move(polyscope_info_viewer));
//    // When the input transform is triggered,
//    // set all the node systems dirty.
//    window->register_function_after_frame([](Window* window) {
//        auto polyscope_render = static_cast<PolyscopeRenderer*>(
//            window->get_widget("Polyscope Renderer"));
//        if (polyscope_render) {
//            bool input_triggered =
//                polyscope_render->GetInputTransformTriggered() ||
//                polyscope_render->GetInputPickTriggered();
//            if (input_triggered) {
//                window->set_all_node_system_dirty();
//            }
//        }
//    });
//
//    window->register_function_after_frame([&stage](Window* window) {
//        pxr::SdfPath json_path;
//        if (stage->consume_editor_creation(json_path)) {
//            auto system = create_dynamic_loading_system();
//
//            auto loaded = system->load_configuration("geometry_nodes.json");
//            loaded = system->load_configuration("basic_nodes.json");
//            loaded = system->load_configuration("polyscope_nodes.json");
//            loaded = system->load_configuration("optimization.json");
//
//            // loading the submission from students
//            namespace fs = std::filesystem;
//            std::regex submission_suffix(R"(.*_nodes_hw_submissions\.json)");
//            spdlog::info("LOADING SUBMISSIONS");
//            for (auto& itr : fs::directory_iterator(".")) {
//                if (std::regex_match(itr.path().string(), submission_suffix))
//                {
//                    spdlog::info("Found: %s", itr.path().string().c_str());
//                    loaded =
//                        system->load_configuration(itr.path().generic_string());
//                }
//            }
//            // finished
//
//            system->init();
//            system->set_node_tree_executor(create_node_tree_executor({}));
//
//            GeomPayload geom_global_params;
//            geom_global_params.stage = stage->get_usd_stage();
//            geom_global_params.prim_path = json_path;
//            geom_global_params.stage_filepath_ = stage->GetStagePath();
//            //            spdlog::warn("Path in the payload is %s",
//            //            geom_global_params.stage_filepath_.c_str());
//
//            // add the stage path to the payload
//            system->set_global_params(geom_global_params);
//
//            UsdBasedNodeWidgetSettings desc;
//
//            desc.json_path = json_path;
//            desc.system = system;
//            desc.stage = stage.get();
//
//            std::unique_ptr<IWidget> node_widget =
//                std::move(create_node_imgui_widget(desc));
//
//            window->register_widget(std::move(node_widget));
//        }
//    });
//
//    window->run();
//
//    unregister_cpp_type();
//
//    // stage.reset();
//    window.reset();
//    stage.reset();
//
//    return 0;
//}
int main()
{
}
