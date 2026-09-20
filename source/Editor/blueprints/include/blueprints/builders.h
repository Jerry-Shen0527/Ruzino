//------------------------------------------------------------------------------
// LICENSE
//   This software is dual-licensed to the public domain and under the following
//   license: you are granted a perpetual, irrevocable license to copy, modify,
//   publish, and distribute this file as you see fit.
//
// CREDITS
//   Written by Michal Cichon
//------------------------------------------------------------------------------
#pragma once

#include "nodes/core/id.hpp"

//------------------------------------------------------------------------------
#include "imgui_node_editor.h"
RUZINO_NAMESPACE_OPEN_SCOPE
//------------------------------------------------------------------------------
namespace ax {
namespace NodeEditor {
    namespace Utilities {
        //------------------------------------------------------------------------------
        struct BlueprintNodeBuilder {
            BlueprintNodeBuilder(
                ImTextureID texture = 0,
                int textureWidth = 0,
                int textureHeight = 0);

            void Begin(Ruzino::NodeId id);
            void End();

            void Header(const ImVec4& color = ImVec4(1, 1, 1, 1));
            void EndHeader();

            void Input(Ruzino::SocketID id);
            void EndInput();

            void Middle();

            void Output(Ruzino::SocketID id);
            void EndOutput();

           private:
            enum class Stage {
                Invalid,
                Begin,
                Header,
                Content,
                Input,
                Output,
                Middle,
                End
            };

            bool SetStage(Stage stage);

            void Pin(Ruzino::SocketID id, ax::NodeEditor::PinKind kind);
            void EndPin();

            ImTextureID HeaderTextureId;
            int HeaderTextureWidth;
            int HeaderTextureHeight;
            Ruzino::NodeId CurrentNodeId;
            Stage CurrentStage;
            ImU32 HeaderColor;
            ImVec2 NodeMin;
            ImVec2 NodeMax;
            ImVec2 HeaderMin;
            ImVec2 HeaderMax;
            ImVec2 ContentMin;
            ImVec2 ContentMax;
            bool HasHeader;
        };

        //------------------------------------------------------------------------------
    }  // namespace Utilities
}  // namespace NodeEditor
}  // namespace ax
RUZINO_NAMESPACE_CLOSE_SCOPE
