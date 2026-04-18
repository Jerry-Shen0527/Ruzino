//
// Slang-specific overrides for MaterialX Hw nodes.
// Uses getSyntax().getTypeName(Type::VECTOR4) instead of hardcoded "vec4".
//

#include "HwNodesSlang.h"

#include <MaterialXGenShader/Shader.h>

MATERIALX_NAMESPACE_BEGIN

// Helper: get the 4-component vector type name from the Syntax system.
static string getVec4Name(GenContext& context)
{
    return context.getShaderGenerator().getSyntax().getTypeName(Type::VECTOR4);
}

//
// HwNormalNodeSlang
//

void HwNormalNodeSlang::emitFunctionCall(const ShaderNode& node, GenContext& context, ShaderStage& stage) const
{
    const HwShaderGenerator& shadergen = static_cast<const HwShaderGenerator&>(context.getShaderGenerator());
    const string v4 = getVec4Name(context);

    const ShaderInput* spaceInput = node.getInput(SPACE);
    const int space = spaceInput ? spaceInput->getValue()->asA<int>() : OBJECT_SPACE;

    DEFINE_SHADER_STAGE(stage, Stage::VERTEX)
    {
        VariableBlock& vertexData = stage.getOutputBlock(HW::VERTEX_DATA);
        const string prefix = shadergen.getVertexDataPrefix(vertexData);
        if (space == WORLD_SPACE)
        {
            ShaderPort* normal = vertexData[HW::T_NORMAL_WORLD];
            if (!normal->isEmitted())
            {
                normal->setEmitted();
                shadergen.emitLine(prefix + normal->getVariable() + " = normalize(mul(" + HW::T_WORLD_INVERSE_TRANSPOSE_MATRIX + ", " + v4 + "(" + HW::T_IN_NORMAL + ", 0.0)).xyz)", stage);
            }
        }
        else
        {
            ShaderPort* normal = vertexData[HW::T_NORMAL_OBJECT];
            if (!normal->isEmitted())
            {
                normal->setEmitted();
                shadergen.emitLine(prefix + normal->getVariable() + " = " + HW::T_IN_NORMAL, stage);
            }
        }
    }

    DEFINE_SHADER_STAGE(stage, Stage::PIXEL)
    {
        VariableBlock& vertexData = stage.getInputBlock(HW::VERTEX_DATA);
        const string prefix = shadergen.getVertexDataPrefix(vertexData);
        shadergen.emitLineBegin(stage);
        shadergen.emitOutput(node.getOutput(), true, false, context, stage);
        if (space == WORLD_SPACE)
        {
            const ShaderPort* normal = vertexData[HW::T_NORMAL_WORLD];
            shadergen.emitString(" = normalize(" + prefix + normal->getVariable() + ")", stage);
        }
        else
        {
            const ShaderPort* normal = vertexData[HW::T_NORMAL_OBJECT];
            shadergen.emitString(" = normalize(" + prefix + normal->getVariable() + ")", stage);
        }
        shadergen.emitLineEnd(stage);
    }
}

//
// HwTangentNodeSlang
//

void HwTangentNodeSlang::emitFunctionCall(const ShaderNode& node, GenContext& context, ShaderStage& stage) const
{
    const HwShaderGenerator& shadergen = static_cast<const HwShaderGenerator&>(context.getShaderGenerator());
    const string v4 = getVec4Name(context);

    const ShaderInput* spaceInput = node.getInput(SPACE);
    const int space = spaceInput ? spaceInput->getValue()->asA<int>() : OBJECT_SPACE;

    DEFINE_SHADER_STAGE(stage, Stage::VERTEX)
    {
        VariableBlock& vertexData = stage.getOutputBlock(HW::VERTEX_DATA);
        const string prefix = shadergen.getVertexDataPrefix(vertexData);
        if (space == WORLD_SPACE)
        {
            ShaderPort* tangent = vertexData[HW::T_TANGENT_WORLD];
            if (!tangent->isEmitted())
            {
                tangent->setEmitted();
                shadergen.emitLine(prefix + tangent->getVariable() + " = normalize(mul(" + HW::T_WORLD_MATRIX + ", " + v4 + "(" + HW::T_IN_TANGENT + ", 0.0)).xyz)", stage);
            }
        }
        else
        {
            ShaderPort* tangent = vertexData[HW::T_TANGENT_OBJECT];
            if (!tangent->isEmitted())
            {
                tangent->setEmitted();
                shadergen.emitLine(prefix + tangent->getVariable() + " = " + HW::T_IN_TANGENT, stage);
            }
        }
    }

    DEFINE_SHADER_STAGE(stage, Stage::PIXEL)
    {
        VariableBlock& vertexData = stage.getInputBlock(HW::VERTEX_DATA);
        const string prefix = shadergen.getVertexDataPrefix(vertexData);
        shadergen.emitLineBegin(stage);
        shadergen.emitOutput(node.getOutput(), true, false, context, stage);
        if (space == WORLD_SPACE)
        {
            const ShaderPort* tangent = vertexData[HW::T_TANGENT_WORLD];
            shadergen.emitString(" = normalize(" + prefix + tangent->getVariable() + ")", stage);
        }
        else
        {
            const ShaderPort* tangent = vertexData[HW::T_TANGENT_OBJECT];
            shadergen.emitString(" = normalize(" + prefix + tangent->getVariable() + ")", stage);
        }
        shadergen.emitLineEnd(stage);
    }
}

//
// HwBitangentNodeSlang
//

void HwBitangentNodeSlang::emitFunctionCall(const ShaderNode& node, GenContext& context, ShaderStage& stage) const
{
    const HwShaderGenerator& shadergen = static_cast<const HwShaderGenerator&>(context.getShaderGenerator());
    const string v4 = getVec4Name(context);
    const GenOptions& options = context.getOptions();

    const ShaderInput* spaceInput = node.getInput(SPACE);
    const int space = spaceInput ? spaceInput->getValue()->asA<int>() : OBJECT_SPACE;

    DEFINE_SHADER_STAGE(stage, Stage::VERTEX)
    {
        VariableBlock& vertexData = stage.getOutputBlock(HW::VERTEX_DATA);
        const string prefix = shadergen.getVertexDataPrefix(vertexData);
        if (space == WORLD_SPACE)
        {
            ShaderPort* bitangent = vertexData[HW::T_BITANGENT_WORLD];

            if (!bitangent->isEmitted())
            {
                bitangent->setEmitted();

                if (options.hwImplicitBitangents)
                {
                    ShaderPort* normal = vertexData[HW::T_NORMAL_WORLD];
                    if (!normal->isEmitted())
                    {
                        normal->setEmitted();
                        shadergen.emitLine(prefix + normal->getVariable() + " = normalize(mul(" + HW::T_WORLD_INVERSE_TRANSPOSE_MATRIX + ", " + v4 + "(" + HW::T_IN_NORMAL + ", 0.0)).xyz)", stage);
                    }
                    ShaderPort* tangent = vertexData[HW::T_TANGENT_WORLD];
                    if (!tangent->isEmitted())
                    {
                        tangent->setEmitted();
                        shadergen.emitLine(prefix + tangent->getVariable() + " = normalize(mul(" + HW::T_WORLD_MATRIX + ", " + v4 + "(" + HW::T_IN_TANGENT + ", 0.0)).xyz)", stage);
                    }
                    shadergen.emitLine(prefix + bitangent->getVariable() + " = cross(" + prefix + normal->getVariable() + ", " + prefix + tangent->getVariable() + ")", stage);
                }
                else
                {
                    shadergen.emitLine(prefix + bitangent->getVariable() + " = normalize(mul(" + HW::T_WORLD_MATRIX + ", " + v4 + "(" + HW::T_IN_BITANGENT + ", 0.0)).xyz)", stage);
                }
            }
        }
        else
        {
            ShaderPort* bitangent = vertexData[HW::T_BITANGENT_OBJECT];
            if (!bitangent->isEmitted())
            {
                bitangent->setEmitted();

                if (options.hwImplicitBitangents)
                {
                    shadergen.emitLine(prefix + bitangent->getVariable() + " = cross(" + HW::T_IN_NORMAL + ", " + HW::T_IN_TANGENT + ")", stage);
                }
                else
                {
                    shadergen.emitLine(prefix + bitangent->getVariable() + " = " + HW::T_IN_BITANGENT, stage);
                }
            }
        }
    }

    DEFINE_SHADER_STAGE(stage, Stage::PIXEL)
    {
        VariableBlock& vertexData = stage.getInputBlock(HW::VERTEX_DATA);
        const string prefix = shadergen.getVertexDataPrefix(vertexData);
        shadergen.emitLineBegin(stage);
        shadergen.emitOutput(node.getOutput(), true, false, context, stage);
        if (space == WORLD_SPACE)
        {
            const ShaderPort* bitangent = vertexData[HW::T_BITANGENT_WORLD];
            shadergen.emitString(" = normalize(" + prefix + bitangent->getVariable() + ")", stage);
        }
        else
        {
            const ShaderPort* bitangent = vertexData[HW::T_BITANGENT_OBJECT];
            shadergen.emitString(" = normalize(" + prefix + bitangent->getVariable() + ")", stage);
        }
        shadergen.emitLineEnd(stage);
    }
}

//
// HwViewDirectionNodeSlang
//

void HwViewDirectionNodeSlang::emitFunctionCall(const ShaderNode& node, GenContext& context, ShaderStage& stage) const
{
    const HwShaderGenerator& shadergen = static_cast<const HwShaderGenerator&>(context.getShaderGenerator());
    const string v4 = getVec4Name(context);

    const ShaderInput* spaceInput = node.getInput(SPACE);
    const int space = spaceInput ? spaceInput->getValue()->asA<int>() : OBJECT_SPACE;

    DEFINE_SHADER_STAGE(stage, Stage::VERTEX)
    {
        VariableBlock& vertexData = stage.getOutputBlock(HW::VERTEX_DATA);
        const string prefix = shadergen.getVertexDataPrefix(vertexData);
        ShaderPort* position = vertexData[HW::T_POSITION_WORLD];
        if (!position->isEmitted())
        {
            position->setEmitted();
            shadergen.emitLine(prefix + position->getVariable() + " = hPositionWorld.xyz", stage);
        }
    }

    DEFINE_SHADER_STAGE(stage, Stage::PIXEL)
    {
        VariableBlock& vertexData = stage.getInputBlock(HW::VERTEX_DATA);
        const string prefix = shadergen.getVertexDataPrefix(vertexData);
        shadergen.emitLineBegin(stage);
        shadergen.emitOutput(node.getOutput(), true, false, context, stage);
        ShaderPort* position = vertexData[HW::T_POSITION_WORLD];
        if (space == WORLD_SPACE)
        {
            shadergen.emitString(" = normalize(" + prefix + position->getVariable() + " - " + HW::T_VIEW_POSITION + ")", stage);
        }
        else
        {
            shadergen.emitString(" = normalize(mul(" + HW::T_WORLD_INVERSE_TRANSPOSE_MATRIX + ", " + v4 + "(" + prefix + position->getVariable() + " - " + HW::T_VIEW_POSITION + ", 0.0)).xyz)", stage);
        }
        shadergen.emitLineEnd(stage);
    }
}

MATERIALX_NAMESPACE_END
