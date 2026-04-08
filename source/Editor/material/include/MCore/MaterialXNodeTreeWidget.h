//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#ifndef MATERIALX_GRAPH_H
#define MATERIALX_GRAPH_H
#include <MaterialXCore/Document.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXRender/Image.h>
#include <MaterialXRender/ImageHandler.h>
#include <MaterialXRender/Util.h>
#include <blueprints/imgui_node_editor.h>

#include <stack>

#include "GUI/ImGuiFileDialog.h"
#include "MCore/api.h"
#include "nodes/core/node.hpp"
#include "nodes/ui/node_editor_widget_base.hpp"

RUZINO_NAMESPACE_OPEN_SCOPE
namespace ed = ax::NodeEditor;

namespace mx = MaterialX;

using UiNodePtr = Node*;
using UiPinPtr = NodeSocket*;
using UiEdge = NodeLink;
using Link = NodeLink;

class MenuItem {
   public:
    MenuItem(
        const std::string& name,
        const std::string& type,
        const std::string& category,
        const std::string& group)
        : name(name),
          type(type),
          category(category),
          group(group)
    {
    }

    // getters
    std::string getName() const
    {
        return name;
    }
    std::string getType() const
    {
        return type;
    }
    std::string getCategory() const
    {
        return category;
    }
    std::string getGroup() const
    {
        return group;
    }

    // setters
    void setName(const std::string& newName)
    {
        this->name = newName;
    }
    void setType(const std::string& newType)
    {
        this->type = newType;
    }
    void setCategory(const std::string& newCategory)
    {
        this->category = newCategory;
    }
    void setGroup(const std::string& newGroup)
    {
        this->group = newGroup;
    }

   private:
    std::string name;
    std::string type;
    std::string category;
    std::string group;
};

class MCORE_API MaterialXNodeTreeWidget : public NodeEditorWidgetBase {
   protected:
    void initialize() override;

    std::string GetWindowUniqueName() override;

    void create_new_node(ImVec2 openPopupPosition) override;

    void handle_suspended_popups() override;

   public:
    MaterialXNodeTreeWidget(
        const NodeWidgetSettings& desc,
        const mx::FilePath& mtlx_path,
        const std::string& material_path = "");
    void drawGraph();
    bool GetDirty() const
    {
        return tree_->GetDirty();
    }

    void SetDirty(bool dirty)
    {
        tree_->SetDirty(dirty);
    }

    void setFontScale(float val)
    {
        _fontScale = val;
    }

    using UiNode = Node;

    ~MaterialXNodeTreeWidget() { };

   private:
    // Generate node UI from nodedefs
    void createNodeUIList(mx::DocumentPtr doc);

    // Based on the comment node in the ImGui Node Editor
    // blueprints-example.cpp.
    void buildGroupNode(UiNodePtr node);

    // Connect links via connected nodes in UiNodePtr
    void linkGraph();

    // Find link position in current links vector from link id
    int findLinkPosition(int id);

    // Check if link exists in the current link vector
    bool linkExists(Link newLink);

    // Add link to nodegraph and set up connections between UiNodes and
    // MaterialX Nodes to update shader
    // startPinId - where the link was initiated
    // endPinId - where the link was ended
    // void addLink(SocketID startPinId, SocketID endPinId);

    // Extra layout pass for inputs and nodes that do not attach to an output
    // node
    void layoutInputs();

    void findYSpacing(float startPos);
    float totalHeight(int level);
    void setYSpacing(int level, float startingPos);
    float findAvgY(const std::vector<UiNodePtr>& nodes);

    UiPinPtr getPin(SocketID id);

    std::vector<int> createNodes(bool nodegraph);
    int getNodeId(SocketID pinId);

    void createEdge(
        UiNodePtr upNode,
        UiNodePtr downNode,
        mx::InputPtr connectingInput);

    // Set position attributes for nodes which changed position
    void savePosition();

    // Check if node has already been assigned a position
    bool checkPosition(UiNodePtr node);

    // Set the value of the selected node constants in the node property editor
    void setConstant(
        UiNodePtr node,
        mx::InputPtr& input,
        const mx::UIProperties& uiProperties);

    void propertyEditor();

    void copyInputs();

    // Set position of pasted nodes based on original node position
    void positionPasteBin(ImVec2 pos);

    void copyNodeGraph(UiNodePtr origGraph, UiNodePtr copyGraph);
    void copyUiNode(UiNodePtr node);

    void graphButtons();

    void addNodePopup(bool cursor);
    void searchNodePopup(bool cursor);
    bool isPinHovered();
    void addPinPopup();
    bool readOnly();
    void readOnlyPopup();

    // Compiling shaders message
    void shaderPopup();

    void showHelp() const;

    void execute_tree(Node* node) override;

    bool draw_socket_controllers(NodeSocket* input) override;

   public:
    // bool BuildUI() override
    //{
    //     drawGraph();
    //     return true;
    // }

   private:
    mx::StringVec _mtlxFilter;
    mx::StringVec _imageFilter;

    // RenderViewPtr _renderer;

    // image information
    mx::ImagePtr _image;
    mx::ImageHandlerPtr _imageHandler;

    // containers of node information
    std::vector<UiNodePtr> _graphNodes;
    std::vector<UiPinPtr> _currPins;
    std::vector<Link> _currLinks;
    std::vector<UiEdge> _currEdge;
    std::unordered_map<UiNodePtr, std::vector<UiPinPtr>> _downstreamInputs;

    // current nodes and nodegraphs
    UiNodePtr _currUiNode;
    UiNodePtr _prevUiNode;
    UiNodePtr _currRenderNode;
    std::vector<std::string> _currGraphName;
    mx::FilePath mtlx_path_;
    std::string material_path_;

    // Debounce mechanism for USD updates
    float _usdUpdateTimer = 0.0f;
    bool _pendingUsdUpdate = false;  // Track if we need to update USD
    bool _anyControlActive =
        false;  // Track if any ImGui control is being edited
    static constexpr float USD_UPDATE_DELAY =
        0.3f;  // Wait 0.3s after last change

    // Color picker popup state
    struct ColorPickerState {
        bool isOpen = false;
        std::string socketId;
        float color[4] = { 0, 0, 0, 0 };
        int numComponents = 3;
        NodeSocket* socket = nullptr;
    };
    ColorPickerState _colorPickerState;

    void addExtraNodes();

    // for adding new nodes
    std::vector<MenuItem> _nodesToAdd;

    // stacks to dive into and out of node graphs
    std::stack<std::vector<UiNodePtr>> _graphStack;
    std::stack<std::vector<UiPinPtr>> _pinStack;
    // this stack keeps track of the graph total size
    std::stack<int> _sizeStack;

    // map to group and layout nodes
    std::unordered_map<int, std::vector<UiNodePtr>> _levelMap;

    // map for copied nodes
    std::map<UiNodePtr, UiNodePtr> _copiedNodes;

    bool _initial;
    bool _delete;

    // file dialog information
    IGFD::FileDialog _fileDialog;
    IGFD::FileDialog _fileDialogSave;
    IGFD::FileDialog _fileDialogImage;
    IGFD::FileDialog _fileDialogGeom;
    std::string _fileDialogImageInputName;

    bool _isNodeGraph;

    int _graphTotalSize;

    // popup up variables
    bool _popup;
    bool _shaderPopup;
    int _searchNodeId;
    bool _addNewNode;
    bool _ctrlClick;
    bool _isCut;
    // auto layout button clicked
    bool _autoLayout;

    // used when updating materials
    int _frameCount;
    // used for filtering pins when connecting links
    std::string _pinFilterType;

    // DPI scaling for fonts
    float _fontScale = 1.0f;

    // Options
    bool _saveNodePositions;
};

RUZINO_NAMESPACE_CLOSE_SCOPE

#endif
