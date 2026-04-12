#include <spdlog/spdlog.h>

#include <filesystem>
#include <iostream>
#include <string>

#include "GUI/ImGuiFileDialog.h"
#include "GUI/window.h"
#include "nodes/core/logging.hpp"
#include "nodes/core/node_tree.hpp"
#include "nodes/system/node_system.hpp"
#include "nodes/ui/imgui.hpp"

using namespace Ruzino;

struct FooExecutor : public NodeTreeExecutor {
    void prepare_tree(NodeTree* tree, Node* required_node) override
    {
    }

    void execute_tree(NodeTree* tree) override
    {
    }

    std::shared_ptr<NodeTreeExecutor> clone_empty() const override
    {
        return std::make_shared<FooExecutor>();
    }
};

class NodeEditorWidget : public IWidget {
   private:
    std::shared_ptr<NodeSystem> system_;
    std::string current_file_path_;
    std::unique_ptr<IWidget> node_widget_;
    bool file_changed_ = false;

   public:
    NodeEditorWidget(
        std::shared_ptr<NodeSystem> system,
        const std::string& initial_path)
        : system_(system),
          current_file_path_(initial_path)
    {
        LoadNodeGraph(current_file_path_);
    }

    bool HasMenuBar() const override
    {
        return true;
    }

    void DrawMenuBar() override
    {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New", "Ctrl+N")) {
                NewFile();
            }
            if (ImGui::MenuItem("Open...", "Ctrl+O")) {
                OpenFile();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(
                    "Save", "Ctrl+S", false, !current_file_path_.empty())) {
                SaveFile();
            }
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S")) {
                SaveAsFile();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                // Handle exit
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z")) {
                // Handle undo
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y")) {
                // Handle redo
            }
            ImGui::EndMenu();
        }
    }
    bool BuildUI() override
    {
        // Handle keyboard shortcuts
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_N))) {
                NewFile();
            }
            else if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_O))) {
                OpenFile();
            }
            else if (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_S))) {
                if (io.KeyShift) {
                    SaveAsFile();
                }
                else {
                    SaveFile();
                }
            }
        }

        // Display current file status
        ImGui::Text(
            "Current file: %s%s",
            current_file_path_.empty() ? "Untitled"
                                       : current_file_path_.c_str(),
            file_changed_ ? "*" : "");

        // Handle ImGui File Dialog display
        HandleFileDialogs();

        // Render the actual node editor
        if (node_widget_) {
            return node_widget_->BuildUI();
        }
        return true;
    }

    const char* GetWindowName() override
    {
        return "Node Editor";
    }

    ImGuiWindowFlags GetWindowFlag() override
    {
        return ImGuiWindowFlags_MenuBar;
    }

   private:
    void NewFile()
    {
        current_file_path_.clear();
        file_changed_ = false;
        // Create new empty node widget
        FileBasedNodeWidgetSettings widget_desc;
        widget_desc.system = system_;
        widget_desc.json_path = "";
        node_widget_ = create_node_imgui_widget(widget_desc);

        spdlog::info("Created new node graph");
    }

    void OpenFile()
    {
        // Open file dialog
        IGFD::FileDialogConfig config;
        config.path = ".";
        IGFD::FileDialog::Instance()->OpenDialog(
            "ChooseFileDlgKey",
            "Choose File",
            "JSON Files (*.json){.json}",
            config);
    }

    void SaveFile()
    {
        if (current_file_path_.empty()) {
            SaveAsFile();
        }
        else {
            SaveNodeGraph(current_file_path_);
        }
    }

    void SaveAsFile()
    {
        // Save file dialog
        IGFD::FileDialogConfig config;
        config.path = ".";
        config.flags = ImGuiFileDialogFlags_ConfirmOverwrite;
        IGFD::FileDialog::Instance()->OpenDialog(
            "SaveFileDlgKey",
            "Save File",
            "JSON Files (*.json){.json}",
            config);
    }

    void HandleFileDialogs()
    {
        // Handle Open Dialog
        if (IGFD::FileDialog::Instance()->Display("ChooseFileDlgKey")) {
            if (IGFD::FileDialog::Instance()->IsOk()) {
                std::string filePathName =
                    IGFD::FileDialog::Instance()->GetFilePathName();
                LoadNodeGraph(filePathName);
            }
            IGFD::FileDialog::Instance()->Close();
        }

        // Handle Save Dialog
        if (IGFD::FileDialog::Instance()->Display("SaveFileDlgKey")) {
            if (IGFD::FileDialog::Instance()->IsOk()) {
                std::string filePathName =
                    IGFD::FileDialog::Instance()->GetFilePathName();
                SaveNodeGraph(filePathName);
            }
            IGFD::FileDialog::Instance()->Close();
        }
    }

    void LoadNodeGraph(const std::string& file_path)
    {
        if (std::filesystem::exists(file_path)) {
            current_file_path_ = file_path;
            file_changed_ = false;

            // Create new node widget with the loaded configuration
            FileBasedNodeWidgetSettings widget_desc;
            widget_desc.system = system_;
            widget_desc.json_path = file_path;
            node_widget_ = create_node_imgui_widget(widget_desc);

            spdlog::info("Loaded node graph from: %s", file_path.c_str());
        }
        else {
            // Create new empty widget
            FileBasedNodeWidgetSettings widget_desc;
            widget_desc.system = system_;
            widget_desc.json_path = file_path;
            node_widget_ = create_node_imgui_widget(widget_desc);

            current_file_path_ = file_path;
            file_changed_ = false;
            spdlog::info("Created new node graph: %s", file_path.c_str());
        }
    }

    void SaveNodeGraph(const std::string& file_path)
    {
        try {
            // Save the current node tree state
            if (system_ && system_->get_node_tree()) {
                // Implementation depends on your node system's save
                // functionality This is a placeholder - you'll need to
                // implement actual saving
                current_file_path_ = file_path;
                file_changed_ = false;
                spdlog::info("Saved node graph to: %s", file_path.c_str());
            }
        }
        catch (const std::exception& e) {
            spdlog::error("Failed to save node graph: %s", e.what());
        }
    }
};

int main(int argc, char** argv)
{
    // Setup logging
#ifdef _DEBUG
    const auto log_level = spdlog::level::debug;
#else
    const auto log_level = spdlog::level::info;
#endif
    initialize_framework_logging("node_editor", log_level);

    // Default configuration
    std::string json_path = "node_editor_config.json";
    std::vector<std::string> config_files = { "geometry_nodes.json",
                                              "basic_nodes.json" };
    std::string plugin_path_str = "./Plugins";  // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n";
            std::cout << "Options:\n";
            std::cout
                << "  --main-json <file>   Main node editor configuration file "
                   "(default: node_editor_config.json)\n";
            std::cout << "  --config <file>      Add configuration JSON file "
                         "(can be used multiple times)\n";
            std::cout << "  --plugin-path <dir>  Set plugin directory path "
                         "(default: ./Plugins)\n";
            std::cout
                << "  --clear-configs      Clear default configuration files\n";
            std::cout << "  --help, -h           Show this help message\n";
            std::cout << "\nDefault configuration files: geometry_nodes.json, "
                         "basic_nodes.json\n";
            std::cout << "Default plugin path: ./Plugins\n";
            std::cout << "\nExamples:\n";
            std::cout << "  " << argv[0] << " --main-json my_config.json\n";
            std::cout
                << "  " << argv[0]
                << " --config extra_nodes.json --plugin-path /custom/plugins\n";
            std::cout << "  " << argv[0]
                      << " --clear-configs --config custom_only.json\n";
            return 0;
        }
        else if (arg == "--main-json" && i + 1 < argc) {
            json_path = argv[++i];
        }
        else if (arg == "--config" && i + 1 < argc) {
            // Add configuration file
            config_files.push_back(argv[++i]);
        }
        else if (arg == "--plugin-path" && i + 1 < argc) {
            // Set plugin directory path
            plugin_path_str = argv[++i];
        }
        else if (arg == "--clear-configs") {
            // Clear default configuration files
            config_files.clear();
        }
        else if (arg.empty() || arg[0] == '-') {
            spdlog::error("Unknown option: {}", arg);
            spdlog::error("Use --help for usage information.");
            return 1;
        }
        else {
            // Backward compatibility: treat first non-option argument as main
            // JSON
            if (json_path == "node_editor_config.json") {
                json_path = arg;
            }
            else {
                spdlog::error("Unexpected argument: {}", arg);
                spdlog::error("Use --help for usage information.");
                return 1;
            }
        }
    }

    std::cout << "Node editor configuration:\n";
    std::cout << "  Main JSON: " << json_path << "\n";
    std::cout << "  Config files: ";
    for (const auto& config : config_files) {
        std::cout << config << " ";
    }
    std::cout << "\n  Plugin path: " << plugin_path_str << std::endl;

    // Create window
    auto window = std::make_unique<Window>();

    // Create node system
    auto system = create_dynamic_loading_system();

    // Load configuration files
    for (const auto& config_file : config_files) {
        if (std::filesystem::exists(config_file)) {
            spdlog::info("Loading configuration: %s", config_file.c_str());
            system->load_configuration(config_file);
        }
        else {
            spdlog::warn(
                "Configuration file not found: %s", config_file.c_str());
        }
    }

    // Load plugin configurations
    auto plugin_path = std::filesystem::path(plugin_path_str);
    if (std::filesystem::exists(plugin_path)) {
        spdlog::info("Loading plugins from: %s", plugin_path_str.c_str());
        for (auto& p : std::filesystem::directory_iterator(plugin_path)) {
            if (p.path().extension() == ".json") {
                spdlog::info(
                    "Loading plugin: %s", p.path().filename().string().c_str());
                system->load_configuration(p.path().string());
            }
        }
    }
    else {
        spdlog::warn("Plugin directory not found: %s", plugin_path_str.c_str());
    }

    // Initialize node system
    system->init();
    system->set_node_tree_executor(std::make_unique<FooExecutor>());

    // Create enhanced node editor widget with menu support
    auto node_editor = std::make_unique<NodeEditorWidget>(system, json_path);

    // Register widget
    window->register_widget(std::move(node_editor));

    // Register function to update UI periodically
    window->register_function_before_frame([system](Window* window) {
        // You can add periodic operations here if needed
    });

    std::cout << "Node editor running..." << std::endl;

    // Run the window
    window->run();

    // Cleanup
    system.reset();
    window.reset();

    return 0;
}
