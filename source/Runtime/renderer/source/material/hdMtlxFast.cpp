//
// Copyright 2021 Pixar
//
// Licensed under the terms set forth in the LICENSE.txt file available at
// https://openusd.org/license.
//
#include "hdMtlxFast.h"

#include <MaterialXCore/Document.h>
#include <MaterialXCore/Node.h>
#include <MaterialXFormat/Environ.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXFormat/XmlIo.h>
#include <spdlog/spdlog.h>

#include "pxr/base/arch/fileSystem.h"
#include "pxr/base/gf/matrix3d.h"
#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/tf/debug.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/getenv.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/trace/trace.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/hd/materialNetwork2Interface.h"
#include "pxr/imaging/hdMtlx/debugCodes.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/sdf/schema.h"
#include "pxr/usd/sdr/registry.h"
#include "pxr/usd/usdMtlx/utils.h"

namespace mx = MaterialX;

RUZINO_NAMESPACE_OPEN_SCOPE

using namespace pxr;

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    (texcoord)(geompropvalue)(filename)(ND_surface)(typeName)((
        mtlxVersion,
        "mtlx:version")));

TF_DEFINE_PRIVATE_TOKENS(
    _usdTypeTokens,
    ((boolType, "bool"))((intType, "int"))(intarray)((floatType, "float"))(
        floatarray)(color3f)(color3fArray)(color4f)(color4fArray)(float2)(float2Array)(float3)(float3Array)(float4)(float4Array)(matrix3d)(matrix4d)(asset)(string)(stringArray));

// Return the MaterialX Node string with the namespace prepended when present
static std::string _GetMxNodeString(mx::NodeDefPtr const& mxNodeDef)
{
    // If the nodedef is in a namespace, add it to the node string
    return mxNodeDef->hasNamespace()
               ? mxNodeDef->getNamespace() + ":" + mxNodeDef->getNodeString()
               : mxNodeDef->getNodeString();
}

// Return the MaterialX Node Type based on the corresponding NodeDef name,
// which is stored as the hdNodeType.
static TfToken _GetMxNodeType(
    mx::DocumentPtr const& mxDoc,
    TfToken const& hdNodeType)
{
    mx::NodeDefPtr mxNodeDef = mxDoc->getNodeDef(hdNodeType.GetString());
    if (!mxNodeDef) {
        TF_WARN(
            "Unsupported node type '%s' cannot find the associated NodeDef.",
            hdNodeType.GetText());
        return TfToken();
    }

    return TfToken(_GetMxNodeString(mxNodeDef));
}

// Add the mxNode to the mxNodeGraph, or get the mxNode from the NodeGraph
static mx::NodePtr _AddNodeToNodeGraph(
    std::string const& mxNodeName,
    std::string const& mxNodeCategory,
    std::string const& mxNodeType,
    mx::NodeGraphPtr const& mxNodeGraph,
    mx::StringSet* addedNodeNames)
{
    // Add the node to the  mxNodeGraph if needed
    if (addedNodeNames->find(mxNodeName) == addedNodeNames->end()) {
        addedNodeNames->insert(mxNodeName);
        return mxNodeGraph->addNode(mxNodeCategory, mxNodeName, mxNodeType);
    }
    // Otherwise get the existing node from the mxNodeGraph
    return mxNodeGraph->getNode(mxNodeName);
}

static bool _UsesTexcoordNode(mx::NodeDefPtr const& mxNodeDef)
{
    mx::InterfaceElementPtr impl = mxNodeDef->getImplementation();
    if (impl && impl->isA<mx::NodeGraph>()) {
        mx::NodeGraphPtr nodegraph = impl->asA<mx::NodeGraph>();
        if (!nodegraph->getNodes(_tokens->texcoord).empty()) {
            return true;
        }
    }
    return false;
}

static std::string _ConvertToMtlxType(const TfToken& usdTypeName)
{
    static const auto _typeTable =
        std::unordered_map<TfToken, std::string, TfToken::HashFunctor>{
            { _usdTypeTokens->boolType, "boolean" },
            { _usdTypeTokens->intType, "integer" },
            { _usdTypeTokens->intarray, "integerarray" },
            { _usdTypeTokens->floatType, "float" },
            { _usdTypeTokens->floatarray, "floatarray" },
            { _usdTypeTokens->color3f, "color3" },
            { _usdTypeTokens->color3fArray, "color3array" },
            { _usdTypeTokens->color4f, "color4" },
            { _usdTypeTokens->color4fArray, "color4array" },
            { _usdTypeTokens->float2, "vector2" },
            { _usdTypeTokens->float2Array, "vector2array" },
            { _usdTypeTokens->float3, "vector3" },
            { _usdTypeTokens->float3Array, "vector3array" },
            { _usdTypeTokens->float4, "vector4" },
            { _usdTypeTokens->float4Array, "vector4array" },
            { _usdTypeTokens->matrix3d, "matrix33" },
            { _usdTypeTokens->matrix4d, "matrix44" },
            { _usdTypeTokens->asset, "filename" },
            { _usdTypeTokens->string, "string" },
            { _usdTypeTokens->stringArray, "stringarray" }
        };
    auto typeIt = _typeTable.find(usdTypeName);
    return typeIt == _typeTable.end() ? "" : typeIt->second;
}

static std::string _GetInputType(
    mx::NodeDefPtr const& mxNodeDef,
    std::string const& mxInputName,
    TfToken const& usdTypeName = TfToken())
{
    // If given, use the usdTypeName to get the materialX input type
    if (!usdTypeName.IsEmpty()) {
        return _ConvertToMtlxType(usdTypeName);
    }

    // Otherwise look to the nodedef to get the input type
    std::string mxInputType;
    mx::InputPtr mxInput = mxNodeDef->getActiveInput(mxInputName);
    if (mxInput) {
        mxInputType = mxInput->getType();
    }
    return mxInputType;
}

// Nodedef names may change between MaterialX versions, so given the
// prevMxNodeDefName get the corresponding nodedef name appropriate for the
// version of MaterialX being used.
static std::string _GetNodeDefName(std::string const& prevMxNodeDefName)
{
    // This handles the case that nodedef names have changed between MaterialX
    // v1.38 and the current version
    std::string mxNodeDefName = prevMxNodeDefName;
#if MATERIALX_MAJOR_VERSION == 1 && MATERIALX_MINOR_VERSION >= 39
    // Between v1.38 and v1.39 only the normalmap nodedef name changed
    if (prevMxNodeDefName == "ND_normalmap") {
        mxNodeDefName = "ND_normalmap_float";
    }
#endif
    return mxNodeDefName;
}

// Add a MaterialX version of the hdNode to the mxDoc/mxNodeGraph
static mx::NodePtr _AddMaterialXNode(
    HdMaterialNetworkInterface* netInterface,
    TfToken const& hdNodeName,
    mx::DocumentPtr const& mxDoc,
    mx::NodeGraphPtr const& mxNodeGraph,
    mx::StringSet* addedNodeNames,
    std::string const& connectionName,
    HdMtlxTexturePrimvarData* mxHdData)
{
    // Get the mxNode information
    TfToken hdNodeType = netInterface->GetNodeType(hdNodeName);
    mx::NodeDefPtr mxNodeDef =
        mxDoc->getNodeDef(_GetNodeDefName(hdNodeType.GetString()));
    if (!mxNodeDef) {
        TF_WARN("NodeDef not found for Node '%s'", hdNodeType.GetText());
        // Instead of returning here, use a ND_surface definition so that the
        // rest of the network can be processed without errors.
        // This allows networks that might have non mtlx nodes next to
        // the terminal node to come through, and those nodes will be kept
        // out of the shader compile in hdPrman.
        mxNodeDef = mxDoc->getNodeDef(_tokens->ND_surface);
    }

    const SdfPath hdNodePath(hdNodeName.GetString());
    const std::string& mxNodeName = HdMtlxCreateNameFromPath(hdNodePath);
    const std::string& mxNodeCategory = _GetMxNodeString(mxNodeDef);
    const std::string& mxNodeType = mxNodeDef->getType();

    // Add the mxNode to the mxNodeGraph
    mx::NodePtr mxNode = _AddNodeToNodeGraph(
        mxNodeName, mxNodeCategory, mxNodeType, mxNodeGraph, addedNodeNames);

    if (mxNode->getNodeDefString().empty()) {
        mxNode->setNodeDefString(hdNodeType.GetText());
    }

    // For each of the HdNode parameters add the corresponding parameter/input
    // to the mxNode
    TfTokenVector hdNodeParamNames =
        netInterface->GetAuthoredNodeParameterNames(hdNodeName);

    // Debug for geompropvalue nodes
    if (mxNodeCategory == _tokens->geompropvalue) {
        spdlog::info(
            "Processing geompropvalue node '{}', {} authored parameters",
            mxNodeName,
            hdNodeParamNames.size());
        for (const auto& pName : hdNodeParamNames) {
            auto paramData =
                netInterface->GetNodeParameterData(hdNodeName, pName);
            spdlog::info(
                "  Param '{}': value = '{}'",
                pName.GetText(),
                HdMtlxConvertToString(paramData.value));
        }
    }

    for (TfToken const& paramName : hdNodeParamNames) {
        // Get the MaterialX Parameter info
        std::string mxInputName = paramName.GetString();

        const HdMaterialNetworkInterface::NodeParamData paramData =
            netInterface->GetNodeParameterData(hdNodeName, paramName);
        const std::string mxInputValue = HdMtlxConvertToString(paramData.value);

        // Skip Colorspace and typeName parameters, these are already
        // captured in the paramData. Note: these inputs are of the form:
        //  'colorSpace:inputName' and 'typeName:inputName'
        const std::pair<std::string, bool> csResult =
            SdfPath::StripPrefixNamespace(
                mxInputName, SdfFieldKeys->ColorSpace);
        if (csResult.second) {
            continue;
        }
        const std::pair<std::string, bool> tnResult =
            SdfPath::StripPrefixNamespace(mxInputName, _tokens->typeName);
        if (tnResult.second) {
            continue;
        }

        // For UsdPrimvarReader nodes (geompropvalue), map 'varname' to
        // 'geomprop'
        if (mxNodeCategory == _tokens->geompropvalue &&
            mxInputName == "varname") {
            spdlog::info(
                "Mapping varname='{}' to geomprop for node '{}'",
                mxInputValue,
                mxNodeName);
            mxInputName = "geomprop";
        }

        // Set the input value, and colorspace on the mxNode
        const std::string mxInputType =
            _GetInputType(mxNodeDef, mxInputName, paramData.typeName);
        mx::InputPtr mxInput =
            mxNode->setInputValue(mxInputName, mxInputValue, mxInputType);
        if (!paramData.colorSpace.IsEmpty()) {
            mxInput->setColorSpace(paramData.colorSpace);
        }
    }

    // MaterialX nodes that use textures can have more than one filename input
    if (mxHdData) {
        for (mx::InputPtr const& mxInput : mxNodeDef->getActiveInputs()) {
            if (mxInput->getType() == _tokens->filename) {
                // Save the corresponding Mx and Hydra names for ShaderGen
                mxHdData->mxHdTextureMap[mxNodeName].insert(mxInput->getName());
                // Save the path to adjust parameters after for ShaderGen
                mxHdData->hdTextureNodes.insert(hdNodePath);
            }
        }
    }

    // MaterialX primvar node
    if (mxNodeCategory == _tokens->geompropvalue) {
        if (mxHdData) {
            // Save the path to have the primvarName declared in ShaderGen
            mxHdData->hdPrimvarNodes.insert(hdNodePath);
        }

        // Debug: Check if geomprop input was set
        mx::InputPtr geompropInput = mxNode->getInput("geomprop");
        if (geompropInput) {
            spdlog::info(
                "geompropvalue node '{}': geomprop = '{}'",
                mxNodeName,
                geompropInput->getValueString());
        }
        else {
            spdlog::warn(
                "geompropvalue node '{}': No 'geomprop' input found! "
                "Available inputs: {}",
                mxNodeName,
                mxNode->getInputCount());
            for (auto input : mxNode->getInputs()) {
                spdlog::info(
                    "  Input: '{}' = '{}'",
                    input->getName(),
                    input->hasValueString() ? input->getValueString()
                                            : "(connected)");
            }

            // If no geomprop input exists but varname exists, this is likely
            // an error in conversion. Try to fix it by using a default value.
            mx::InputPtr varnameInput = mxNode->getInput("varname");
            if (varnameInput) {
                std::string varnameValue = varnameInput->getValueString();
                spdlog::info(
                    "Found 'varname' input with value '{}', creating "
                    "'geomprop' input",
                    varnameValue);
                mxNode->removeInput("varname");
                mxNode->setInputValue("geomprop", varnameValue, "string");
            }
            else {
                // As a last resort, use "st" as default texture coordinate
                spdlog::warn(
                    "No varname or geomprop found, using default 'st' for "
                    "texture coordinates");
                mxNode->setInputValue("geomprop", "st", "string");
            }
        }
    }

    // Stdlib MaterialX texture coordinate node or a custom node that
    // uses a texture coordinate node
    if (mxNodeCategory == _tokens->texcoord || _UsesTexcoordNode(mxNodeDef)) {
        if (mxHdData) {
            // Save the path to have the textureCoord name declared in ShaderGen
            mxHdData->hdPrimvarNodes.insert(hdNodePath);
        }
    }
    return mxNode;
}

static void _AddInput(
    HdMaterialNetworkInterface* netInterface,
    HdMaterialNetworkInterface::InputConnection const& conn,
    TfToken const& inputName,
    mx::DocumentPtr const& mxDoc,
    mx::NodePtr const& mxCurrNode,
    mx::NodePtr const& mxNextNode,
    mx::InputPtr* mxInput)
{
    spdlog::info(
        "Adding input '{}' to node '{}' (category: '{}')",
        inputName.GetText(),
        mxCurrNode->getName(),
        mxCurrNode->getCategory());

    // If the currNode is connected to a multi-output node, the input on the
    // currNode needs to get the output type and indicate the output name.
    if (mxNextNode->isMultiOutputType()) {
        TfToken hdNextType = netInterface->GetNodeType(conn.upstreamNodeName);
        mx::NodeDefPtr mxNextNodeDef =
            mxDoc->getNodeDef(hdNextType.GetString());
        if (mxNextNodeDef) {
            mx::OutputPtr mxConnOutput =
                mxNextNodeDef->getOutput(conn.upstreamOutputName.GetString());
            if (!mxConnOutput) {
                // E.g. UsdUVTexture connected via its legacy 'rgba' output,
                // which the default (2.3) nodedef no longer declares.
                TF_WARN(
                    "Node '%s' has no output named '%s'; skipping input "
                    "'%s'.",
                    hdNextType.GetText(),
                    conn.upstreamOutputName.GetText(),
                    inputName.GetText());
                return;
            }
            // Add input with the connected Ouptut type and set the output name
            *mxInput = mxCurrNode->addInput(inputName, mxConnOutput->getType());
            (*mxInput)->setConnectedOutput(mxConnOutput);
        }
    }
    else {
        *mxInput = mxCurrNode->addInput(inputName, mxNextNode->getType());
    }
}

static void _AddNodeGraphOutput(
    HdMaterialNetworkInterface* netInterface,
    HdMaterialNetworkInterface::InputConnection const& conn,
    std::string const& outputName,
    mx::DocumentPtr const& mxDoc,
    mx::NodeGraphPtr const& mxNodeGraph,
    mx::NodePtr const& mxNextNode,
    mx::OutputPtr* mxOutput)
{
    // If the mxNodeGraph output is connected to a multi-output node, the
    // output on the mxNodegraph needs to get the output type from that
    // connected node and indicate the output name.
    if (mxNextNode->isMultiOutputType()) {
        TfToken hdNextType = netInterface->GetNodeType(conn.upstreamNodeName);
        mx::NodeDefPtr mxNextNodeDef =
            mxDoc->getNodeDef(hdNextType.GetString());
        if (mxNextNodeDef) {
            mx::OutputPtr mxConnOutput =
                mxNextNodeDef->getOutput(conn.upstreamOutputName.GetString());
            if (!mxConnOutput) {
                // Same legacy-output-name case as _AddInput above.
                TF_WARN(
                    "Node '%s' has no output named '%s'; skipping node "
                    "graph output '%s'.",
                    hdNextType.GetText(),
                    conn.upstreamOutputName.GetText(),
                    outputName.c_str());
                return;
            }
            // Add output with the connected Ouptut type and set the output name
            *mxOutput =
                mxNodeGraph->addOutput(outputName, mxConnOutput->getType());
            (*mxOutput)->setOutputString(mxConnOutput->getName());
        }
    }
    else {
        *mxOutput = mxNodeGraph->addOutput(outputName, mxNextNode->getType());
    }
}

// Recursively traverse the material n/w and gather the nodes in the MaterialX
// NodeGraph and Document
static void _GatherUpstreamNodes(
    HdMaterialNetworkInterface* netInterface,
    HdMaterialNetworkInterface::InputConnection const& hdConnection,
    mx::DocumentPtr const& mxDoc,
    mx::NodeGraphPtr* mxNodeGraph,
    mx::StringSet* addedNodeNames,
    mx::NodePtr* mxUpstreamNode,
    std::string const& connectionName,
    HdMtlxTexturePrimvarData* mxHdData,
    std::string const& materialPathStr = "")
{
    TfToken const& hdNodeName = hdConnection.upstreamNodeName;
    if (netInterface->GetNodeType(hdNodeName).IsEmpty()) {
        TF_WARN(
            "Could not find the connected Node '%s'",
            hdConnection.upstreamNodeName.GetText());
        return;
    }

    // Initilize the mxNodeGraph if needed
    if (!(*mxNodeGraph)) {
        // Use material path to ensure unique NodeGraph names in shared document
        std::string baseNodeGraphName =
            !materialPathStr.empty()
                ? materialPathStr + "_NG"
                : SdfPath(hdNodeName).GetParentPath().GetName();
        const std::string nodeGraphName =
            mxDoc->createValidChildName(baseNodeGraphName);
        *mxNodeGraph = mxDoc->addNodeGraph(nodeGraphName);
    }

    // Add the node to the mxNodeGraph/mxDoc.
    mx::NodePtr mxCurrNode = _AddMaterialXNode(
        netInterface,
        hdNodeName,
        mxDoc,
        *mxNodeGraph,
        addedNodeNames,
        connectionName,
        mxHdData);

    if (!mxCurrNode) {
        return;
    }

    TfTokenVector hdConnectionNames =
        netInterface->GetNodeInputConnectionNames(hdNodeName);

    // Continue traversing the upsteam connections to create the mxNodeGraph
    for (TfToken connName : hdConnectionNames) {
        const auto inputConnections =
            netInterface->GetNodeInputConnection(hdNodeName, connName);
        for (const auto& currConnection : inputConnections) {
            // Gather the nodes uptream from the mxCurrNode
            _GatherUpstreamNodes(
                netInterface,
                currConnection,
                mxDoc,
                mxNodeGraph,
                addedNodeNames,
                mxUpstreamNode,
                connName.GetString(),
                mxHdData,
                materialPathStr);

            // Connect mxCurrNode to the mxUpstreamNode
            mx::NodePtr mxNextNode = *mxUpstreamNode;
            if (!mxNextNode) {
                continue;
            }

            // For UsdPrimvarReader nodes (geompropvalue), map 'varname' input
            // to 'geomprop'
            TfToken mxConnName = connName;
            if (mxCurrNode->getCategory() == _tokens->geompropvalue &&
                connName == "varname") {
                mxConnName = TfToken("geomprop");
            }

            // Make sure to not add the same input twice
            mx::InputPtr mxInput = mxCurrNode->getInput(mxConnName);
            if (!mxInput) {
                _AddInput(
                    netInterface,
                    currConnection,
                    mxConnName,
                    mxDoc,
                    mxCurrNode,
                    mxNextNode,
                    &mxInput);
            }
            if (mxInput) {
                mxInput->setConnectedNode(mxNextNode);
            }
        }
    }

    *mxUpstreamNode = mxCurrNode;
}

// Create/update MaterialX shared document from the given HdMaterialNetwork2
mx::ElementPtr HdMtlxCreateMtlxDocumentFromHdNetworkFast(
    HdMaterialNetwork2 const& hdNetwork,
    HdMaterialNode2 const& hdMaterialXNode,
    SdfPath const& hdMaterialXNodePath,
    SdfPath const& materialPath,
    mx::DocumentPtr const& sharedDocument,
    std::mutex& documentMutex,
    HdMtlxTexturePrimvarData* mxHdData)
{
    // XXX Unfortunate but necessary to cast away constness even though
    // hdNetwork isn't modified.
    HdMaterialNetwork2Interface netInterface(
        materialPath, const_cast<HdMaterialNetwork2*>(&hdNetwork));

    TfToken terminalNodeName = hdMaterialXNodePath.GetAsToken();

    return HdMtlxCreateMtlxDocumentFromHdMaterialNetworkInterfaceFast(
        &netInterface,
        terminalNodeName,
        netInterface.GetNodeInputConnectionNames(terminalNodeName),
        sharedDocument,
        documentMutex,
        mxHdData);
}

// Add parameter inputs for the terminal node (which is a StandardSurface or
// USDPreviewSurface node)
static void _AddParameterInputsToTerminalNode(
    HdMaterialNetworkInterface* netInterface,
    TfToken const& terminalNodeName,
    TfToken const& mxType,
    mx::NodePtr const& mxShaderNode)
{
    TfTokenVector paramNames =
        netInterface->GetAuthoredNodeParameterNames(terminalNodeName);

    // Cache key based on shader node category and type
    static std::unordered_map<std::string, mx::NodeDefPtr> s_nodeDefCache;
    static std::mutex s_nodeDefCacheMutex;

    std::string cache_key =
        mxShaderNode->getCategory() + ":" + mxShaderNode->getType();
    mx::NodeDefPtr mxNodeDef;

    {
        std::lock_guard<std::mutex> lock(s_nodeDefCacheMutex);
        auto cache_it = s_nodeDefCache.find(cache_key);
        if (cache_it != s_nodeDefCache.end()) {
            mxNodeDef = cache_it->second;
        }
        else {
            spdlog::info(
                "Terminal NodeDef cache miss for '{}', searching 3000+ "
                "definitions...",
                cache_key);
            mxNodeDef = mxShaderNode->getNodeDef("", true);
            s_nodeDefCache[cache_key] = mxNodeDef;
        }
    }

    if (!mxNodeDef) {
        TF_WARN("NodeDef not found for Node '%s'", mxType.GetText());
        return;
    }

    for (TfToken const& paramName : paramNames) {
        // Get the MaterialX Parameter info
        const std::string& mxInputName = paramName.GetString();
        const HdMaterialNetworkInterface::NodeParamData paramData =
            netInterface->GetNodeParameterData(terminalNodeName, paramName);
        const std::string mxInputValue = HdMtlxConvertToString(paramData.value);

        // Skip Colorspace and typeName parameters, these are already
        // captured in the paramData. Note: these inputs are of the form:
        //  'colorSpace:inputName' and 'typeName:inputName'
        const std::pair<std::string, bool> csResult =
            SdfPath::StripPrefixNamespace(
                mxInputName, SdfFieldKeys->ColorSpace);
        if (csResult.second) {
            continue;
        }
        const std::pair<std::string, bool> tnResult =
            SdfPath::StripPrefixNamespace(mxInputName, _tokens->typeName);
        if (tnResult.second) {
            continue;
        }

        // Set the Input value on the mxShaderNode
        mx::InputPtr mxInput = mxShaderNode->setInputValue(
            mxInputName, mxInputValue, _GetInputType(mxNodeDef, mxInputName));
        if (!paramData.colorSpace.IsEmpty()) {
            mxInput->setColorSpace(paramData.colorSpace);
        }
    }
}

// Updates mxDoc from traversing the node graph leading into the terminal node.
static void _CreateMtlxNodeGraphFromTerminalNodeConnections(
    HdMaterialNetworkInterface* netInterface,
    TfToken const& terminalNodeName,
    TfTokenVector const& terminalNodeConnectionNames,
    mx::DocumentPtr const& mxDoc,
    mx::NodePtr const& mxShaderNode,
    HdMtlxTexturePrimvarData* mxHdData)
{
    mx::NodeGraphPtr mxNodeGraph;
    mx::StringSet addedNodeNames;  // Set of NodeNames in the mxNodeGraph

    // Get material path for unique naming
    SdfPath materialPath = netInterface->GetMaterialPrimPath();
    std::string materialPathStr = materialPath.GetString();
    // Replace '/' with '_' to make it a valid name
    std::replace(materialPathStr.begin(), materialPathStr.end(), '/', '_');
    if (!materialPathStr.empty() && materialPathStr[0] == '_') {
        materialPathStr = materialPathStr.substr(1);
    }

    for (TfToken const& cName : terminalNodeConnectionNames) {
        const std::string& mxNodeGraphOutput = cName.GetString();
        const auto inputConnections =
            netInterface->GetNodeInputConnection(terminalNodeName, cName);
        for (const auto& currConnection : inputConnections) {
            // Gather the nodes uptream from the hdMaterialXNode
            mx::NodePtr mxUpstreamNode;

            _GatherUpstreamNodes(
                netInterface,
                currConnection,
                mxDoc,
                &mxNodeGraph,
                &addedNodeNames,
                &mxUpstreamNode,
                mxNodeGraphOutput,
                mxHdData,
                materialPathStr);

            if (!mxUpstreamNode) {
                continue;
            }

            // Connect currNode to the upstream Node
            std::string fullOutputName =
                mxNodeGraphOutput + "_" +
                currConnection.upstreamOutputName.GetString();
            mx::OutputPtr mxOutput;
            _AddNodeGraphOutput(
                netInterface,
                currConnection,
                fullOutputName,
                mxDoc,
                mxNodeGraph,
                mxUpstreamNode,
                &mxOutput);
            if (mxOutput) {
                mxOutput->setConnectedNode(mxUpstreamNode);
            }

            // Connect NodeGraph Output to the ShaderNode
            mx::InputPtr mxInput;
            _AddInput(
                netInterface,
                currConnection,
                cName,
                mxDoc,
                mxShaderNode,
                mxUpstreamNode,
                &mxInput);
            if (mxInput) {
                mxInput->setConnectedOutput(mxOutput);
            }
        }
    }
}

MaterialX::ElementPtr
HdMtlxCreateMtlxDocumentFromHdMaterialNetworkInterfaceFast(
    HdMaterialNetworkInterface* netInterface,
    TfToken const& terminalNodeName,
    TfTokenVector const& terminalNodeConnectionNames,
    MaterialX::DocumentPtr const& sharedDocument,
    std::mutex& documentMutex,
    HdMtlxTexturePrimvarData* mxHdData)
{
    TRACE_FUNCTION_SCOPE("Add Material to Shared Mtlx Document")
    if (!netInterface || !sharedDocument) {
        return nullptr;
    }

    // Lock the shared document for thread-safe access
    std::lock_guard<std::mutex> lock(documentMutex);

    // Use the shared document (no copy, no import - much faster!)
    mx::DocumentPtr mxDoc = sharedDocument;

    // Get the version of the MaterialX document if specified, otherwise
    // default to v1.38.
    std::string materialXVersionString = "1.38";
    const VtValue materialXVersionValue =
        netInterface->GetMaterialConfigValue(_tokens->mtlxVersion);
    if (materialXVersionValue.IsHolding<std::string>()) {
        materialXVersionString = materialXVersionValue.Get<std::string>();
        TF_DEBUG(HDMTLX_VERSION_UPGRADE)
            .Msg(
                "[%s] : MaterialX document version : '%s'\n",
                TF_FUNC_NAME().c_str(),
                materialXVersionString.c_str());
    }
    else {
        TF_DEBUG(HDMTLX_VERSION_UPGRADE)
            .Msg(
                "[%s] : MaterialX document version : '%s' (Using default)\n",
                TF_FUNC_NAME().c_str(),
                materialXVersionString.c_str());
    }
    mxDoc->setVersionString(materialXVersionString);

    // Create a material that instantiates the shader
    SdfPath materialPath = netInterface->GetMaterialPrimPath();
    const std::string& materialName = materialPath.GetName();
    TfToken mxType =
        _GetMxNodeType(mxDoc, netInterface->GetNodeType(terminalNodeName));

    // Use unique names for all nodes since we're using a shared document
    std::string uniqueShaderName =
        mxDoc->createValidChildName(materialName + "_Surface");
    std::string uniqueMaterialName = mxDoc->createValidChildName(materialName);

    mx::NodePtr mxShaderNode =
        mxDoc->addNode(mxType.GetString(), uniqueShaderName, "surfaceshader");
    mx::NodePtr mxMaterial =
        mxDoc->addMaterialNode(uniqueMaterialName, mxShaderNode);

    _CreateMtlxNodeGraphFromTerminalNodeConnections(
        netInterface,
        terminalNodeName,
        terminalNodeConnectionNames,
        mxDoc,
        mxShaderNode,
        mxHdData);

    _AddParameterInputsToTerminalNode(
        netInterface, terminalNodeName, mxType, mxShaderNode);

    if (TfDebug::IsEnabled(HDMTLX_VERSION_UPGRADE)) {
        const std::string filename = mxMaterial->getName() + "_before.mtlx";
        TF_DEBUG(HDMTLX_VERSION_UPGRADE)
            .Msg(
                "[%s] : MaterialX document before upgrade: '%s'\n",
                TF_FUNC_NAME().c_str(),
                filename.c_str());
        mx::writeToXmlFile(mxDoc, mx::FilePath(filename));
    }

    // Potentially upgrade the MaterialX document to the "current" version,
    // using the MaterialX upgrade mechanism.
    // mxDoc->upgradeVersion();

    if (TfDebug::IsEnabled(HDMTLX_VERSION_UPGRADE)) {
        const std::string filename = mxMaterial->getName() + "_after.mtlx";
        TF_DEBUG(HDMTLX_VERSION_UPGRADE)
            .Msg(
                "[%s] : MaterialX document after upgrade: '%s'\n",
                TF_FUNC_NAME().c_str(),
                filename.c_str());
        mx::writeToXmlFile(mxDoc, mx::FilePath(filename));
    }
    else if (TfDebug::IsEnabled(HDMTLX_WRITE_DOCUMENT)) {
        const std::string filename = mxMaterial->getName() + ".mtlx";
        TF_DEBUG(HDMTLX_WRITE_DOCUMENT)
            .Msg(
                "[%s] : MaterialX document: '%s'\n",
                TF_FUNC_NAME().c_str(),
                filename.c_str());
        mx::writeToXmlFile(mxDoc, mx::FilePath(filename));
    }

    // Return the material element (for shader generation)
    return mxMaterial;
}

RUZINO_NAMESPACE_CLOSE_SCOPE
