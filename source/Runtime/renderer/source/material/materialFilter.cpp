#include "materialFilter.h"

#include <spdlog/spdlog.h>

#include "MaterialXGenShader/TypeDesc.h"
#include "MaterialXGenShader/Util.h"
#include "pxr/base/arch/library.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usdImaging/usdImaging/tokens.h"

RUZINO_NAMESPACE_OPEN_SCOPE
TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    (mtlx)

    // Hydra MaterialX Node Types
    (ND_standard_surface_surfaceshader)(ND_UsdPreviewSurface_surfaceshader)(ND_displacement_float)(ND_displacement_vector3)(ND_image_vector2)(ND_image_vector3)(ND_image_vector4)
    // For supporting Usd texturing nodes
    (
        wrapS)(wrapT)(repeat)(periodic)(ND_UsdUVTexture_23)(ND_dot_vector2)(ND_UsdPrimvarReader_vector2)(UsdPrimvarReader_float2)(UsdUVTexture)(UsdVerticalFlip)(varname)(file)(filename)(black)(clamp)(uaddressmode)(vaddressmode)(ND_geompropvalue_vector2)(ND_separate2_vector2)(ND_separate3_vector3)(ND_separate4_vector4)(ND_separate3_color3)(ND_separate4_color4)(ND_floor_float)(ND_convert_float_color3)(ND_convert_float_color4)(ND_convert_float_vector2)(ND_convert_float_vector3)(ND_convert_float_vector4)(ND_convert_vector2_vector3)(ND_convert_vector3_color3)(ND_convert_vector3_vector2)(ND_convert_vector3_vector4)(ND_convert_vector4_color4)(ND_convert_vector4_vector3)(ND_convert_color3_vector3)(ND_convert_color4_vector4)(ND_convert_color3_color4)(ND_convert_color4_color3)(convert)(ND_multiply_float)(ND_add_float)(ND_subtract_float)(ND_combine2_vector2)(separate2)(separate3)(separate4)(floor)(multiply)(add)(subtract)(combine2)(texcoord)(geomprop)(geompropvalue)(in)(in1)(in2)(out)(outx)(outy)(st)(vector2)((
        string_type,
        "string"))  // Color Space
    ((cs_raw, "raw"))((cs_auto, "auto"))((cs_srgb, "sRGB"))(
        (mtlx_srgb, "srgb_texture")));

TfToken _FixSingleType(TfToken const& nodeType)
{
    if (nodeType == UsdImagingTokens->UsdPreviewSurface) {
        return _tokens->ND_UsdPreviewSurface_surfaceshader;
    }
    else if (nodeType == UsdImagingTokens->UsdUVTexture) {
        return _tokens->ND_UsdUVTexture_23;
    }
    else if (nodeType == UsdImagingTokens->UsdPrimvarReader_float2) {
        return _tokens->ND_UsdPrimvarReader_vector2;
    }

    else {
        return TfToken("ND_" + nodeType.GetString());
    }
}

bool _FindGraphAndNodeByName(
    mx::DocumentPtr const& mxDoc,
    std::string const& mxNodeGraphName,
    std::string const& mxNodeName,
    mx::NodeGraphPtr* mxNodeGraph,
    mx::NodePtr* mxNode)
{
    // Graph names are uniquified with mxDoc->createValidChildName in hdMtlx,
    // so attempting to get the graph by the expected name may fail.
    // Go to some extra effort to find the graph that contains the named node.

    *mxNodeGraph = mxDoc->getNodeGraph(mxNodeGraphName);

    if (*mxNodeGraph) {
        *mxNode = (*mxNodeGraph)->getNode(mxNodeName);
    }
    if (!*mxNode) {
        std::vector<mx::NodeGraphPtr> graphs = mxDoc->getNodeGraphs();
        // first try last graph
        if (graphs.size()) {
            *mxNode = (*(graphs.rbegin()))->getNode(mxNodeName);
            if (*mxNode) {
                *mxNodeGraph = *graphs.rbegin();
            }
        }
        // Sometimes the above approach fails, so go looking
        // through all the graph nodes for the texture
        if (!*mxNode) {
            for (auto graph : graphs) {
                *mxNode = graph->getNode(mxNodeName);
                if (*mxNode) {
                    *mxNodeGraph = graph;
                    break;
                }
            }
        }
    }
    return (*mxNode != nullptr);
}

TfToken _GetHdWrapString(
    TfToken const& hdTextureNodeName,
    std::string const& mxInputValue)
{
    if (mxInputValue == "constant") {
        TF_WARN(
            "RtxHioImagePlugin: Texture '%s' has unsupported wrap mode "
            "'constant' using 'black' instead.",
            hdTextureNodeName.GetText());
        return _tokens->black;
    }
    if (mxInputValue == "clamp") {
        return _tokens->clamp;
    }
    if (mxInputValue == "mirror") {
        TF_WARN(
            "RtxHioImagePlugin: Texture '%s' has unsupported wrap mode "
            "'mirror' using 'repeat' instead.",
            hdTextureNodeName.GetText());
        return _tokens->repeat;
    }
    return _tokens->repeat;
}

void _GetWrapModes(
    HdMaterialNetworkInterface* netInterface,
    TfToken const& hdTextureNodeName,
    TfToken* uWrap,
    TfToken* vWrap)
{
    // For <tiledimage> nodes want to always use "repeat"
    *uWrap = _tokens->repeat;
    *vWrap = _tokens->repeat;

    // For <image> nodes:
    VtValue vUAddrMode = netInterface->GetNodeParameterValue(
        hdTextureNodeName, _tokens->uaddressmode);
    if (!vUAddrMode.IsEmpty()) {
        *uWrap = _GetHdWrapString(
            hdTextureNodeName, vUAddrMode.UncheckedGet<std::string>());
    }
    VtValue vVAddrMode = netInterface->GetNodeParameterValue(
        hdTextureNodeName, _tokens->vaddressmode);
    if (!vVAddrMode.IsEmpty()) {
        *vWrap = _GetHdWrapString(
            hdTextureNodeName, vVAddrMode.UncheckedGet<std::string>());
    }
}

bool _NodeHasTextureCoordPrimvar(
    mx::DocumentPtr const& mxDoc,
    const SdrShaderNodeConstPtr mtlxSdrNode)
{
    // Custom nodes may have a <texcoord> or <geompropvalue> node as
    // a part of the defining nodegraph
    const mx::NodeDefPtr mxNodeDef =
        mxDoc->getNodeDef(mtlxSdrNode->GetIdentifier().GetString());
    mx::InterfaceElementPtr impl = mxNodeDef->getImplementation();
    if (impl && impl->isA<mx::NodeGraph>()) {
        const mx::NodeGraphPtr nodegraph = impl->asA<mx::NodeGraph>();
        // Return True if the defining nodegraph uses a texcoord node
        if (!nodegraph->getNodes(_tokens->texcoord).empty()) {
            return true;
        }
        // Or a geompropvalue node of type vector2, which we assume to be
        // for texture coordinates.
        auto geompropvalueNodes = nodegraph->getNodes(_tokens->geompropvalue);
        for (const mx::NodePtr& mxGeomPropNode : geompropvalueNodes) {
#if MATERIALX_MAJOR_VERSION == 1 && MATERIALX_MINOR_VERSION <= 38
            if (mxGeomPropNode->getType() == mx::Type::VECTOR2->getName()) {
#else
            if (mxGeomPropNode->getType() == mx::Type::VECTOR2.getName()) {
#endif
                return true;
            }
        }
    }
    return false;
}

TfToken _GetColorSpace(
    HdMaterialNetworkInterface* netInterface,
    TfToken const& hdTextureNodeName,
    HdMaterialNetworkInterface::NodeParamData paramData)
{
    const TfToken nodeType = netInterface->GetNodeType(hdTextureNodeName);
    if (nodeType == _tokens->ND_image_vector2 ||
        nodeType == _tokens->ND_image_vector3 ||
        nodeType == _tokens->ND_image_vector4) {
        // For images not used as color use "raw" (eg. normal maps)
        return _tokens->cs_raw;
    }
    else {
#if PXR_VERSION >= 2402
        if (paramData.colorSpace == _tokens->mtlx_srgb) {
            return _tokens->cs_srgb;
        }
        else {
            return _tokens->cs_auto;
        }
#else
        return _tokens->cs_auto;
#endif
    }
}

void _UpdateTextureNodes(
    HdMaterialNetworkInterface* netInterface,
    std::set<SdfPath> const& hdTextureNodePaths,
    mx::DocumentPtr const& mxDoc)
{
    for (SdfPath const& texturePath : hdTextureNodePaths) {
        TfToken const& textureNodeName = texturePath.GetToken();
        std::string mxTextureNodeName = HdMtlxCreateNameFromPath(texturePath);
        const TfToken nodeType = netInterface->GetNodeType(textureNodeName);
        if (nodeType.IsEmpty()) {
            TF_WARN(
                "Connot find texture node '%s' in material network.",
                textureNodeName.GetText());
            continue;
        }
        // Get the filename parameter name,
        // MaterialX stdlib nodes use 'file' however, this could be different
        // for custom nodes that use textures.
        TfToken fileParamName = _tokens->file;
        const mx::NodeDefPtr nodeDef = mxDoc->getNodeDef(nodeType);
        if (nodeDef) {
            for (auto const& mxInput : nodeDef->getActiveInputs()) {
                if (mxInput->getType() == _tokens->filename) {
                    fileParamName = TfToken(mxInput->getName());
                }
            }
        }
#if PXR_VERSION >= 2402
        HdMaterialNetworkInterface::NodeParamData fileParamData =
            netInterface->GetNodeParameterData(textureNodeName, fileParamName);
        const VtValue vFile = fileParamData.value;
#else
        VtValue vFile =
            netInterface->GetNodeParameterValue(textureNodeName, fileParamName);
#endif
        if (vFile.IsEmpty()) {
            TF_WARN(
                "File path missing for texture node '%s'.",
                textureNodeName.GetText());
            continue;
        }

        std::string path;

        // Typically expect SdfAssetPath, but UsdUVTexture nodes may
        // have changed value to string due to MatfiltConvertPreviewMaterial
        // inserting rtxplugin call.
        if (vFile.IsHolding<SdfAssetPath>()) {
            path = vFile.Get<SdfAssetPath>().GetResolvedPath();
            if (path.empty()) {
                path = vFile.Get<SdfAssetPath>().GetAssetPath();
            }
        }
        else if (vFile.IsHolding<std::string>()) {
            path = vFile.Get<std::string>();
        }
        // Convert to posix path beause windows backslashes will get lost
        // before reaching the rtx plugin
        path = mx::FilePath(path).asString(mx::FilePath::FormatPosix);

        if (!path.empty()) {
            const std::string ext = ArGetResolver().GetExtension(path);

            mx::NodeGraphPtr mxNodeGraph;
            mx::NodePtr mxTextureNode;
            _FindGraphAndNodeByName(
                mxDoc,
                texturePath.GetParentPath().GetName(),
                mxTextureNodeName,
                &mxNodeGraph,
                &mxTextureNode);

            if (!mxTextureNode) {
                continue;
            }

            // Update texture nodes that use non-native texture formats
            // to read them via a Renderman texture plugin.
            bool needInvertT = false;
            if (TfStringStartsWith(path, "rtxplugin:")) {
                mxTextureNode->setInputValue(
                    _tokens->file.GetText(),       // name
                    path,                          // value
                    _tokens->filename.GetText());  // type
            }
            else if (!ext.empty() && ext != "tex") {
                // Update the input value to use the Renderman texture plugin
                const std::string pluginName =
                    std::string("RtxHioImage") + ARCH_LIBRARY_SUFFIX;

                TfToken uWrap, vWrap;
                _GetWrapModes(netInterface, textureNodeName, &uWrap, &vWrap);

#if PXR_VERSION >= 2402
                TfToken colorSpace = _GetColorSpace(
                    netInterface, textureNodeName, fileParamData);
#else
                TfToken colorSpace =
                    _GetColorSpace(netInterface, textureNodeName);
#endif

                std::string const& mxInputValue = TfStringPrintf(
                    "rtxplugin:%s?filename=%s&wrapS=%s&wrapT=%s&"
                    "sourceColorSpace=%s",
                    pluginName.c_str(),
                    path.c_str(),
                    uWrap.GetText(),
                    vWrap.GetText(),
                    colorSpace.GetText());

                // Update the MaterialX Texture Node with the new mxInputValue
                mxTextureNode->setInputValue(
                    fileParamName.GetText(),       // name
                    mxInputValue,                  // value
                    _tokens->filename.GetText());  // type
            }
            else {
                needInvertT = true;
                // For tex files, update value with resolved path, because prman
                // may not be able to find a usd relative path.
                mxTextureNode->setInputValue(
                    _tokens->file.GetText(),       // name
                    path,                          // value
                    _tokens->filename.GetText());  // type
            }

            // UsdUvTexture nodes and MtlxImage nodes have different
            // names for their texture coordinate connection.
            const TfToken texCoordToken =
                (nodeType == _tokens->ND_UsdUVTexture_23) ? _tokens->st
                                                          : _tokens->texcoord;

            // If texcoord param isn't connected, make a default connection
            // to a mtlx geompropvalue node.
            mx::InputPtr texcoordInput = mxTextureNode->getInput(texCoordToken);
            if (!texcoordInput) {
                // Get the sdr node for the mxTexture node
                SdrRegistry& sdrRegistry = SdrRegistry::GetInstance();
                const SdrShaderNodeConstPtr sdrTextureNode =
                    sdrRegistry.GetShaderNodeByIdentifierAndType(
                        nodeType, _tokens->mtlx);

                // If the node does not already contain a texcoord primvar node
                // add one and connect it to the mxTextureNode
                // XXX If a custom node uses a texture but does not explicitly
                // use a texcoords or geomprop node for the texture coordinates
                // this will force a connection onto the custom node and the
                // material will likely not render.
                if (!_NodeHasTextureCoordPrimvar(mxDoc, sdrTextureNode)) {
                    // Get the primvarname from the sdrTextureNode metadata
                    auto metadata = sdrTextureNode->GetMetadata();
                    auto primvarName = metadata[SdrNodeMetadata->Primvars];

                    // Create a geompropvalue node for the texture coordinates
                    const std::string stNodeName =
                        textureNodeName.GetString() + "__texcoord";
                    mx::NodePtr geompropNode = mxNodeGraph->addNode(
                        _tokens->geompropvalue, stNodeName, _tokens->vector2);
                    geompropNode->setInputValue(
                        _tokens->geomprop, primvarName, _tokens->string_type);
                    geompropNode->setNodeDefString(
                        _tokens->ND_geompropvalue_vector2);

                    // Add the texcoord input and connect to the new node
                    texcoordInput = mxTextureNode->addInput(
                        _tokens->texcoord, _tokens->vector2);
                    texcoordInput->setConnectedNode(geompropNode);
                }
            }
            if (needInvertT) {
                // This inserts standard mtlx nodes to carry out the math
                // for udim aware invert of t; only want to flip
                // the fractional portion of the t value, like this:
                // 2*floor(t) + 1.0 - t
                texcoordInput = mxTextureNode->getInput(texCoordToken);
                if (texcoordInput) {
                    mx::NodePtr primvarNode = texcoordInput->getConnectedNode();
                    const std::string separateNodeName =
                        mxTextureNodeName + "__separate";
                    const std::string floorNodeName =
                        mxTextureNodeName + "__floor";
                    const std::string multiplyNodeName =
                        mxTextureNodeName + "__multiply";
                    const std::string addNodeName = mxTextureNodeName + "__add";
                    const std::string subtractNodeName =
                        mxTextureNodeName + "__subtract";
                    const std::string combineNodeName =
                        mxTextureNodeName + "__combine";

                    mx::NodePtr separateNode = mxNodeGraph->addNode(
                        _tokens->separate2, separateNodeName, _tokens->vector2);
                    separateNode->setNodeDefString(
                        _tokens->ND_separate2_vector2);

                    mx::NodePtr floorNode =
                        mxNodeGraph->addNode(_tokens->floor, floorNodeName);
                    floorNode->setNodeDefString(_tokens->ND_floor_float);

                    mx::NodePtr multiplyNode = mxNodeGraph->addNode(
                        _tokens->multiply, multiplyNodeName);
                    multiplyNode->setNodeDefString(_tokens->ND_multiply_float);

                    mx::NodePtr addNode =
                        mxNodeGraph->addNode(_tokens->add, addNodeName);
                    addNode->setNodeDefString(_tokens->ND_add_float);

                    mx::NodePtr subtractNode = mxNodeGraph->addNode(
                        _tokens->subtract, subtractNodeName);
                    subtractNode->setNodeDefString(_tokens->ND_subtract_float);

                    mx::NodePtr combineNode = mxNodeGraph->addNode(
                        _tokens->combine2, combineNodeName);
                    combineNode->setNodeDefString(_tokens->ND_combine2_vector2);

                    mx::InputPtr separateNode_inInput =
                        separateNode->addInput(_tokens->in, _tokens->vector2);
                    mx::OutputPtr separateNode_outxOutput =
                        separateNode->addOutput(_tokens->outx);
                    mx::OutputPtr separateNode_outyOutput =
                        separateNode->addOutput(_tokens->outy);
                    separateNode_inInput->setConnectedNode(primvarNode);

                    mx::InputPtr floorNode_inInput =
                        floorNode->addInput(_tokens->in);
                    mx::OutputPtr floorNode_outOutput =
                        floorNode->addOutput(_tokens->out);
                    floorNode_inInput->setConnectedNode(separateNode);
                    floorNode_inInput->setConnectedOutput(
                        separateNode_outyOutput);

                    mx::InputPtr multiplyNode_in1Input =
                        multiplyNode->addInput(_tokens->in1);
                    mx::OutputPtr multiplyNode_outOutput =
                        multiplyNode->addOutput(_tokens->out);
                    multiplyNode_in1Input->setConnectedNode(floorNode);
                    multiplyNode->setInputValue(_tokens->in2, 2);

                    mx::InputPtr addNode_in1Input =
                        addNode->addInput(_tokens->in1);
                    mx::OutputPtr addNode_outOutput =
                        addNode->addOutput(_tokens->out);
                    addNode_in1Input->setConnectedNode(multiplyNode);
                    addNode->setInputValue(_tokens->in2, 1);

                    mx::InputPtr subtractNode_in1Input =
                        subtractNode->addInput(_tokens->in1);
                    mx::InputPtr subtractNode_in2Input =
                        subtractNode->addInput(_tokens->in2);
                    mx::OutputPtr subtractNode_outOutput =
                        subtractNode->addOutput(_tokens->out);
                    subtractNode_in1Input->setConnectedNode(addNode);
                    subtractNode_in2Input->setConnectedNode(separateNode);
                    subtractNode_in2Input->setConnectedOutput(
                        separateNode_outyOutput);

                    mx::InputPtr combineNode_in1Input =
                        combineNode->addInput(_tokens->in1);
                    mx::InputPtr combineNode_in2Input =
                        combineNode->addInput(_tokens->in2);
                    mx::OutputPtr combineNode_outOutput =
                        combineNode->addOutput(_tokens->out, _tokens->vector2);
                    combineNode_in1Input->setConnectedNode(separateNode);
                    combineNode_in2Input->setConnectedNode(subtractNode);
                    texcoordInput->setConnectedNode(combineNode);
                }
            }
        }
    }
}

void _FixNodeTypes(HdMaterialNetwork2Interface* netInterface)
{
    const TfTokenVector nodeNames = netInterface->GetNodeNames();
    for (TfToken const& nodeName : nodeNames) {
        TfToken nodeType = netInterface->GetNodeType(nodeName);

        if (TfStringStartsWith(nodeType.GetText(), "Usd")) {
            if (nodeType == _tokens->UsdPrimvarReader_float2) {
                nodeType = _tokens->ND_UsdPrimvarReader_vector2;
            }
            else if (nodeType == _tokens->UsdVerticalFlip) {
                nodeType = _tokens->ND_dot_vector2;  // pass through node
            }
            else {
                nodeType = _FixSingleType(nodeType);
            }
            netInterface->SetNodeType(nodeName, nodeType);
        }
    }
}

void _FixNodeValues(HdMaterialNetwork2Interface* netInterface)
{
    // Fix textures wrap mode from repeat to periodic, because MaterialX does
    // not support repeat mode.
    const TfTokenVector nodeNames = netInterface->GetNodeNames();

    for (TfToken const& nodeName : nodeNames) {
        auto params = netInterface->GetAuthoredNodeParameterNames(nodeName);

        auto input_connections =
            netInterface->GetNodeInputConnectionNames(nodeName);

        TfToken nodeType = netInterface->GetNodeType(nodeName);
        if (nodeType == _tokens->ND_UsdUVTexture_23) {
            VtValue wrapS =
                netInterface->GetNodeParameterValue(nodeName, _tokens->wrapS);
            VtValue wrapT =
                netInterface->GetNodeParameterValue(nodeName, _tokens->wrapT);
            if (wrapS.IsHolding<TfToken>() && wrapT.IsHolding<TfToken>()) {
                TfToken wrapSValue = wrapS.Get<TfToken>();
                TfToken wrapTValue = wrapT.Get<TfToken>();
                if (wrapSValue == _tokens->repeat) {
                    netInterface->SetNodeParameterValue(
                        nodeName, _tokens->wrapS, VtValue(_tokens->periodic));
                }
                if (wrapTValue == _tokens->repeat) {
                    netInterface->SetNodeParameterValue(
                        nodeName, _tokens->wrapT, VtValue(_tokens->periodic));
                }
            }
        }

        if (nodeType == _tokens->ND_UsdPreviewSurface_surfaceshader) {
            // Delete the 'specular' input from UsdPreviewSurface if exists

            auto specularInput = netInterface->GetNodeParameterValue(
                nodeName, pxr::TfToken("specular"));

            if (!specularInput.IsEmpty()) {
                netInterface->DeleteNodeParameter(
                    nodeName, pxr::TfToken("specular"));
            }

            auto metallicInput = netInterface->GetNodeInputConnection(
                nodeName, pxr::TfToken("metallic"));

            if (!metallicInput.empty()) {
                auto input = metallicInput[0];
                auto connected_param = netInterface->GetNodeParameterData(
                    input.upstreamNodeName, input.upstreamOutputName);

                if (connected_param.value.IsHolding<GfVec3f>()) {
                    GfVec3f value = connected_param.value.Get<GfVec3f>();
                    netInterface->SetNodeParameterValue(
                        input.upstreamNodeName,
                        input.upstreamNodeName,
                        VtValue(value[0]));
                }
            }

            auto opacityThresholdInput = netInterface->GetNodeParameterValue(
                nodeName, pxr::TfToken("opacityThreshold"));
            if (!opacityThresholdInput.IsEmpty()) {
                auto v = opacityThresholdInput.Get<float>();

                if (v > 0.00001)
                    spdlog::info("opacityThreshold: %f", v);
            }
        }
    }
}

void _FixOmittedConnections(
    mx::DocumentPtr const& mxDoc,
    const std::vector<mx::TypedElementPtr>& renderableElements)
{
    // Static cache for NodeDef lookups - persists across all material
    // processing
    static std::unordered_map<std::string, mx::NodeDefPtr> nodedef_cache;
    static std::unordered_map<std::string, mx::NodeDefPtr> rough_nodedef_cache;
    static std::mutex cache_mutex;

    // Recursively checks and fixes nodes and their upstream connections
    std::function<void(mx::NodePtr, std::set<mx::NodePtr>&)>
        processNodeRecursively =
            [&mxDoc, &processNodeRecursively](
                mx::NodePtr node, std::set<mx::NodePtr>& visitedNodes) -> void {
        // Avoid infinite recursion by tracking visited nodes
        if (visitedNodes.find(node) != visitedNodes.end()) {
            return;
        }
        visitedNodes.insert(node);

        // Create cache key from node category and type
        std::string cache_key = node->getCategory() + ":" + node->getType();

        // Check cache first before expensive getNodeDef call
        mx::NodeDefPtr node_def;
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            auto cache_it = nodedef_cache.find(cache_key);
            if (cache_it != nodedef_cache.end()) {
                node_def = cache_it->second;
            }
            else {
                spdlog::info(
                    "NodeDef cache miss for '{}', searching 3000+ "
                    "definitions...",
                    cache_key);
                node_def = node->getNodeDef(mx::EMPTY_STRING);
                nodedef_cache[cache_key] = node_def;
            }
        }

        mx::NodeDefPtr rough_node_def;
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            auto rough_cache_it = rough_nodedef_cache.find(cache_key);
            if (rough_cache_it != rough_nodedef_cache.end()) {
                rough_node_def = rough_cache_it->second;
            }
            else {
                spdlog::info(
                    "Rough NodeDef cache miss for '{}', searching 3000+ "
                    "definitions...",
                    cache_key);
                rough_node_def = node->getNodeDef(mx::EMPTY_STRING, true);
                rough_nodedef_cache[cache_key] = rough_node_def;
            }
        }

        if (rough_node_def && !node_def) {
            // Process each input on this node. getInputs() returns a vector
            // COPY (MaterialX), so removing inputs inside the loop cannot
            // invalidate the iteration — keep it that way if this is ever
            // switched to a reference.
            for (auto input : node->getInputs()) {
                auto input_in_def = rough_node_def->getInput(input->getName());

                if (!input_in_def) {
                    // Strict getNodeDef requires an exact input match, so an
                    // authored name absent from the def breaks shader
                    // generation (e.g. legacy `roughness` vs
                    // `specular_roughness` in standard_surface >= 1.38).
                    // Map known legacy aliases, drop anything else — the
                    // shader then uses the def's default for that input.
                    static const std::unordered_map<std::string, std::string>
                        legacy_aliases = {
                            { "roughness", "specular_roughness" },
                        };
                    auto alias_it = legacy_aliases.find(input->getName());
                    if (alias_it != legacy_aliases.end() &&
                        !node->getInput(alias_it->second)) {
                        auto def_input =
                            rough_node_def->getInput(alias_it->second);
                        if (def_input) {
                            auto renamed = node->addInput(
                                alias_it->second, def_input->getType());
                            if (input->hasValue()) {
                                renamed->setValueString(
                                    input->getValueString());
                            }
                            auto upstream = input->getConnectedOutput();
                            if (upstream) {
                                renamed->setConnectedOutput(upstream);
                            }
                        }
                    }
                    node->removeInput(input->getName());
                    continue;
                }

                if (input_in_def->getType() != input->getType()) {
                    spdlog::info("Fixing skipped link.");

                    // Get required information
                    auto upstream = input->getConnectedOutput();
                    auto parent = node->getParent();
                    mx::GraphElementPtr graphParent = nullptr;

                    if (parent->isA<mx::NodeGraph>()) {
                        graphParent = parent->asA<mx::NodeGraph>();
                    }
                    else if (parent->isA<mx::Document>()) {
                        graphParent = parent->asA<mx::Document>();
                    }

                    if (!graphParent || !upstream) {
                        continue;
                    }

                    // Handle type conversion cases
                    std::string nodeName, nodeDefString;

                    if (input_in_def->getType() == mx::Type::FLOAT.getName() &&
                        input->getType() == mx::Type::COLOR3.getName()) {
                        nodeName = _tokens->separate3;
                        nodeDefString = _tokens->ND_separate3_color3;
                    }
                    else if (
                        input_in_def->getType() ==
                            mx::Type::VECTOR3.getName() &&
                        input->getType() == mx::Type::COLOR3.getName()) {
                        nodeName = "convert";
                        nodeDefString = _tokens->ND_convert_color3_vector3;
                    }
                    else {
                        continue;
                    }

                    // Create conversion node with unique name for shared
                    // document
                    std::string baseConversionName =
                        upstream->getName() + "_conversion";
                    std::string uniqueConversionName =
                        graphParent->createValidChildName(baseConversionName);
                    auto conversionNode =
                        graphParent->addNode(nodeName, uniqueConversionName);
                    conversionNode->setNodeDefString(nodeDefString);
                    conversionNode->addInputsFromNodeDef();

                    // Add outputs from node definition
                    auto conversionDef =
                        conversionNode->getNodeDef(mx::EMPTY_STRING, true);
                    for (auto output : conversionDef->getActiveOutputs()) {
                        conversionNode->addOutput(
                            output->getName(), output->getType());
                    }

                    // Connect nodes
                    std::string outputName =
                        (nodeName == _tokens->separate3) ? "outr" : "out";
                    input->setConnectedOutput(
                        conversionNode->getOutput(outputName));
                    input->setType(input_in_def->getType());
                    conversionNode->getInput("in")->setConnectedOutput(
                        upstream);
                }
            }
        }

        for (auto input : node->getInputs()) {
            auto node = input->getConnectedNode();
            if (node) {
                processNodeRecursively(node, visitedNodes);
            }
        }
    };

    // Start processing from each renderable element
    for (auto const& elem : renderableElements) {
        // Skip nodes that aren't actually shaders
        if (!elem->isA<mx::Node>()) {
            continue;
        }

        mx::NodePtr node = elem->asA<mx::Node>();
        std::set<mx::NodePtr> visitedNodes;
        processNodeRecursively(node, visitedNodes);
    }
}

HdMaterialNode2 const* _GetTerminalNode(
    HdMaterialNetwork2 const& network,
    TfToken const& terminalName,
    SdfPath* terminalNodePath)
{
    // Get the Surface or Volume Terminal
    auto const& terminalConnIt = network.terminals.find(terminalName);

    if (terminalConnIt == network.terminals.end()) {
        return nullptr;
    }
    HdMaterialConnection2 const& connection = terminalConnIt->second;
    SdfPath const& terminalPath = connection.upstreamNode;
    auto const& terminalIt = network.nodes.find(terminalPath);
    *terminalNodePath = terminalPath;
    return &terminalIt->second;
}

RUZINO_NAMESPACE_CLOSE_SCOPE
