#define _SILENCE_CXX20_OLD_SHARED_PTR_ATOMIC_SUPPORT_DEPRECATION_WARNING

#include <MaterialXCore/Document.h>
#include <MaterialXFormat/XmlIo.h>
#include <pxr/base/tf/stringUtils.h>
#include <rzconsole/ConsoleInterpreter.h>
#include <rzconsole/ConsoleObjects.h>
#include <rzconsole/imgui_console.h>
#include <rzconsole/spdlog_console_sink.h>
#include <spdlog/spdlog.h>

#include <any>
#include <filesystem>
#include <rzpython/interpreter.hpp>
#include <rzpython/rzpython.hpp>
#include <rzpython/tcp_server.hpp>

#include "GCore/algorithms/intersection.h"
#include "GCore/geom_payload.hpp"
#include "GUI/ImGuiFileDialog.h"
#include "GUI/window.h"
#include "MCore/MaterialXNodeTree.hpp"
#include "MCore/MaterialXNodeTreeWidget.h"
#include "nodes/system/node_system.hpp"
#include "nodes/ui/imgui.hpp"
#include "pxr/base/tf/setenv.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/primSpec.h"
#include "pxr/usd/sdf/propertySpec.h"
#include "pxr/usd/usd/stage.h"
#include "stage/stage.hpp"
#include "usd_nodejson.hpp"
#include "widgets/usdtree/usd_fileviewer.h"
#include "widgets/usdview/usdview_widget.hpp"

using namespace Ruzino;
namespace mx = MaterialX;

class MaterialXNodeSystem : public NodeSystem {
   public:
    MaterialXNodeSystem()
    {
        descriptor = std::make_shared<MaterialXNodeTreeDescriptor>();
    }

    // Factory method to create a MaterialX system with a default material
    // document
    static std::shared_ptr<MaterialXNodeSystem> create_with_default_material(
        const std::string& material_name,
        mx::DocumentPtr ptr = nullptr)
    {
        auto system = std::make_shared<MaterialXNodeSystem>();

        mx::DocumentPtr doc;
        // Create a minimal MaterialX document in memory using MaterialX API
        if (!ptr) {
            doc = mx::createDocument();

            // CRITICAL FIX: Create complete material with standard_surface
            // shader This ensures the .mtlx file has children BEFORE the
            // reference is created
            mx::NodePtr materialNode =
                doc->addNode("surfacematerial", material_name, "material");

            // Create standard_surface shader node with proper nodedef
            std::string shader_name = "standard_surface_surfaceshader";
            mx::NodePtr shaderNode =
                doc->addNode("standard_surface", shader_name, "surfaceshader");

            // Set the nodedef attribute (CRITICAL for USD to create child
            // prims)
            shaderNode->setNodeDefString("ND_standard_surface_surfaceshader");

            // Connect shader to material
            mx::InputPtr surfaceShaderInput =
                materialNode->addInput("surfaceshader", "surfaceshader");
            surfaceShaderInput->setNodeName(shader_name);

            spdlog::info(
                "Created default MaterialX document with standard_surface "
                "shader");
        }
        else {
            doc = ptr;
        }
        // Create MaterialXNodeTree with the in-memory document
        std::unique_ptr<NodeTree> tree = std::make_unique<MaterialXNodeTree>(
            system->node_tree_descriptor(), doc);

        system->init(std::move(tree));
        system->set_node_tree_executor(create_node_tree_executor({}));

        return system;
    }

    void set_node_tree_executor(
        std::unique_ptr<NodeTreeExecutor> executor) override
    {
    }

    bool load_configuration(const std::string& config) override
    {
        return true;
    }

    ~MaterialXNodeSystem() override
    {
    }

    void execute(bool is_ui_execution, Node* required_node) const override
    {
    }

    bool get_dirty()
    {
        return get_node_tree()->GetDirty();
    }

    std::shared_ptr<NodeTreeDescriptor> node_tree_descriptor() override
    {
        return descriptor;
    }

   private:
    std::shared_ptr<MaterialXNodeTreeDescriptor> descriptor;
};

// ---------------------------------------------------------------------------
// Extracted editor creation functions (reusable for startup restoration)
// ---------------------------------------------------------------------------

static void create_geometry_editor(
    Stage* stage,
    Window* window,
    const pxr::SdfPath& json_path,
    const std::shared_ptr<UsdviewEngine*>& render_bare_ptr)
{
    auto system = create_dynamic_loading_system();
    system->load_configuration("geometry_nodes.json");
    system->load_configuration("basic_nodes.json");

    auto plugin_path = std::filesystem::path("./Plugins");
    if (std::filesystem::exists(plugin_path))
        for (auto& p : std::filesystem::directory_iterator(plugin_path)) {
            if (p.path().extension() == ".json") {
                system->load_configuration(p.path().string());
            }
        }

    system->init();
    system->set_node_tree_executor(create_node_tree_executor({}));

    UsdBasedNodeWidgetSettings desc;
    desc.json_path = json_path;
    desc.system = system;
    desc.stage = stage;

    std::unique_ptr<IWidget> node_widget =
        std::move(create_node_imgui_widget(desc));
    node_widget->set_editor_info(json_path.GetString(), "geom");

    node_widget->SetCallBack(
        [stage, json_path, system, render_bare_ptr](Window*, IWidget*) {
            GeomPayload geom_global_params;
#ifdef GEOM_USD_EXTENSION
            geom_global_params.stage = stage->get_usd_stage();
            geom_global_params.prim_path = json_path;

            geom_global_params.is_modifier_mode = true;
            geom_global_params.current_modifier_index = 0;
            geom_global_params.modifier_layer = stage->get_modifier_layer();
            geom_global_params.modifier_input_path = json_path;
            geom_global_params.modifier_output_path = json_path;

            static std::map<pxr::SdfPath, std::string> last_node_graph_map;

            std::string current_node_graph =
                stage->load_string_from_usd(json_path);

            auto it = last_node_graph_map.find(json_path);
            bool is_initial_load = (it == last_node_graph_map.end());
            bool node_graph_changed =
                !is_initial_load && (it->second != current_node_graph);

            if (node_graph_changed) {
                auto modifier_layer = stage->get_modifier_layer();
                if (modifier_layer) {
                    auto prim_spec =
                        modifier_layer->GetPrimAtPath(json_path);
                    if (prim_spec) {
                        std::vector<pxr::SdfPropertySpecHandle> props_to_remove;
                        for (auto pit = prim_spec->GetProperties().begin();
                             pit != prim_spec->GetProperties().end();
                             ++pit) {
                            if (*pit) {
                                props_to_remove.push_back(*pit);
                            }
                        }
                        for (auto& prop : props_to_remove) {
                            prim_spec->RemoveProperty(prop);
                        }
                        prim_spec->SetTypeName(pxr::TfToken());
                    }
                }
                last_node_graph_map[json_path] = current_node_graph;
            }

#endif

            geom_global_params.has_simulation = false;

            if (*render_bare_ptr) {
                geom_global_params.pick =
                    (*render_bare_ptr)->consume_pick_event();

                auto brush = (*render_bare_ptr)->consume_brush_state();
                geom_global_params.brush_point = brush.point;
                geom_global_params.brush_time = brush.time;
                geom_global_params.brush_active = brush.active;
                geom_global_params.brush_new_point = brush.new_point;
            }

            system->set_global_params(geom_global_params);

            if (geom_global_params.pick ||
                geom_global_params.brush_new_point) {
                system->execute();
            }
        });

    window->register_widget(std::move(node_widget));
}

static void create_material_editor(
    Stage* stage,
    Window* window,
    const std::string& material_path_str)
{
    spdlog::info("Material editor requested for: {}", material_path_str);

    pxr::SdfPath material_path(material_path_str);
    auto material_prim =
        stage->get_usd_stage()->GetPrimAtPath(material_path);

    if (!material_prim) {
        spdlog::error("Material prim not found: {}", material_path_str);
        return;
    }

    std::string stage_path = stage->GetStagePath();
    std::filesystem::path stage_file(stage_path);
    std::filesystem::path stage_dir = stage_file.parent_path();

    std::string material_name = material_prim.GetName();
    std::string mtlx_filename = material_name + ".mtlx";
    std::filesystem::path mtlx_path = stage_dir / mtlx_filename;

    bool has_mtlx_file = std::filesystem::exists(mtlx_path);
    bool has_reference = false;

    auto prim_stack = material_prim.GetPrimStack();
    for (const auto& spec : prim_stack) {
        if (spec->HasReferences()) {
            has_reference = true;
            spdlog::info("Material already has reference(s)");
            break;
        }
    }

    std::shared_ptr<MaterialXNodeSystem> mtlx_system;

    if (has_mtlx_file && has_reference) {
        spdlog::info(
            "Loading existing MaterialX file: {}", mtlx_path.string());
        try {
            mx::DocumentPtr existing_doc = mx::createDocument();
            mx::readFromXmlFile(
                existing_doc, mx::FilePath(mtlx_path.string()));

            mtlx_system =
                MaterialXNodeSystem::create_with_default_material(
                    material_name, existing_doc);

            spdlog::info(
                "Successfully loaded existing MaterialX document");
        }
        catch (const std::exception& e) {
            spdlog::error(
                "Failed to load existing MaterialX file: {}", e.what());
            has_mtlx_file = false;
            has_reference = false;
        }
    }

    if (!has_mtlx_file || !has_reference) {
        spdlog::info(
            "Creating new MaterialX file at: {}", mtlx_path.string());

        mtlx_system = MaterialXNodeSystem::create_with_default_material(
            material_name);

        auto* mtlx_tree_temp = static_cast<MaterialXNodeTree*>(
            mtlx_system->get_node_tree());
        mtlx_tree_temp->saveDocument(mx::FilePath(mtlx_path.string()));

        std::string mtlx_relative_path = "./" + mtlx_filename;
        std::string mtlx_material_path_str =
            "/MaterialX/Materials/" + material_name;

        auto references = material_prim.GetReferences();
        references.ClearReferences();
        references.AddReference(pxr::SdfReference(
            mtlx_relative_path, pxr::SdfPath(mtlx_material_path_str)));

        spdlog::info(
            "Added MaterialX reference: {} -> {}",
            mtlx_relative_path,
            mtlx_material_path_str);

        stage->get_usd_stage()->Save();
    }

    FileBasedNodeWidgetSettings widget_desc;
    widget_desc.system = mtlx_system;
    widget_desc.json_path =
        (stage_dir / (material_name + "_layout.json")).string();

    std::unique_ptr<IWidget> node_widget =
        std::make_unique<MaterialXNodeTreeWidget>(
            widget_desc, mtlx_path.string(), material_path_str);

    window->register_widget(std::move(node_widget));
}

class PythonConsoleWidgetFactory : public IWidgetFactory {
   public:
    std::unique_ptr<IWidget> Create(
        const std::vector<std::unique_ptr<IWidget>>& others) override
    {
        // Create Python interpreter
        auto interpreter = python::CreatePythonInterpreter();

        // Create console with capture_log enabled
        ImGui_Console::Options opts;
        opts.show_info = true;
        opts.show_warnings = true;
        opts.show_errors = true;
        opts.capture_log = false;

        auto console = std::make_unique<ImGui_Console>(interpreter, opts);

        // Add some initial messages
        console->Print("==================================");
        console->Print("=== Ruzino Interactive Console ===");
        console->Print("==================================");

        return std::move(console);
    }
};

int main(int argc, char* argv[])
{
#ifdef _DEBUG
    spdlog::set_level(spdlog::level::debug);
#else
    spdlog::set_level(spdlog::level::info);
#endif
    spdlog::set_pattern("%^[%T] %n: %v%$");
    auto window = std::make_unique<Window>();

    // Set MaterialX standard library path using USD's TfSetenv (preferred
    // method)
    std::string mtlx_stdlib = "libraries";
    if (std::filesystem::exists(mtlx_stdlib)) {
        pxr::TfSetenv("PXR_MTLX_STDLIB_SEARCH_PATHS", mtlx_stdlib.c_str());
        spdlog::info("Set PXR_MTLX_STDLIB_SEARCH_PATHS={}", mtlx_stdlib);
    }
    else {
        spdlog::warn("MaterialX stdlib not found at {}", mtlx_stdlib);
    }

    python::initialize();
    // Check for command line arguments to specify USD file
    std::unique_ptr<Stage> stage;
    if (argc > 1) {
        // Use custom stage path from command line
        std::string stage_path = argv[1];
        stage = create_custom_global_stage(stage_path);
    }
    else {
        // Use default stage
        stage = create_global_stage();
    }

#ifdef REAL_TIME
    window->register_function_before_frame(
        [&stage](Window* window) { stage->tick(window->get_elapsed_time()); });
#else

    window->register_function_before_frame(
        [&stage](Window* window) { stage->tick(1.0f / 60.f); });
#endif
    // Add a sphere

    auto usd_file_viewer = std::make_unique<UsdFileViewer>(stage.get());
    auto render = std::make_unique<UsdviewEngine>(stage.get());

    // Use shared_ptr to track render widget lifecycle
    std::shared_ptr<UsdviewEngine*> render_bare_ptr =
        std::make_shared<UsdviewEngine*>(render.get());

    render->SetCallBack([render_bare_ptr](
                            Window* window, IWidget* render_widget) {
        auto node_system = static_cast<const std::shared_ptr<NodeSystem>*>(
            dynamic_cast<UsdviewEngine*>(render_widget)
                ->emit_create_renderer_ui_control());

        if (node_system) {
            UsdviewEngine* engine =
                dynamic_cast<UsdviewEngine*>(*render_bare_ptr);
            auto current_renderer = engine->GetCurrentRenderer();

            FileBasedNodeWidgetSettings desc;
            desc.system = *node_system;
            desc.json_path =
                "../../Assets/" + current_renderer + "/render_nodes_save.json";

            std::unique_ptr<IWidget> node_widget =
                std::move(create_node_imgui_widget(desc));

            window->register_widget(std::move(node_widget));
        }
    });

    window->register_widget(std::move(render));
    window->register_widget(std::move(usd_file_viewer));

    // Register Python Console widget in menu
    auto python_console_factory =
        std::make_unique<PythonConsoleWidgetFactory>();
    window->register_openable_widget(
        std::move(python_console_factory), { "Tools", "Python Console" });
    python::import("GUI_py");
    python::import("stage_py");

    // Add Python reference to window for console access
    python::reference("window", window.get());
    python::reference("stage", stage.get());

    // Start Python TCP server on port 5555
    auto tcp_server = python::create_python_tcp_server(5555);
    tcp_server->set_execute_callback(
        [](const std::string& code) -> std::string {
            auto [success, error] = python::execute_with_error(code);
            python::flush_python_output();
            if (success) {
                return "OK\n";
            }
            return std::string("ERROR: ") + error + "\n";
        });
    tcp_server->start();

    // Register File menu actions
    window->register_menu_action("file_open", [&stage, &window]() {
        auto instance = IGFD::FileDialog::Instance();
        IGFD::FileDialogConfig config;
        config.path = "../../Assets";
        instance->OpenDialog(
            "OpenStageDialog",
            "Open USD Stage",
            "USD Files{.usd,.usda,.usdc,.usdz}",
            config);
    });

    window->register_menu_action(
        "file_save", [&stage, &window]() {
            stage->Save();

            // Persist open editors alongside the scene
            std::vector<std::string> entries;
            for (auto* w : window->get_widgets()) {
                auto path = w->get_associated_prim_path();
                auto type = w->get_editor_type();
                if (!path.empty() && !type.empty()) {
                    entries.push_back(type + ":" + path);
                }
            }
            stage->save_open_editors(entries);
        });

    window->register_menu_action("file_save_as", [&stage, &window]() {
        auto instance = IGFD::FileDialog::Instance();
        IGFD::FileDialogConfig config;
        config.path = "../../Assets";
        instance->OpenDialog(
            "SaveStageDialog",
            "Save USD Stage As",
            "USD Files{.usd,.usda,.usdc,.usdz}",
            config);
    });

    // Subscribe to file dialog result events
    window->events().subscribe(
        "file_open_selected", [&stage, &window](const std::string& file_path) {
            if (stage->OpenStage(file_path)) {
                spdlog::info("Successfully opened stage: {}", file_path);
                // Trigger widget recreation
                window->events().emit("stage_reloaded");
            }
        });

    window->events().subscribe(
        "file_save_as_selected",
        [&stage](const std::string& file_path) { stage->SaveAs(file_path); });

    // Subscribe to stage reload event to recreate widgets
    window->events().subscribe(
        "stage_reloaded",
        [&stage, &window, render_bare_ptr](const std::string&) {
            spdlog::info("Stage reloaded, recreating widgets...");

            // Invalidate old render pointer to prevent use-after-free
            *render_bare_ptr = nullptr;

            // Simply recreate widgets - the old ones will be replaced because
            // they have the same unique name
            auto usd_file_viewer = std::make_unique<UsdFileViewer>(stage.get());
            auto render = std::make_unique<UsdviewEngine>(stage.get());

            // Update the shared pointer to point to new widget
            *render_bare_ptr = render.get();

            render->SetCallBack(
                [render_bare_ptr](Window* window, IWidget* render_widget) {
                    auto node_system =
                        static_cast<const std::shared_ptr<NodeSystem>*>(
                            dynamic_cast<UsdviewEngine*>(render_widget)
                                ->emit_create_renderer_ui_control());
                    if (node_system) {
                        UsdviewEngine* engine =
                            dynamic_cast<UsdviewEngine*>(*render_bare_ptr);
                        auto current_renderer = engine->GetCurrentRenderer();

                        FileBasedNodeWidgetSettings desc;
                        desc.system = *node_system;
                        desc.json_path = "../../Assets/" + current_renderer +
                                         "/render_nodes_save.json";

                        std::unique_ptr<IWidget> node_widget =
                            std::move(create_node_imgui_widget(desc));

                        window->register_widget(std::move(node_widget));
                    }
                });

            window->register_widget(std::move(render));
            window->register_widget(std::move(usd_file_viewer));

            spdlog::info("Widgets recreated successfully");
        });

    // Subscribe to material editor events
    window->events().subscribe(
        "material_editor_requested",
        [&stage, &window](const std::string& material_path_str) {
            create_material_editor(
                stage.get(), window.get(), material_path_str);
        });

    // Subscribe to MaterialX graph change events
    window->events().subscribe(
        "materialx_graph_changed",
        [&stage](const std::string& material_path_str) {
            spdlog::info("MaterialX graph changed for: {}", material_path_str);

            pxr::SdfPath material_path(material_path_str);
            auto material_prim =
                stage->get_usd_stage()->GetPrimAtPath(material_path);

            if (!material_prim) {
                spdlog::error("Material prim not found: {}", material_path_str);
                return;
            }

            // Get the MaterialX file path
            std::string stage_path = stage->GetStagePath();
            std::filesystem::path stage_file(stage_path);
            std::filesystem::path stage_dir = stage_file.parent_path();
            std::string material_name = material_prim.GetName();
            std::filesystem::path mtlx_path =
                stage_dir / (material_name + ".mtlx");

            // Load the MaterialX file to find the surface shader
            mx::DocumentPtr mtlx_doc = mx::createDocument();
            try {
                mx::readFromXmlFile(mtlx_doc, mx::FilePath(mtlx_path.string()));
            }
            catch (const std::exception& e) {
                spdlog::error("Failed to read MaterialX file: {}", e.what());
                return;
            }
            // CRITICAL: Clear USD-layer opinions to let MaterialX reference
            // values show through This solves the opinion strength problem
            // where USD overrides MaterialX

            auto root_layer = stage->get_usd_stage()->GetRootLayer();
            auto material_prim_spec = root_layer->GetPrimAtPath(material_path);

            if (material_prim_spec) {
                // Get all authored attributes from the USD prim (not from
                // reference)
                auto attributes = material_prim.GetAuthoredAttributes();

                for (const auto& attr : attributes) {
                    std::string attr_name = attr.GetName().GetString();

                    // Step 1: Check if this attribute is an input parameter
                    // (starts with "inputs:") and exists in the root layer
                    if (pxr::TfStringStartsWith(attr_name, "inputs:")) {
                        // Check if the attribute is authored in the root layer
                        // (not just coming from the reference)
                        auto attr_spec =
                            material_prim_spec->GetAttributes().get(
                                pxr::TfToken(attr_name));

                        if (attr_spec) {
                            // Step 2: Clear the attribute from the root layer
                            // This removes the USD opinion, allowing MaterialX
                            // reference to win
                            spdlog::info(
                                "Clearing USD opinion for: {} (type: {})",
                                attr_name,
                                attr.GetTypeName().GetCPPTypeName());

                            // Directly use the prim spec handle to remove the
                            // property
                            material_prim_spec->RemoveProperty(attr_spec);
                        }
                    }
                }
            }

            // Re-add the reference to ensure it's up to date
            auto mtlx_relative_path = "./" + material_name + ".mtlx";
            auto mtlx_material_path_str =
                "/MaterialX/Materials/" + material_name;
            auto references = material_prim.GetReferences();
            references.ClearReferences();
            references.AddReference(
                pxr::SdfReference(
                    mtlx_relative_path, pxr::SdfPath(mtlx_material_path_str)));
            // Put on the connection again
            spdlog::info(
                "Re-added MaterialX reference: {} -> {}",
                mtlx_relative_path,
                mtlx_material_path_str);

            //  Find surface shader and sync parameters
            std::string surface_shader_name;
            mx::NodePtr shader_node = nullptr;

            for (auto material_node : mtlx_doc->getMaterialNodes()) {
                auto surfaceshader_input =
                    material_node->getInput("surfaceshader");
                if (surfaceshader_input) {
                    auto connected_node =
                        surfaceshader_input->getConnectedNode();
                    if (connected_node) {
                        surface_shader_name = connected_node->getName();
                        shader_node = connected_node;
                    }
                    else {
                        auto nodename =
                            surfaceshader_input->getAttribute("nodename");
                        if (!nodename.empty()) {
                            surface_shader_name = nodename;
                            shader_node = mtlx_doc->getNode(nodename);
                        }
                    }
                }
                break;
            }

            if (surface_shader_name.empty() || !shader_node) {
                spdlog::warn(
                    "No surface shader found in MaterialX file, skipping USD "
                    "update");
                return;
            }
            // Create USD surface connection
            pxr::SdfPath shader_path =
                material_path.AppendChild(pxr::TfToken(surface_shader_name));
            pxr::SdfPath shader_surface_output_path =
                shader_path.AppendProperty(pxr::TfToken("outputs:surface"));

            pxr::TfToken outputs_surface_token("outputs:surface");
            auto existing_attr =
                material_prim_spec->GetAttributes().get(outputs_surface_token);

            if (existing_attr) {
                auto conn_list = existing_attr->GetConnectionPathList();
                conn_list.ClearEdits();
                conn_list.GetExplicitItems().clear();
                conn_list.GetExplicitItems().push_back(
                    shader_surface_output_path);
            }
            else {
                auto surface_output_attr = pxr::SdfAttributeSpec::New(
                    material_prim_spec,
                    outputs_surface_token,
                    pxr::SdfValueTypeNames->Token);

                if (surface_output_attr) {
                    auto conn_list =
                        surface_output_attr->GetConnectionPathList();
                    conn_list.GetExplicitItems().push_back(
                        shader_surface_output_path);
                }
            }
            stage->get_usd_stage()->Save();
        });
    // Subscribe to document viewer events
    window->events().subscribe(
        "material_doc_viewer_requested",
        [&stage, &window](const std::string& material_path_str) {
            spdlog::info(
                "Material document viewer requested for: {}",
                material_path_str);

            // TODO: Implement document viewer creation
            // This would create a MaterialXDocumentViewer widget
        });

    window->register_function_after_frame(
        [&stage, render_bare_ptr](Window* window) {
            pxr::SdfPath json_path;
            if (stage->consume_editor_creation(json_path)) {
                create_geometry_editor(
                    stage.get(), window, json_path, render_bare_ptr);
            }
        });

    window->register_function_after_frame([render_bare_ptr](Window* window) {
        if (*render_bare_ptr) {
            (*render_bare_ptr)->finish_render();
        }
    });

    window->register_function_after_frame(
        [&stage](Window* window) { stage->finish_tick(); });
    window->SetMaximized(true);

    // Restore editors that were open in the previous session
    {
        auto editors = stage->load_open_editors();
        for (const auto& entry : editors) {
            auto colon = entry.find(':');
            if (colon == std::string::npos)
                continue;

            std::string type = entry.substr(0, colon);
            std::string path = entry.substr(colon + 1);
            pxr::SdfPath sdf_path(path);

            if (!stage->get_usd_stage()->GetPrimAtPath(sdf_path)) {
                spdlog::info(
                    "Skipping editor restoration for removed prim: {}", path);
                continue;
            }

            if (type == "geom") {
                create_geometry_editor(
                    stage.get(), window.get(), sdf_path, render_bare_ptr);
            }
            else if (type == "materialx") {
                create_material_editor(
                    stage.get(), window.get(), path);
            }
        }
        if (!editors.empty()) {
            spdlog::info("Restored {} editor(s) from previous session",
                         editors.size());
        }
    }

    window->run();

    // Save open editors before shutdown
    {
        std::vector<std::string> entries;
        for (auto* w : window->get_widgets()) {
            auto path = w->get_associated_prim_path();
            auto type = w->get_editor_type();
            if (!path.empty() && !type.empty()) {
                entries.push_back(type + ":" + path);
            }
        }
        stage->save_open_editors(entries);
        stage->Save();
    }

    tcp_server->stop();
    unregister_cpp_type();

    window.reset();

#ifdef GPU_GEOM_ALGORITHM
    deinit_gpu_geometry_algorithms();
#endif
    stage.reset();
}
