//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//
#define IMGUI_DEFINE_MATH_OPERATORS

#include <MCore/MaterialXNodeTreeWidget.h>
#include <MaterialXFormat/Util.h>
#include <blueprints/imgui_node_editor_internal.h>
#include <imgui_stdlib.h>

#include <nodes/core/node_link.hpp>

#include "GUI/window.h"
#include "MCore/MaterialXNodeTree.hpp"
#include "spdlog/spdlog.h"
namespace mx = MaterialX;

RUZINO_NAMESPACE_OPEN_SCOPE

// Based on the dimensions of the dot_color3 node, computed by calling
// ed::getNodeSize
const ImVec2 DEFAULT_NODE_SIZE = ImVec2(138, 116);

const int DEFAULT_ALPHA = 255;
const int FILTER_ALPHA = 50;

const std::array<std::string, 22> NODE_GROUP_ORDER = {
    "texture2d",      "texture3d",    "procedural",  "procedural2d",
    "procedural3d",   "geometric",    "translation", "convolution2d",
    "math",           "adjustment",   "compositing", "conditional",
    "channel",        "organization", "global",      "application",
    "material",       "shader",       "pbr",         "light",
    "colortransform", "none"
};

// Based on ImRect_Expanded function in ImGui Node Editor blueprints-example.cpp
ImRect expandImRect(const ImRect& rect, float x, float y)
{
    ImRect result = rect;
    result.Min.x -= x;
    result.Min.y -= y;
    result.Max.x += x;
    result.Max.y += y;
    return result;
}

// Based on the splitter function in the ImGui Node Editor
// blueprints-example.cpp
static bool splitter(
    bool split_vertically,
    float thickness,
    float* size1,
    float* size2,
    float min_size1,
    float min_size2,
    float splitter_long_axis_size = -1.0f)
{
    using namespace ImGui;
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    ImGuiID id = window->GetID("##Splitter");
    ImRect bb;
    bb.Min = window->DC.CursorPos +
             (split_vertically ? ImVec2(*size1, 0.0f) : ImVec2(0.0f, *size1));
    bb.Max = bb.Min + CalcItemSize(
                          split_vertically
                              ? ImVec2(thickness, splitter_long_axis_size)
                              : ImVec2(splitter_long_axis_size, thickness),
                          0.0f,
                          0.0f);
    return SplitterBehavior(
        bb,
        id,
        split_vertically ? ImGuiAxis_X : ImGuiAxis_Y,
        size1,
        size2,
        min_size1,
        min_size2,
        0.0f);
}

// Based on showLabel from ImGui Node Editor blueprints-example.cpp
auto showLabel = [](const char* label, ImColor color) {
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetTextLineHeight());
    auto size = ImGui::CalcTextSize(label);

    auto padding = ImGui::GetStyle().FramePadding;
    auto spacing = ImGui::GetStyle().ItemSpacing;

    ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(spacing.x, -spacing.y));

    auto rectMin = ImGui::GetCursorScreenPos() - padding;
    auto rectMax = ImGui::GetCursorScreenPos() + size + padding;

    auto drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(rectMin, rectMax, color, size.y * 0.15f);
    ImGui::TextUnformatted(label);
};

bool MaterialXNodeTreeWidget::checkPosition(UiNodePtr node)
{
    // mx::ElementPtr elem = node->getElement();
    // return elem && !elem->getAttribute(mx::Element::XPOS_ATTRIBUTE).empty();
    return false;
}

// Calculate the total vertical space the node level takes up
float MaterialXNodeTreeWidget::totalHeight(int level)
{
    float total = 0.f;
    for (UiNodePtr node : _levelMap[level]) {
        total += ed::GetNodeSize(node->ID).y;
    }
    return total;
}

// Set the y-position of node based on the starting position and the nodes above
// it
void MaterialXNodeTreeWidget::setYSpacing(int level, float startingPos)
{
    // set the y spacing for each node
    float currPos = startingPos;
    for (UiNodePtr node : _levelMap[level]) {
        ImVec2 oldPos = ed::GetNodePosition(node->ID);
        ed::SetNodePosition(node->ID, ImVec2(oldPos.x, currPos));
        currPos += ed::GetNodeSize(node->ID).y + 40;
    }
}

// Calculate the average y-position for a specific node level
float MaterialXNodeTreeWidget::findAvgY(const std::vector<UiNodePtr>& nodes)
{
    // find the mid point of node level grou[
    float total = 0.f;
    int count = 0;
    for (UiNodePtr node : nodes) {
        ImVec2 pos = ed::GetNodePosition(node->ID);
        ImVec2 size = ed::GetNodeSize(node->ID);

        total += ((size.y + pos.y) + pos.y) / 2;
        count++;
    }
    return (total / count);
}

void MaterialXNodeTreeWidget::layoutInputs()
{
    // Layout inputs after other nodes so that they can be all in a line on
    // / far / left side of node graph
    if (_levelMap.begin() != _levelMap.end()) {
        int levelCount = -1;
        for (std::pair<int, std::vector<UiNodePtr>> nodes : _levelMap) {
            ++levelCount;
        }
        ImVec2 startingPos =
            ed::GetNodePosition(_levelMap[levelCount].back()->ID);
        startingPos.y +=
            ed::GetNodeSize(_levelMap[levelCount].back()->ID).y + 20;

        for (UiNodePtr uiNode : _graphNodes) {
            if (uiNode->getOutputConnections().size() == 0 &&
                (getMaterialXInput(uiNode) != nullptr)) {
                ed::SetNodePosition(uiNode->ID, ImVec2(startingPos));
                startingPos.y += ed::GetNodeSize(uiNode->ID).y;
                startingPos.y += 23;
            }
            else if (
                uiNode->getOutputConnections().size() == 0 &&
                (getMaterialXNode(uiNode) != nullptr)) {
                if (getMaterialXNode(uiNode)->getCategory() !=
                    mx::SURFACE_MATERIAL_NODE_STRING) {
                    // layoutPosition(uiNode, ImVec2(1200, 750), _initial, 0);
                }
            }
        }
    }
}

void MaterialXNodeTreeWidget::setConstant(
    UiNodePtr node,
    mx::InputPtr& input,
    const mx::UIProperties& uiProperties)
{
    auto mtlx_tree = static_cast<MaterialXNodeTree*>(tree_);

    ImGui::PushItemWidth(-1);

    mx::ValuePtr minVal = uiProperties.uiMin;
    mx::ValuePtr maxVal = uiProperties.uiMax;

    // If input is a float set the float slider UI to the value
    if (input->getType() == "float") {
        mx::ValuePtr val = input->getValue();

        if (val && val->isA<float>()) {
            // Update the value to the default for new nodes
            float prev, temp;
            prev = temp = val->asA<float>();
            float min = minVal ? minVal->asA<float>() : 0.f;
            float max = maxVal ? maxVal->asA<float>() : 100.f;
            float speed = (max - min) / 1000.0f;
            ImGui::DragFloat("##hidelabel", &temp, speed, min, max);

            // Set input value and update materials if different from previous
            // value
            if (prev != temp) {
                mtlx_tree->addNodeInput(_currUiNode, input);
                input->setValue(temp, input->getType());
            }
        }
    }
    else if (input->getType() == "integer") {
        mx::ValuePtr val = input->getValue();
        if (val && val->isA<int>()) {
            int prev, temp;
            prev = temp = val->asA<int>();
            int min = minVal ? minVal->asA<int>() : 0;
            int max = maxVal ? maxVal->asA<int>() : 100;
            float speed = (max - min) / 100.0f;
            ImGui::DragInt("##hidelabel", &temp, speed, min, max);

            // Set input value and update materials if different from previous
            // value
            if (prev != temp) {
                mtlx_tree->addNodeInput(_currUiNode, input);
                input->setValue(temp, input->getType());
            }
        }
    }
    else if (input->getType() == "color3") {
        mx::ValuePtr val = input->getValue();
        if (val && val->isA<mx::Color3>()) {
            mx::Color3 prev, temp;
            prev = temp = val->asA<mx::Color3>();
            float min = minVal ? minVal->asA<mx::Color3>()[0] : 0.f;
            float max = maxVal ? maxVal->asA<mx::Color3>()[0] : 100.f;
            float speed = (max - min) / 1000.0f;
            ImGui::PushItemWidth(-100);
            ImGui::DragFloat3("##hidelabel", &temp[0], speed, min, max);
            ImGui::PopItemWidth();
            ImGui::SameLine();
            ImGui::ColorEdit3(
                "##color", &temp[0], ImGuiColorEditFlags_NoInputs);

            // Set input value and update materials if different from previous
            // value
            if (prev != temp) {
                mtlx_tree->addNodeInput(_currUiNode, input);
                input->setValue(temp, input->getType());
            }
        }
    }
    else if (input->getType() == "color4") {
        mx::ValuePtr val = input->getValue();
        if (val && val->isA<mx::Color4>()) {
            mx::Color4 prev, temp;
            prev = temp = val->asA<mx::Color4>();
            float min = minVal ? minVal->asA<mx::Color4>()[0] : 0.f;
            float max = maxVal ? maxVal->asA<mx::Color4>()[0] : 100.f;
            float speed = (max - min) / 1000.0f;
            ImGui::PushItemWidth(-100);
            ImGui::DragFloat4("##hidelabel", &temp[0], speed, min, max);
            ImGui::PopItemWidth();
            ImGui::SameLine();

            // Color edit for the color picker to the right of the color floats
            ImGui::ColorEdit4(
                "##color", &temp[0], ImGuiColorEditFlags_NoInputs);

            // Set input value and update materials if different from previous
            // value
            if (temp != prev) {
                mtlx_tree->addNodeInput(_currUiNode, input);
                input->setValue(temp, input->getType());
            }
        }
    }
    else if (input->getType() == "vector2") {
        mx::ValuePtr val = input->getValue();
        if (val && val->isA<mx::Vector2>()) {
            mx::Vector2 prev, temp;
            prev = temp = val->asA<mx::Vector2>();
            float min = minVal ? minVal->asA<mx::Vector2>()[0] : 0.f;
            float max = maxVal ? maxVal->asA<mx::Vector2>()[0] : 100.f;
            float speed = (max - min) / 1000.0f;
            ImGui::DragFloat2("##hidelabel", &temp[0], speed, min, max);

            // Set input value and update materials if different from previous
            // value
            if (prev != temp) {
                mtlx_tree->addNodeInput(_currUiNode, input);
                input->setValue(temp, input->getType());
            }
        }
    }
    else if (input->getType() == "vector3") {
        mx::ValuePtr val = input->getValue();
        if (val && val->isA<mx::Vector3>()) {
            mx::Vector3 prev, temp;
            prev = temp = val->asA<mx::Vector3>();
            float min = minVal ? minVal->asA<mx::Vector3>()[0] : 0.f;
            float max = maxVal ? maxVal->asA<mx::Vector3>()[0] : 100.f;
            float speed = (max - min) / 1000.0f;
            ImGui::DragFloat3("##hidelabel", &temp[0], speed, min, max);

            // Set input value and update materials if different from previous
            // value
            if (prev != temp) {
                mtlx_tree->addNodeInput(_currUiNode, input);
                input->setValue(temp, input->getType());
            }
        }
    }
    else if (input->getType() == "vector4") {
        mx::ValuePtr val = input->getValue();
        if (val && val->isA<mx::Vector4>()) {
            mx::Vector4 prev, temp;
            prev = temp = val->asA<mx::Vector4>();
            float min = minVal ? minVal->asA<mx::Vector4>()[0] : 0.f;
            float max = maxVal ? maxVal->asA<mx::Vector4>()[0] : 100.f;
            float speed = (max - min) / 1000.0f;
            ImGui::DragFloat4("##hidelabel", &temp[0], speed, min, max);

            // Set input value and update materials if different from previous
            // value
            if (prev != temp) {
                mtlx_tree->addNodeInput(_currUiNode, input);
                input->setValue(temp, input->getType());
            }
        }
    }
    else if (input->getType() == "string") {
        mx::ValuePtr val = input->getValue();
        if (val && val->isA<std::string>()) {
            std::string prev, temp;
            prev = temp = val->asA<std::string>();
            ImGui::InputText("##constant", &temp);

            // Set input value and update materials if different from previous
            // value
            if (prev != temp) {
                mtlx_tree->addNodeInput(_currUiNode, input);
                input->setValue(temp, input->getType());
            }
        }
    }
    else if (input->getType() == "filename") {
        mx::ValuePtr val = input->getValue();

        if (val && val->isA<std::string>()) {
            std::string prev, temp;
            prev = temp = val->asA<std::string>();
            ImGui::PushStyleColor(
                ImGuiCol_Button, ImVec4(.15f, .15f, .15f, 1.0f));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonHovered, ImVec4(.2f, .4f, .6f, 1.0f));

            // Browser button to select new file
            ImGui::PushItemWidth(-100);
            if (ImGui::Button("Browse")) {
                const std::string uniqueId =
                    "imageFileBrowser_" + input->getName();
                IGFD::FileDialogConfig config;
                config.path = mx::FilePath(temp).getParentPath().asString();
                config.countSelectionMax = 1;
                config.flags = ImGuiFileDialogFlags_Modal;

                ImGuiFileDialog::Instance()->OpenDialog(
                    uniqueId,
                    "Select Image File",
                    _imageFilter.empty()
                        ? nullptr
                        : mx::joinStrings(_imageFilter, ",").c_str(),
                    config);
            }
            ImGui::PopItemWidth();
            ImGui::SameLine();
            ImGui::Text("%s", mx::FilePath(temp).getBaseName().c_str());
            ImGui::PopStyleColor();
            ImGui::PopStyleColor();

            // Process file dialog result
            const std::string uniqueId = "imageFileBrowser_" + input->getName();
            if (ImGuiFileDialog::Instance()->Display(uniqueId)) {
                if (ImGuiFileDialog::Instance()->IsOk()) {
                    // Get the selected filename
                    std::string filePath =
                        ImGuiFileDialog::Instance()->GetFilePathName();
                    temp = filePath;

                    // Update the input value
                    mtlx_tree->addNodeInput(_currUiNode, input);
                    input->setFilePrefix(mx::EMPTY_STRING);
                    input->setValueString(temp);
                }

                // Close the dialog
                ImGuiFileDialog::Instance()->Close();
            }

            // Set input value and update materials if different from previous
            // value
            if (prev != temp) {
                mtlx_tree->addNodeInput(_currUiNode, input);
                input->setValueString(temp);
                input->setValue(temp, input->getType());
            }
        }
    }
    else if (input->getType() == "boolean") {
        mx::ValuePtr val = input->getValue();
        if (val && val->isA<bool>()) {
            bool prev, temp;
            prev = temp = val->asA<bool>();
            ImGui::Checkbox("", &temp);

            // Set input value and update materials if different from previous
            // value
            if (prev != temp) {
                mtlx_tree->addNodeInput(_currUiNode, input);
                input->setValue(temp, input->getType());
            }
        }
    }

    ImGui::PopItemWidth();
}

void MaterialXNodeTreeWidget::createNodeUIList(mx::DocumentPtr doc)
{
    _nodesToAdd.clear();

    auto nodeDefs = doc->getNodeDefs();
    std::unordered_map<std::string, std::vector<mx::NodeDefPtr>> groupToNodeDef;
    std::vector<std::string> groupList =
        std::vector(NODE_GROUP_ORDER.begin(), NODE_GROUP_ORDER.end());

    for (const auto& nodeDef : nodeDefs) {
        std::string group = nodeDef->getNodeGroup();
        if (group.empty()) {
            group = NODE_GROUP_ORDER.back();
        }

        // If the group is not in the groupList already (seeded by
        // NODE_GROUP_ORDER) then add it.
        if (std::find(groupList.begin(), groupList.end(), group) ==
            groupList.end()) {
            groupList.emplace_back(group);
        }

        if (groupToNodeDef.find(group) == groupToNodeDef.end()) {
            groupToNodeDef[group] = std::vector<mx::NodeDefPtr>();
        }
        groupToNodeDef[group].push_back(nodeDef);
    }

    for (const auto& group : groupList) {
        auto it = groupToNodeDef.find(group);
        if (it != groupToNodeDef.end()) {
            const auto& groupNodeDefs = it->second;

            for (const auto& nodeDef : groupNodeDefs) {
                _nodesToAdd.emplace_back(
                    nodeDef->getName(),
                    nodeDef->getType(),
                    nodeDef->getNodeString(),
                    group);
            }
        }
    }

    // addExtraNodes();
}

void MaterialXNodeTreeWidget::positionPasteBin(ImVec2 pos)
{
    ImVec2 totalPos = ImVec2(0, 0);
    ImVec2 avgPos = ImVec2(0, 0);

    // Get average position of original nodes
    for (auto pasteNode : _copiedNodes) {
        ImVec2 origPos = ed::GetNodePosition(pasteNode.first->ID);
        totalPos.x += origPos.x;
        totalPos.y += origPos.y;
    }
    avgPos.x = totalPos.x / (int)_copiedNodes.size();
    avgPos.y = totalPos.y / (int)_copiedNodes.size();

    // Get offset from clicked position
    ImVec2 offset = ImVec2(0, 0);
    offset.x = pos.x - avgPos.x;
    offset.y = pos.y - avgPos.y;
    for (auto pasteNode : _copiedNodes) {
        if (!pasteNode.second) {
            continue;
        }
        ImVec2 newPos = ImVec2(0, 0);
        newPos.x = ed::GetNodePosition(pasteNode.first->ID).x + offset.x;
        newPos.y = ed::GetNodePosition(pasteNode.first->ID).y + offset.y;
        ed::SetNodePosition(pasteNode.second->ID, newPos);
    }
}

void MaterialXNodeTreeWidget::graphButtons()
{
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.15f, .15f, .15f, 1.0f));
    ImGui::SetWindowFontScale(_fontScale);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            // Buttons for loading and saving a .mtlx
            if (ImGui::MenuItem("New", "Ctrl-N")) {
                // clearGraph();
            }
            else if (ImGui::MenuItem("Open", "Ctrl-O")) {
                // loadGraphFromFile(true);
            }
            else if (ImGui::MenuItem("Reload", "Ctrl-R")) {
                // loadGraphFromFile(false);
            }
            else if (ImGui::MenuItem("Save", "Ctrl-S")) {
                // saveGraphToFile();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Graph")) {
            if (ImGui::MenuItem("Auto Layout")) {
                _autoLayout = true;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Viewer")) {
            if (ImGui::MenuItem("Load Geometry")) {
                // loadGeometry();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Options")) {
            ImGui::Checkbox("Save Node Positions", &_saveNodePositions);
            ImGui::EndMenu();
        }

        if (ImGui::Button("Help")) {
            ImGui::OpenPopup("Help");
        }
        if (ImGui::BeginPopup("Help")) {
            showHelp();
            ImGui::EndPopup();
        }

        ImGui::EndMenuBar();
    }

    // Menu keys
    // ImGuiIO& guiIO = ImGui::GetIO();
    // if (guiIO.KeyCtrl && !_fileDialogSave.isOpened() &&
    //     !_fileDialog.isOpened() && !_fileDialogGeom.isOpened()) {
    //     if (ImGui::IsKeyReleased(ImGuiKey_O)) {
    //         loadGraphFromFile(true);
    //     }
    //     else if (ImGui::IsKeyReleased(ImGuiKey_N)) {
    //         clearGraph();
    //     }
    //     else if (ImGui::IsKeyReleased(ImGuiKey_R)) {
    //         loadGraphFromFile(false);
    //     }
    //     else if (ImGui::IsKeyReleased(ImGuiKey_S)) {
    //         saveGraphToFile();
    //     }
    // }

    // Split window into panes for NodeEditor
    static float leftPaneWidth = 375.0f;
    static float rightPaneWidth = 750.0f;
    splitter(true, 4.0f, &leftPaneWidth, &rightPaneWidth, 20.0f, 20.0f);

    // Create back button and graph hierarchy name display
    ImGui::Indent(leftPaneWidth + 15.f);
    if (ImGui::Button("<")) {
        // upNodeGraph();
    }
    ImGui::SameLine();
    if (!_currGraphName.empty()) {
        for (std::string name : _currGraphName) {
            ImGui::Text("%s", name.c_str());
            ImGui::SameLine();
            if (name != _currGraphName.back()) {
                ImGui::Text(">");
                ImGui::SameLine();
            }
        }
    }
    ImVec2 windowPos2 = ImGui::GetWindowPos();
    ImGui::Unindent(leftPaneWidth + 15.f);
    ImGui::PopStyleColor();
    ImGui::NewLine();

    // Create two windows using splitter
    float paneWidth = (leftPaneWidth - 2.0f);
}

void MaterialXNodeTreeWidget::showHelp() const
{
    ImGui::Text("MATERIALX GRAPH EDITOR HELP");
    if (ImGui::CollapsingHeader("Graph")) {
        if (ImGui::TreeNode("Navigation")) {
            ImGui::BulletText("F : Frame selected nodes in graph.");
            ImGui::BulletText("RIGHT MOUSE button to pan.");
            ImGui::BulletText("SCROLL WHEEL to zoom.");
            ImGui::BulletText("\"<\" BUTTON to view parent of current graph");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Editing")) {
            ImGui::BulletText("TAB : Show popup menu to add new nodes.");
            ImGui::BulletText("CTRL-C : Copy selected nodes to clipboard.");
            ImGui::BulletText("CTRL-V : Paste clipboard to graph.");
            ImGui::BulletText("CTRL-F : Find a node by name.");
            ImGui::BulletText(
                "CTRL-X : Delete selected nodes and add to clipboard.");
            ImGui::BulletText("DELETE : Delete selected nodes or connections.");
            ImGui::TreePop();
        }
    }
    if (ImGui::CollapsingHeader("Viewer")) {
        ImGui::BulletText("LEFT MOUSE button to tumble.");
        ImGui::BulletText("RIGHT MOUSE button to pan.");
        ImGui::BulletText("SCROLL WHEEL to zoom.");
        ImGui::BulletText("Keypad +/- to zoom in fixed increments");
    }

    if (ImGui::CollapsingHeader("Property Editor")) {
        ImGui::BulletText("UP/DOWN ARROW to move between inputs.");
        ImGui::BulletText(
            "LEFT-MOUSE DRAG to modify values while entry field is in focus.");
        ImGui::BulletText(
            "DBL_CLICK or CTRL+CLICK LEFT-MOUSE on entry field to input "
            "values.");
        ImGui::Separator();
        ImGui::BulletText(
            "\"Show all inputs\" Will toggle between showing all inputs and\n "
            "only those that have been modified.");
        ImGui::BulletText(
            "\"Node Info\" Will toggle showing node information.");
    }
}

void MaterialXNodeTreeWidget::addNodePopup(bool cursor)
{
    bool open_AddPopup = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootWindow) &&
                         ImGui::IsKeyReleased(ImGuiKey_Tab);
    static char input[32]{ "" };
    if (open_AddPopup) {
        cursor = true;
        ImGui::OpenPopup("add node");
    }
    if (ImGui::BeginPopup("add node")) {
        ImGui::Text("Add Node");
        ImGui::Separator();
        if (cursor) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputText("##input", input, sizeof(input));
        std::string subs(input);

        // Input string length
        // Filter extra nodes - includes inputs, outputs, groups, and node
        // graphs
        const std::string NODEGRAPH_ENTRY = "Node Graph";

        auto mtlx_tree = static_cast<MaterialXNodeTree*>(tree_);
        // Filter nodedefs and add to menu if matches filter
        for (auto node : _nodesToAdd) {
            // Filter out list of nodes
            if (subs.size() > 0) {
                ImGui::SetNextWindowSizeConstraints(
                    ImVec2(250.0f, 300.0f), ImVec2(-1.0f, 500.0f));
                std::string str(node.getName());
                std::string nodeName = node.getName();

                // Disallow creating nested nodegraphs
                if (_isNodeGraph && node.getGroup() == NODEGRAPH_ENTRY) {
                    continue;
                }

                // Allow spaces to be used to search for node names
                std::replace(subs.begin(), subs.end(), ' ', '_');

                if (str.find(subs) != std::string::npos) {
                    if (ImGui::MenuItem(getUserNodeDefName(nodeName).c_str()) ||
                        (ImGui::IsItemFocused() &&
                         ImGui::IsKeyPressed(ImGuiKey_Enter))) {
                        mtlx_tree->addNode(
                            node.getCategory(),
                            getUserNodeDefName(nodeName),
                            node.getType());
                        _addNewNode = true;
                        memset(input, '\0', sizeof(input));
                    }
                }
            }
            else {
                ImGui::SetNextWindowSizeConstraints(
                    ImVec2(100, 10), ImVec2(-1, 300));
                if (ImGui::BeginMenu(node.getGroup().c_str())) {
                    ImGui::SetWindowFontScale(_fontScale);
                    std::string name = node.getName();
                    std::string prefix = "ND_";
                    if (name.compare(0, prefix.size(), prefix) == 0 &&
                        name.compare(
                            prefix.size(),
                            std::string::npos,
                            node.getCategory()) == 0) {
                        if (ImGui::MenuItem(getUserNodeDefName(name).c_str()) ||
                            (ImGui::IsItemFocused() &&
                             ImGui::IsKeyPressed(ImGuiKey_Enter))) {
                            mtlx_tree->addNode(
                                node.getCategory(),
                                getUserNodeDefName(name),
                                node.getType());
                            _addNewNode = true;
                        }
                    }
                    else {
                        if (ImGui::BeginMenu(node.getCategory().c_str())) {
                            if (ImGui::MenuItem(
                                    getUserNodeDefName(name).c_str()) ||
                                (ImGui::IsItemFocused() &&
                                 ImGui::IsKeyPressed(ImGuiKey_Enter))) {
                                mtlx_tree->addNode(
                                    node.getCategory(),
                                    getUserNodeDefName(name),
                                    node.getType());
                                _addNewNode = true;
                            }
                            ImGui::EndMenu();
                        }
                    }

                    ImGui::EndMenu();
                }
            }
        }
        ImGui::EndPopup();
        open_AddPopup = false;
    }
}

bool MaterialXNodeTreeWidget::isPinHovered()
{
    SocketID currentPin = ed::GetHoveredPin();
    SocketID nullPin = 0;
    return currentPin != nullPin;
}

void MaterialXNodeTreeWidget::readOnlyPopup()
{
    if (_popup) {
        ImGui::SetNextWindowSize(ImVec2(200, 100));
        ImGui::OpenPopup("Read Only");
        _popup = false;
    }
    if (ImGui::BeginPopup("Read Only")) {
        ImGui::Text("This graph is Read Only");
        ImGui::EndPopup();
    }
}

void MaterialXNodeTreeWidget::initialize()
{
    ed::Config config;

    config.UserPointer = this;

    config.SaveSettings = [](const char* data,
                             size_t size,
                             ax::NodeEditor::SaveReasonFlags reason,
                             void* userPointer) -> bool {
        if (static_cast<bool>(
                reason & (NodeEditor::SaveReasonFlags::Navigation))) {
            return true;
        }
        auto ptr = static_cast<MaterialXNodeTreeWidget*>(userPointer);
        auto storage = ptr->storage_.get();

        auto ui_json = std::string(data + 1, size - 2);

        ptr->tree_->set_ui_settings(ui_json);

        std::string node_serialize = ptr->tree_->serialize();

        storage->save(node_serialize);
        return true;
    };

    config.LoadSettings = [](void* userPointer) {
        auto ptr = static_cast<MaterialXNodeTreeWidget*>(userPointer);
        auto storage = ptr->storage_.get();

        std::string data = storage->load();

        return data;
    };

    m_Editor = ed::CreateEditor(&config);
}

std::string MaterialXNodeTreeWidget::GetWindowUniqueName()
{
    return "MaterialXNodeTreeWidget##" + material_path_;
}

void MaterialXNodeTreeWidget::create_new_node(ImVec2 openPopupPosition)
{
    if (ImGui::BeginPopup("Create New Node")) {
        // Remember the position where node should be created
        static ImVec2 newNodePosition;
        static bool positionRemembered = false;

        if (!positionRemembered) {
            newNodePosition = openPopupPosition;
            positionRemembered = true;
        }

        ImGui::Text("Add Node");
        ImGui::Separator();

        static char input[32]{ "" };
        if (create_new_node_search_cursor) {
            ImGui::SetKeyboardFocusHere();
            create_new_node_search_cursor = false;
        }
        ImGui::InputText("##input", input, sizeof(input));
        std::string subs(input);

        auto mtlx_tree = static_cast<MaterialXNodeTree*>(tree_);
        const std::string NODEGRAPH_ENTRY = "Node Graph";

        bool nodeCreated = false;
        Node* createdNode = nullptr;

        // Filter nodedefs and add to menu if matches filter
        for (auto& node_item : _nodesToAdd) {
            if (subs.size() > 0) {
                ImGui::SetNextWindowSizeConstraints(
                    ImVec2(250.0f, 300.0f), ImVec2(-1.0f, 500.0f));
                std::string nodeName = node_item.getName();

                // Disallow creating nested nodegraphs
                if (_isNodeGraph && node_item.getGroup() == NODEGRAPH_ENTRY) {
                    continue;
                }

                // Allow spaces to be used to search for node names
                std::string search = subs;
                std::replace(search.begin(), search.end(), ' ', '_');

                if (nodeName.find(search) != std::string::npos) {
                    if (ImGui::MenuItem(getUserNodeDefName(nodeName).c_str()) ||
                        (ImGui::IsItemFocused() &&
                         ImGui::IsKeyPressed(ImGuiKey_Enter))) {
                        // Store node count before creation
                        size_t nodeCountBefore = tree_->nodes.size();

                        mtlx_tree->addNode(
                            node_item.getCategory(),
                            getUserNodeDefName(nodeName),
                            node_item.getType());

                        // Get the newly created node
                        if (tree_->nodes.size() > nodeCountBefore) {
                            createdNode = tree_->nodes.back().get();
                            nodeCreated = true;
                        }

                        memset(input, '\0', sizeof(input));
                        break;
                    }
                }
            }
            else {
                ImGui::SetNextWindowSizeConstraints(
                    ImVec2(100, 10), ImVec2(-1, 300));
                if (ImGui::BeginMenu(node_item.getGroup().c_str())) {
                    ImGui::SetWindowFontScale(_fontScale);
                    std::string name = node_item.getName();
                    std::string prefix = "ND_";
                    if (name.compare(0, prefix.size(), prefix) == 0 &&
                        name.compare(
                            prefix.size(),
                            std::string::npos,
                            node_item.getCategory()) == 0) {
                        if (ImGui::MenuItem(getUserNodeDefName(name).c_str()) ||
                            (ImGui::IsItemFocused() &&
                             ImGui::IsKeyPressed(ImGuiKey_Enter))) {
                            size_t nodeCountBefore = tree_->nodes.size();

                            mtlx_tree->addNode(
                                node_item.getCategory(),
                                getUserNodeDefName(name),
                                node_item.getType());

                            if (tree_->nodes.size() > nodeCountBefore) {
                                createdNode = tree_->nodes.back().get();
                                nodeCreated = true;
                            }
                        }
                    }
                    else {
                        if (ImGui::BeginMenu(node_item.getCategory().c_str())) {
                            if (ImGui::MenuItem(
                                    getUserNodeDefName(name).c_str()) ||
                                (ImGui::IsItemFocused() &&
                                 ImGui::IsKeyPressed(ImGuiKey_Enter))) {
                                size_t nodeCountBefore = tree_->nodes.size();

                                mtlx_tree->addNode(
                                    node_item.getCategory(),
                                    getUserNodeDefName(name),
                                    node_item.getType());

                                if (tree_->nodes.size() > nodeCountBefore) {
                                    createdNode = tree_->nodes.back().get();
                                    nodeCreated = true;
                                }
                            }
                            ImGui::EndMenu();
                        }
                    }

                    ImGui::EndMenu();
                }
            }

            if (nodeCreated) {
                break;
            }
        }

        // If a node was created, set its position and try to auto-connect
        if (nodeCreated && createdNode) {
            positionRemembered = false;
            createNewNode = false;
            tree_->SetDirty();

            // Need to set editor context and Resume to set node position
            ed::SetCurrentEditor(m_Editor);
            ed::Resume();
            ed::SetNodePosition(createdNode->ID, newNodePosition);
            ed::Suspend();

            // Auto-connect if there's a pin waiting to be connected
            if (auto startPin = newNodeLinkPin) {
                auto& pins = startPin->in_out == PinKind::Input
                                 ? createdNode->get_outputs()
                                 : createdNode->get_inputs();

                for (auto& pin : pins) {
                    if (tree_->can_create_link(startPin, pin)) {
                        tree_->add_link(startPin->ID, pin->ID);
                        break;
                    }
                }
            }

            newNodeLinkPin = nullptr;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
    else {
        createNewNode = false;
    }
}

MaterialXNodeTreeWidget::MaterialXNodeTreeWidget(
    const NodeWidgetSettings& desc,
    const mx::FilePath& mtlx_path,
    const std::string& material_path)
    : NodeEditorWidgetBase(desc),
      mtlx_path_(mtlx_path),
      material_path_(material_path)
{
    MaterialXNodeTreeWidget::initialize();
    auto mtlx_tree = static_cast<MaterialXNodeTree*>(tree_);
    createNodeUIList(mtlx_tree->get_mtlx_stdlib());
}

void MaterialXNodeTreeWidget::handle_suspended_popups()
{
    if (_colorPickerState.isOpen) {
        ImGui::OpenPopup("Color Picker");
        _colorPickerState.isOpen = false;
    }

    if (ImGui::BeginPopup("Color Picker")) {
        bool colorChanged = false;

        if (_colorPickerState.numComponents == 3) {
            colorChanged = ImGui::ColorPicker3(
                (_colorPickerState.socketId + "_picker").c_str(),
                _colorPickerState.color);
        }
        else if (_colorPickerState.numComponents == 4) {
            colorChanged = ImGui::ColorPicker4(
                (_colorPickerState.socketId + "_picker").c_str(),
                _colorPickerState.color,
                ImGuiColorEditFlags_AlphaBar);
        }

        if (colorChanged && _colorPickerState.socket) {
            auto mtlx_tree = static_cast<MaterialXNodeTree*>(tree_);
            mx::InputPtr mtlxInput =
                getMaterialXPinInput(_colorPickerState.socket);

            if (mtlxInput) {
                mtlx_tree->addNodeInput(
                    _colorPickerState.socket->node, mtlxInput);
                mx::NodePtr mxNode =
                    getMaterialXNode(_colorPickerState.socket->node);

                if (mxNode) {
                    mtlxInput = mxNode->getInput(mtlxInput->getName());
                    if (mtlxInput) {
                        if (_colorPickerState.numComponents == 3) {
                            mtlxInput->setValue(
                                mx::Color3(
                                    _colorPickerState.color[0],
                                    _colorPickerState.color[1],
                                    _colorPickerState.color[2]),
                                "color3");
                        }
                        else {
                            mtlxInput->setValue(
                                mx::Color4(
                                    _colorPickerState.color[0],
                                    _colorPickerState.color[1],
                                    _colorPickerState.color[2],
                                    _colorPickerState.color[3]),
                                "color4");
                        }
                        _colorPickerState.socket->storage = mtlxInput;
                        tree_->SetDirty();
                    }
                }
            }
        }

        ImGui::EndPopup();
    }
}

void MaterialXNodeTreeWidget::execute_tree(Node* node)
{
    auto mtlx_tree = static_cast<MaterialXNodeTree*>(tree_);

    // Always save MaterialX file immediately when dirty (lightweight operation)
    if (mtlx_tree->GetDirty()) {
        spdlog::debug("MaterialX tree dirty, saving document");
        mtlx_tree->saveDocument(mtlx_path_);
        mtlx_tree->SetDirty(false);

        // Mark that we have pending changes for USD
        _pendingUsdUpdate = true;
        _usdUpdateTimer = 0.0f;
    }

    // USD update logic with proper debouncing
    if (_pendingUsdUpdate) {
        // If any control is still active, reset timer
        if (_anyControlActive) {
            spdlog::debug("Control still active, resetting timer");
            _anyControlActive = false;  // Reset for next frame
            _usdUpdateTimer = 0.0f;
            return;
        }

        // Increment timer when no controls are active
        _usdUpdateTimer += ImGui::GetIO().DeltaTime;
        spdlog::trace(
            "USD update timer: {:.2f}s / {:.2f}s",
            _usdUpdateTimer,
            USD_UPDATE_DELAY);

        // Trigger USD update after delay
        if (_usdUpdateTimer >= USD_UPDATE_DELAY) {
            spdlog::info(
                "USD update triggered after debounce delay ({:.2f}s)",
                _usdUpdateTimer);
            if (window) {
                window->events().emit(
                    "materialx_graph_changed", material_path_);
            }
            _pendingUsdUpdate = false;  // Clear pending flag
            _usdUpdateTimer = 0.0f;
        }
    }

    // Reset active flag for next frame
    _anyControlActive = false;
}

void MaterialXNodeTreeWidget::addExtraNodes()
{
    auto mtlx_tree = static_cast<MaterialXNodeTree*>(tree_);

    if (!mtlx_tree->_graphDoc) {
        return;
    }

    // Get all types from the doc
    std::vector<std::string> types;
    std::vector<mx::TypeDefPtr> typeDefs = mtlx_tree->_graphDoc->getTypeDefs();
    types.reserve(typeDefs.size());
    for (auto typeDef : typeDefs) {
        types.push_back(typeDef->getName());
    }

    // Add input and output nodes for all types
    for (const std::string& type : types) {
        std::string nodeName = "ND_input_" + type;
        _nodesToAdd.emplace_back(nodeName, type, "input", "Input Nodes");
        nodeName = "ND_output_" + type;
        _nodesToAdd.emplace_back(nodeName, type, "output", "Output Nodes");
    }

    // Add group node
    _nodesToAdd.emplace_back("ND_group", "", "group", "Group Nodes");

    // Add nodegraph node
    _nodesToAdd.emplace_back("ND_nodegraph", "", "nodegraph", "Node Graph");
}

bool MaterialXNodeTreeWidget::draw_socket_controllers(NodeSocket* input)
{
    if (input->socket_group) {
        return false;
    }

    // Get MaterialX input from the socket
    mx::InputPtr mtlxInput = getMaterialXPinInput(input);
    if (!mtlxInput) {
        // Fallback to default implementation
        ImGui::TextUnformatted(input->ui_name);
        ImGui::Spring(0);
        return false;
    }

    // CRITICAL: Ensure we have the actual input pointer from the node, not a
    // stale cached pointer This is necessary because addNodeInput() may have
    // created a new input on a previous frame
    mx::NodePtr mxNode = getMaterialXNode(input->node);
    if (mxNode) {
        std::string inputName = mtlxInput->getName();
        mx::InputPtr actualInput = mxNode->getInput(inputName);
        if (actualInput) {
            mtlxInput = actualInput;
            // Update socket storage with the correct pointer
            input->storage = mtlxInput;
        }
    }

    auto mtlx_tree = static_cast<MaterialXNodeTree*>(tree_);
    bool changed = false;
    std::string type = mtlxInput->getType();
    mx::ValuePtr value = mtlxInput->getValue();

    ImGui::PushItemWidth(120.0f);
    std::string widgetId = "##" + std::to_string(input->ID.Get());

    if (type == "float") {
        if (value && value->isA<float>()) {
            float temp = value->asA<float>();
            float min = 0.0f;
            float max = 1.0f;
            float speed = 0.001f;

            if (ImGui::DragFloat(widgetId.c_str(), &temp, speed, min, max)) {
                mtlx_tree->addNodeInput(input->node, mtlxInput);
                // Re-fetch pointer in case addNodeInput created a new one
                mx::NodePtr mxNode = getMaterialXNode(input->node);
                if (mxNode) {
                    mtlxInput = mxNode->getInput(mtlxInput->getName());
                    if (mtlxInput) {
                        mtlxInput->setValue(temp, type);
                        input->storage = mtlxInput;  // Update socket storage
                        changed = true;
                    }
                }
            }
            ImGui::Spring(0);
            ImGui::TextUnformatted(input->ui_name);
        }
    }
    else if (type == "integer") {
        if (value && value->isA<int>()) {
            int temp = value->asA<int>();
            int min = 0;
            int max = 100;
            float speed = 1.0f;

            if (ImGui::DragInt(widgetId.c_str(), &temp, speed, min, max)) {
                mtlx_tree->addNodeInput(input->node, mtlxInput);
                mx::NodePtr mxNode = getMaterialXNode(input->node);
                if (mxNode) {
                    mtlxInput = mxNode->getInput(mtlxInput->getName());
                    if (mtlxInput) {
                        mtlxInput->setValue(temp, type);
                        input->storage = mtlxInput;
                        changed = true;
                    }
                }
            }
            ImGui::Spring(0);
            ImGui::TextUnformatted(input->ui_name);
        }
    }
    else if (type == "color3" || type == "color4") {
        if (value && value->isA<mx::Color3>()) {
            mx::Color3 color = value->asA<mx::Color3>();
            float colorArray[3] = { color[0], color[1], color[2] };

            std::string colorId = widgetId + "_color";

            // Display color button
            if (ImGui::ColorButton(
                    colorId.c_str(),
                    ImVec4(colorArray[0], colorArray[1], colorArray[2], 1.0f),
                    ImGuiColorEditFlags_NoTooltip,
                    ImVec2(120, 20))) {
                // Store state to open picker in suspended context
                _colorPickerState.isOpen = true;
                _colorPickerState.socketId = colorId;
                _colorPickerState.color[0] = colorArray[0];
                _colorPickerState.color[1] = colorArray[1];
                _colorPickerState.color[2] = colorArray[2];
                _colorPickerState.numComponents = 3;
                _colorPickerState.socket = input;
            }
            ImGui::Spring(0);
            ImGui::TextUnformatted(input->ui_name);
        }
        else if (value && value->isA<mx::Color4>()) {
            mx::Color4 color = value->asA<mx::Color4>();
            float colorArray[4] = { color[0], color[1], color[2], color[3] };

            std::string colorId = widgetId + "_color";

            // Display color button
            if (ImGui::ColorButton(
                    colorId.c_str(),
                    ImVec4(
                        colorArray[0],
                        colorArray[1],
                        colorArray[2],
                        colorArray[3]),
                    ImGuiColorEditFlags_NoTooltip |
                        ImGuiColorEditFlags_AlphaPreview,
                    ImVec2(120, 20))) {
                // Store state to open picker in suspended context
                _colorPickerState.isOpen = true;
                _colorPickerState.socketId = colorId;
                _colorPickerState.color[0] = colorArray[0];
                _colorPickerState.color[1] = colorArray[1];
                _colorPickerState.color[2] = colorArray[2];
                _colorPickerState.color[3] = colorArray[3];
                _colorPickerState.numComponents = 4;
                _colorPickerState.socket = input;
            }
            ImGui::Spring(0);
            ImGui::TextUnformatted(input->ui_name);
        }
    }
    else if (type == "vector2") {
        if (value && value->isA<mx::Vector2>()) {
            mx::Vector2 vec = value->asA<mx::Vector2>();
            float vecArray[2] = { vec[0], vec[1] };

            if (ImGui::DragFloat2(
                    widgetId.c_str(), vecArray, 0.01f, 0.0f, 1.0f)) {
                mtlx_tree->addNodeInput(input->node, mtlxInput);
                mx::NodePtr mxNode = getMaterialXNode(input->node);
                if (mxNode) {
                    mtlxInput = mxNode->getInput(mtlxInput->getName());
                    if (mtlxInput) {
                        mtlxInput->setValue(
                            mx::Vector2(vecArray[0], vecArray[1]), type);
                        input->storage = mtlxInput;
                        changed = true;
                    }
                }
            }
            ImGui::Spring(0);
            ImGui::TextUnformatted(input->ui_name);
        }
    }
    else if (type == "vector3") {
        if (value && value->isA<mx::Vector3>()) {
            mx::Vector3 vec = value->asA<mx::Vector3>();
            float vecArray[3] = { vec[0], vec[1], vec[2] };

            if (ImGui::DragFloat3(
                    widgetId.c_str(), vecArray, 0.01f, 0.0f, 1.0f)) {
                mtlx_tree->addNodeInput(input->node, mtlxInput);
                mx::NodePtr mxNode = getMaterialXNode(input->node);
                if (mxNode) {
                    mtlxInput = mxNode->getInput(mtlxInput->getName());
                    if (mtlxInput) {
                        mtlxInput->setValue(
                            mx::Vector3(vecArray[0], vecArray[1], vecArray[2]),
                            type);
                        input->storage = mtlxInput;
                        changed = true;
                    }
                }
            }
            ImGui::Spring(0);
            ImGui::TextUnformatted(input->ui_name);
        }
    }
    else if (type == "vector4") {
        if (value && value->isA<mx::Vector4>()) {
            mx::Vector4 vec = value->asA<mx::Vector4>();
            float vecArray[4] = { vec[0], vec[1], vec[2], vec[3] };

            if (ImGui::DragFloat4(
                    widgetId.c_str(), vecArray, 0.01f, 0.0f, 1.0f)) {
                mtlx_tree->addNodeInput(input->node, mtlxInput);
                mx::NodePtr mxNode = getMaterialXNode(input->node);
                if (mxNode) {
                    mtlxInput = mxNode->getInput(mtlxInput->getName());
                    if (mtlxInput) {
                        mtlxInput->setValue(
                            mx::Vector4(
                                vecArray[0],
                                vecArray[1],
                                vecArray[2],
                                vecArray[3]),
                            type);
                        input->storage = mtlxInput;
                        changed = true;
                    }
                }
            }
            ImGui::Spring(0);
            ImGui::TextUnformatted(input->ui_name);
        }
    }
    else if (type == "string" || type == "filename") {
        std::string str = value ? value->getValueString() : "";
        char buffer[256];
        strncpy(buffer, str.c_str(), sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';

        if (ImGui::InputText(widgetId.c_str(), buffer, sizeof(buffer))) {
            mtlx_tree->addNodeInput(input->node, mtlxInput);
            mx::NodePtr mxNode = getMaterialXNode(input->node);
            if (mxNode) {
                mtlxInput = mxNode->getInput(mtlxInput->getName());
                if (mtlxInput) {
                    mtlxInput->setValueString(buffer);
                    input->storage = mtlxInput;
                    changed = true;
                }
            }
        }
        ImGui::Spring(0);
        ImGui::TextUnformatted(input->ui_name);
    }
    else if (type == "boolean") {
        if (value && value->isA<bool>()) {
            bool temp = value->asA<bool>();

            if (ImGui::Checkbox(widgetId.c_str(), &temp)) {
                mtlx_tree->addNodeInput(input->node, mtlxInput);
                mx::NodePtr mxNode = getMaterialXNode(input->node);
                if (mxNode) {
                    mtlxInput = mxNode->getInput(mtlxInput->getName());
                    if (mtlxInput) {
                        mtlxInput->setValue(temp, type);
                        input->storage = mtlxInput;
                        changed = true;
                    }
                }
            }
            ImGui::Spring(0);
            ImGui::TextUnformatted(input->ui_name);
        }
    }
    else {
        // Unknown type
        ImGui::TextUnformatted(input->ui_name);
        ImGui::Spring(0);
    }

    ImGui::PopItemWidth();

    if (changed) {
        tree_->SetDirty();
    }

    // Track if this control is being actively edited (mouse button held down)
    if (ImGui::IsItemActive()) {
        _anyControlActive = true;
        spdlog::trace("Control active detected");
    }

    return changed;
}

RUZINO_NAMESPACE_CLOSE_SCOPE
