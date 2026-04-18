//
// Slang-specific overrides for MaterialX Hw nodes.
// The upstream HwNormalNode/HwTangentNode/HwBitangentNode/HwViewDirectionNode
// hardcode "vec4" (GLSL syntax) instead of using getSyntax().getTypeName().
// These overrides use the Syntax API so the correct type name is emitted.
//

#pragma once

#include <MaterialXGenShader/Nodes/HwNormalNode.h>
#include <MaterialXGenShader/Nodes/HwTangentNode.h>
#include <MaterialXGenShader/Nodes/HwBitangentNode.h>
#include <MaterialXGenShader/Nodes/HwViewDirectionNode.h>

MATERIALX_NAMESPACE_BEGIN

class HwNormalNodeSlang : public HwNormalNode
{
  public:
    static ShaderNodeImplPtr create()
    {
        return std::make_shared<HwNormalNodeSlang>();
    }
    void emitFunctionCall(const ShaderNode& node, GenContext& context, ShaderStage& stage) const override;
};

class HwTangentNodeSlang : public HwTangentNode
{
  public:
    static ShaderNodeImplPtr create()
    {
        return std::make_shared<HwTangentNodeSlang>();
    }
    void emitFunctionCall(const ShaderNode& node, GenContext& context, ShaderStage& stage) const override;
};

class HwBitangentNodeSlang : public HwBitangentNode
{
  public:
    static ShaderNodeImplPtr create()
    {
        return std::make_shared<HwBitangentNodeSlang>();
    }
    void emitFunctionCall(const ShaderNode& node, GenContext& context, ShaderStage& stage) const override;
};

class HwViewDirectionNodeSlang : public HwViewDirectionNode
{
  public:
    static ShaderNodeImplPtr create()
    {
        return std::make_shared<HwViewDirectionNodeSlang>();
    }
    void emitFunctionCall(const ShaderNode& node, GenContext& context, ShaderStage& stage) const override;
};

MATERIALX_NAMESPACE_END
