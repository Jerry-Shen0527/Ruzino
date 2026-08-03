//
// Copyright Contributors to the MaterialX Project
// SPDX-License-Identifier: Apache-2.0
//

#include "ClosureCompoundNodeSlang.h"

#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/HwShaderGenerator.h>
#include <MaterialXGenShader/ShaderGenerator.h>

MATERIALX_NAMESPACE_BEGIN
ShaderNodeImplPtr ClosureCompoundNodeSlang::create()
{
    return std::make_shared<ClosureCompoundNodeSlang>();
}

void ClosureCompoundNodeSlang::addClassification(ShaderNode& node) const
{
    // Add classification from the graph implementation.
    node.addClassification(_rootGraph->getClassification());
}

void ClosureCompoundNodeSlang::emitFunctionDefinition(
    const ShaderNode& node,
    GenContext& context,
    ShaderStage& stage) const
{
    DEFINE_SHADER_STAGE(stage, Stage::PIXEL)
    {
        const ShaderGenerator& shadergen = context.getShaderGenerator();
        const Syntax& syntax = shadergen.getSyntax();

        bool isStandardSurface =
            _functionName.find("standard_surface") != string::npos;
        bool isUsdPreviewSurface =
            _functionName.find("UsdPreviewSurface") != string::npos;

        if (isStandardSurface || isUsdPreviewSurface) {
            // Emit the opacity fetch function directly from the MaterialX
            // graph (texture sampling + opacity input expression). The bindless
            // data loading placeholder ($BindlessDataLoading) is the only thing
            // filled in later by materialX.cpp; the opacity computation itself
            // is emitted here, replacing the old string-surgery approach.
            emitOpacityFetchFunctionDefinition(
                node, context, stage, isUsdPreviewSurface);
            return;
        }

        // Emit functions for all child nodes
        shadergen.emitFunctionDefinitions(*_rootGraph, context, stage);

        string delim = "";

        // Begin function signature
        shadergen.emitLineBegin(stage);

        shadergen.emitString("void " + _functionName + "(", stage);

        if (context.getShaderGenerator().nodeNeedsClosureData(node)) {
            shadergen.emitString(
                delim + HW::CLOSURE_DATA_TYPE + " " + HW::CLOSURE_DATA_ARG +
                    ", ",
                stage);
        }

        auto& vertexData = stage.getInputBlock(HW::VERTEX_DATA);
        if (!vertexData.empty()) {
            shadergen.emitString(
                delim + vertexData.getName() + " " + vertexData.getInstance(),
                stage);
            delim = ", ";
        }

        const string& type = syntax.getTypeName(Type::VECTOR3);
        shadergen.emitString(delim + type + " " + HW::DIR_L, stage);
        shadergen.emitString(", " + type + " " + HW::DIR_V, stage);
        // eta
        shadergen.emitString(delim + "uint eta_flipped", stage);

        // Add all inputs
        for (ShaderGraphInputSocket* inputSocket :
             _rootGraph->getInputSockets()) {
            shadergen.emitString(
                delim + syntax.getTypeName(inputSocket->getType()) + " " +
                    inputSocket->getVariable(),
                stage);
            delim = ", ";
        }

        // Add all outputs
        for (ShaderGraphOutputSocket* outputSocket :
             _rootGraph->getOutputSockets()) {
            shadergen.emitString(
                delim + syntax.getOutputTypeName(outputSocket->getType()) +
                    " " + outputSocket->getVariable(),
                stage);
            delim = ", ";
        }

        // End function signature
        shadergen.emitString(")", stage);
        shadergen.emitLineEnd(stage, false);

        // Begin function body
        shadergen.emitFunctionBodyBegin(*_rootGraph, context, stage);

        if (isStandardSurface) {
            shadergen.emitLine("if (transmission > 0.0)", stage, false);
            shadergen.emitLine("if (eta_flipped > 0.0)", stage, false);
            shadergen.emitLine("specular_IOR = 1.0 / specular_IOR", stage);
        }

        // Emit all texturing nodes. These are inputs to the
        // closure nodes and need to be emitted first.
        shadergen.emitFunctionCalls(
            *_rootGraph, context, stage, ShaderNode::Classification::TEXTURE);

        // Emit function calls for internal closures nodes connected to the
        // graph sockets. These will in turn emit function calls for any
        // dependent closure nodes upstream.
        for (ShaderGraphOutputSocket* outputSocket :
             _rootGraph->getOutputSockets()) {
            if (outputSocket->getConnection()) {
                const ShaderNode* upstream =
                    outputSocket->getConnection()->getNode();
                if (upstream->getParent() == _rootGraph.get() &&
                    (upstream->hasClassification(
                         ShaderNode::Classification::CLOSURE) ||
                     upstream->hasClassification(
                         ShaderNode::Classification::SHADER))) {
                    shadergen.emitFunctionCall(*upstream, context, stage);
                }
            }
        }

        // Emit final results
        for (ShaderGraphOutputSocket* outputSocket :
             _rootGraph->getOutputSockets()) {
            const string result =
                shadergen.getUpstreamResult(outputSocket, context);
            shadergen.emitLine(
                outputSocket->getVariable() + " = " + result, stage);
        }

        // End function body
        shadergen.emitFunctionBodyEnd(*_rootGraph, context, stage);
    }
}

void ClosureCompoundNodeSlang::emitOpacityFetchFunctionDefinition(
    const ShaderNode& node,
    GenContext& context,
    ShaderStage& stage,
    bool isUsdPreviewSurface) const
{
    const ShaderGenerator& shadergen = context.getShaderGenerator();
    const Syntax& syntax = shadergen.getSyntax();

    shadergen.emitLineBreak(stage);
    shadergen.emitComment(
        "Opacity fetch function - computes opacity from material graph", stage);

    // Emit function signature
    shadergen.emitLine("void fetch_shader_opacity(", stage, false);
    shadergen.emitLine("    inout uint material_params_index,", stage, false);
    shadergen.emitLine("    inout uint shader_type_id,", stage, false);
    shadergen.emitLine("    in MaterialDataBlob data,", stage, false);
    shadergen.emitLine("    in VertexInfo vertexInfo)", stage, false);
    shadergen.emitLine("{", stage, false);

    // Bindless data loading placeholder — replaced by materialX.cpp with the
    // per-material bindless data code from BindlessContext (uniform values +
    // texture id slots). This is the ONLY placeholder this function relies on;
    // the opacity value itself is emitted directly below, no string surgery.
    shadergen.emitLine("$BindlessDataLoading", stage, false);

    // Find the "opacity" input by name and emit it directly. emitInput will
    // recursively emit any upstream nodes the opacity input depends on (e.g. a
    // texture sample feeding opacity), so no separate emitFunctionCalls pass
    // is needed here. This replaces the old hack that string-parsed the
    // opacity expression out of fetch_shader_data (with a hardcoded magic
    // parameter index of 38).
    //
    // standard_surface.opacity is color3 → take its luminance.
    // UsdPreviewSurface.opacity is float   → use as-is.
    bool opacityEmitted = false;
    for (const auto& input : node.getInputs()) {
        if (input->getName() == "opacity") {
            TypeDesc type = input->getType();
            if (type == Type::COLOR3) {
                // Emit the color3 expression, then convert to luminance.
                shadergen.emitLineBegin(stage);
                shadergen.emitString("    float3 opacity_color3 = ", stage);
                shadergen.emitInput(input, context, stage);
                shadergen.emitLineEnd(stage, true);
                shadergen.emitLine(
                    "    float opacity_value = dot(opacity_color3, "
                    "float3(0.212671, 0.715160, 0.072169));",
                    stage,
                    false);
            }
            else {
                // Float (or other scalar) — use directly.
                shadergen.emitLineBegin(stage);
                shadergen.emitString("    float opacity_value = ", stage);
                shadergen.emitInput(input, context, stage);
                shadergen.emitLineEnd(stage, true);
            }
            opacityEmitted = true;
            break;
        }
    }

    if (!opacityEmitted) {
        // No opacity input on this material — fully opaque.
        shadergen.emitLine("    float opacity_value = 1.0;", stage, false);
    }

    shadergen.emitLineBreak(stage);
    shadergen.emitLine(
        "    shader_type_id = " + std::to_string(isUsdPreviewSurface ? 1 : 0) +
            ";",
        stage,
        false);
    shadergen.emitLine(
        "    material_params_index = asuint(opacity_value);", stage, false);
    shadergen.emitLine("}", stage, false);
    shadergen.emitLineBreak(stage);
}

void ClosureCompoundNodeSlang::emitFunctionCall(
    const ShaderNode& node,
    GenContext& context,
    ShaderStage& stage) const
{
    const ShaderGenerator& shadergen = context.getShaderGenerator();

    DEFINE_SHADER_STAGE(stage, Stage::VERTEX)
    {
        // Emit function calls for all child nodes to the vertex shader stage
        shadergen.emitFunctionCalls(*_rootGraph, context, stage);
    }

    DEFINE_SHADER_STAGE(stage, Stage::PIXEL)
    {
        // Emit calls for any closure dependencies upstream from this node.
        shadergen.emitDependentFunctionCalls(
            node, context, stage, ShaderNode::Classification::CLOSURE);

        //// Declare the output variables
        emitOutputVariables(node, context, stage);

        // Check if this is a standard surface material
        bool isStandardSurface =
            _functionName.find("standard_surface") != string::npos;
        bool isUsdPreviewSurface =
            _functionName.find("UsdPreviewSurface") != string::npos;

        if (isStandardSurface) {
            // Use packStandardSurfaceMaterialParams to create optimized
            // structure

            // Parameter name mapping for standard_surface - must match input
            // order
            static const std::vector<string> paramNames = {
                "base",
                "base_color",
                "diffuse_roughness",
                "metalness",
                "specular",
                "specular_color",
                "specular_roughness",
                "specular_IOR",
                "specular_anisotropy",
                "specular_rotation",
                "transmission",
                "transmission_color",
                "transmission_depth",
                "transmission_scatter",
                "transmission_scatter_anisotropy",
                "transmission_dispersion",
                "transmission_extra_roughness",
                "subsurface",
                "subsurface_color",
                "subsurface_radius",
                "subsurface_scale",
                "subsurface_anisotropy",
                "sheen",
                "sheen_color",
                "sheen_roughness",
                "coat",
                "coat_color",
                "coat_roughness",
                "coat_anisotropy",
                "coat_rotation",
                "coat_IOR",
                "coat_normal",
                "coat_affect_color",
                "coat_affect_roughness",
                "thin_film_thickness",
                "thin_film_IOR",
                "emission",
                "emission_color",
                "opacity",
                "thin_walled",
                "normal",
                "tangent"
            };

            auto inputs = node.getInputs();

            // Begin function call to packStandardSurfaceMaterialParams
            shadergen.emitLineBegin(stage);
            shadergen.emitString(
                "PackedStandardSurfaceMaterialParams params = "
                "packStandardSurfaceMaterialParams(",
                stage);
            shadergen.emitLineEnd(stage, false);

            // Emit all parameters to the packing function
            size_t minInputs = std::min(inputs.size(), paramNames.size());
            for (size_t i = 0; i < minInputs; ++i) {
                shadergen.emitLineBegin(stage);
                shadergen.emitString("    ", stage);
                shadergen.emitInput(inputs[i], context, stage);
                if (i < minInputs - 1 || inputs.size() < paramNames.size()) {
                    shadergen.emitString(",", stage);
                }
                shadergen.emitLineEnd(stage, false);
            }

            // Fill defaults for missing parameters
            if (inputs.size() < 40) {
                shadergen.emitLine("    0,  // thin_walled", stage, false);
            }
            if (inputs.size() < 41) {
                shadergen.emitLine("    vd.normalW,  // normal", stage, false);
            }
            if (inputs.size() < 42) {
                shadergen.emitLine("    vd.tangentW  // tangent", stage, false);
            }

            // Close function call
            shadergen.emitLine(");", stage);

            // Write params to buffer using reinterpret cast
            shadergen.emitLine(
                "materialParamsBuffer[material_params_index] = "
                "reinterpret<MaterialParams>(params);",
                stage);
            shadergen.emitLine("shader_type_id = 0;", stage);
        }
        else if (isUsdPreviewSurface) {
            // Fill PreviewSurfaceMaterialParams
            shadergen.emitLine(
                "PreviewSurfaceMaterialParams params = {};", stage);

            // Parameter name mapping for preview_surface
            // Order MUST match the USD Preview Surface node inputs order
            static const std::vector<string> paramNames = {
                "diffuseColor",         // 0
                "emissiveColor",        // 1
                "useSpecularWorkflow",  // 2
                "specularColor",        // 3
                "metallic",             // 4
                "roughness",            // 5
                "clearcoat",            // 6
                "clearcoatRoughness",   // 7
                "opacity",              // 8
                "opacityMode",          // 9
                "opacityThreshold",     // 10
                "ior",                  // 11
                "normal",               // 12
                "displacement",         // 13
                "occlusion"             // 14
            };

            auto inputs = node.getInputs();

            // Map inputs by NAME, not by position — getInputs() only
            // returns authored inputs whose order depends on USD dict
            // ordering, which may not match paramNames.
            for (auto& input : inputs) {
                const string& inputName = input->getName();
                for (size_t j = 0; j < paramNames.size(); ++j) {
                    if (paramNames[j] == inputName) {
                        shadergen.emitLineBegin(stage);
                        shadergen.emitString(
                            "params." + inputName + " = ", stage);
                        shadergen.emitInput(input, context, stage);
                        shadergen.emitLineEnd(stage);
                        break;
                    }
                }
            }

            // Write params to buffer using reinterpret cast
            shadergen.emitLine(
                "materialParamsBuffer[material_params_index] = "
                "reinterpret<MaterialParams>(params);",
                stage);
            shadergen.emitLine("shader_type_id = 1;", stage);
        }
        else {
        }
    }
}

MATERIALX_NAMESPACE_END
