//
// Copyright 2026 Ruzino
//
// Licensed under the terms set forth in the LICENSE.txt file.
//
// Implementation of the Hydra 2.0 direct-read helpers declared in
// hydra2Ingest.h. Every translation mirrors the corresponding code in
// HdSceneIndexAdapterSceneDelegate (OpenUSD 26.08); see the header for the
// fallback contract. (An A/B gate that forced the legacy pull via
// RZ_HYDRA2_PREFER_LEGACY=1 lived here until the migration passed
// pixel-parity validation on 2026-09-02, then was removed.)
//
#include "hydra2Ingest.h"

// Schema headers: include order matters for a few generated headers
// (materialNetworkSchema expects materialConnectionSchema first, and the
// generated token declarations need TF_DECLARE_PUBLIC_TOKENS from
// staticTokens.h to already be in scope). Windows COM headers
// (#define interface struct — pulled in transitively by RHI/nvrhi) must be
// neutralized around these includes: the "interface" token name appears in
// several generated token lists. Keeping all of this here confines the
// hazards to this single TU.
#pragma push_macro("interface")
#ifdef interface
#undef interface
#endif
#include "pxr/base/tf/staticTokens.h"
#include "pxr/imaging/hd/capsuleSchema.h"
#include "pxr/imaging/hd/coneSchema.h"
#include "pxr/imaging/hd/cubeSchema.h"
#include "pxr/imaging/hd/cylinderSchema.h"
#include "pxr/imaging/hd/dataSource.h"
#include "pxr/imaging/hd/dataSourceLegacyPrim.h"
#include "pxr/imaging/hd/geomSubset.h"
#include "pxr/imaging/hd/geomSubsetSchema.h"
#include "pxr/imaging/hd/instancerTopologySchema.h"
#include "pxr/imaging/hd/lightSchema.h"
#include "pxr/imaging/hd/materialBindingSchema.h"
#include "pxr/imaging/hd/materialBindingsSchema.h"
#include "pxr/imaging/hd/materialConnectionSchema.h"
#include "pxr/imaging/hd/materialNetworkSchema.h"
#include "pxr/imaging/hd/materialNodeParameterSchema.h"
#include "pxr/imaging/hd/materialNodeSchema.h"
#include "pxr/imaging/hd/materialSchema.h"
#include "pxr/imaging/hd/meshSchema.h"
#include "pxr/imaging/hd/primvarSchema.h"
#include "pxr/imaging/hd/primvarsSchema.h"
#include "pxr/imaging/hd/purposeSchema.h"
#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/imaging/hd/sphereSchema.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/visibilitySchema.h"
#include "pxr/imaging/hd/volumeFieldSchema.h"
#include "pxr/imaging/hd/xformSchema.h"
#include "pxr/imaging/pxOsd/tokens.h"
#pragma pop_macro("interface")

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <unordered_set>

PXR_NAMESPACE_OPEN_SCOPE

namespace Ruzino_Hydra2 {

namespace {

    // The terminal scene index of the render index this delegate serves. Null
    // in contexts without scene index emulation (then every reader below
    // no-ops).
    HdSceneIndexBaseRefPtr GetTerminalSceneIndex(HdSceneDelegate* sceneDelegate)
    {
        if (!sceneDelegate) {
            return HdSceneIndexBaseRefPtr();
        }
        return sceneDelegate->GetRenderIndex().GetTerminalSceneIndex();
    }

    // Mirrors HdPrimvarDescriptorFromSchema (adapter-internal): name from the
    // primvars container key, interpolation/role/indexed from the primvar
    // schema. Invalid interpolation maps to HdInterpolationCount, which callers
    // must skip (same as the adapter).
    HdPrimvarDescriptor _PrimvarDescriptorFromSchema(
        TfToken const& name,
        HdPrimvarSchema const& primvar)
    {
        HdPrimvarDescriptor desc;
        desc.name = name;

        HdTokenDataSourceHandle interpolationDs = primvar.GetInterpolation();
        if (!interpolationDs) {
            desc.interpolation = HdInterpolationCount;
            return desc;
        }
        TfToken const& interp = interpolationDs->GetTypedValue(0.0f);

        // Mirrors Hd_InterpolationAsEnum in the adapter.
        if (interp == HdPrimvarSchemaTokens->constant) {
            desc.interpolation = HdInterpolationConstant;
        }
        else if (interp == HdPrimvarSchemaTokens->uniform) {
            desc.interpolation = HdInterpolationUniform;
        }
        else if (interp == HdPrimvarSchemaTokens->varying) {
            desc.interpolation = HdInterpolationVarying;
        }
        else if (interp == HdPrimvarSchemaTokens->vertex) {
            desc.interpolation = HdInterpolationVertex;
        }
        else if (interp == HdPrimvarSchemaTokens->faceVarying) {
            desc.interpolation = HdInterpolationFaceVarying;
        }
        else if (interp == HdPrimvarSchemaTokens->instance) {
            desc.interpolation = HdInterpolationInstance;
        }
        else {
            desc.interpolation = HdInterpolationCount;
        }

        if (HdTokenDataSourceHandle roleDs = primvar.GetRole()) {
            desc.role = roleDs->GetTypedValue(0.0f);
        }
        desc.indexed = primvar.IsIndexed();
        return desc;
    }

    // Mirrors the adapter's _IsLegacyInstancer: an instancer prim flagged
    // isLegacyInstancer in its instancerTopology container. Legacy scene
    // delegates hide such instancers by removing prototypes, so visibility is
    // ignored for them entirely.
    bool _IsLegacyInstancer(HdSceneIndexPrim const& prim)
    {
        if (prim.primType != HdPrimTypeTokens->instancer) {
            return false;
        }
        HdContainerDataSourceHandle const container =
            HdInstancerTopologySchema::GetFromParent(prim.dataSource)
                .GetContainer();
        if (!container) {
            return false;
        }
        HdBoolDataSourceHandle const ds = HdBoolDataSource::Cast(
            container->Get(HdLegacyFlagTokens->isLegacyInstancer));
        if (!ds) {
            return false;
        }
        return ds->GetTypedValue(0.0f);
    }

    bool _IsVisibleSource(HdContainerDataSourceHandle const& primSource)
    {
        if (HdVisibilitySchema visSchema =
                HdVisibilitySchema::GetFromParent(primSource)) {
            if (HdBoolDataSourceHandle const visDs =
                    visSchema.GetVisibility()) {
                return visDs->GetTypedValue(0.0f);
            }
        }
        return true;
    }

    SdfPath _GetBoundMaterialPath(
        HdContainerDataSourceHandle const& ds,
        TfToken const& purpose)
    {
        if (HdMaterialBindingsSchema const bindingsSchema =
                HdMaterialBindingsSchema::GetFromParent(ds)) {
            if (HdMaterialBindingSchema const bindingSchema =
                    bindingsSchema.GetMaterialBinding(purpose)) {
                if (HdPathDataSourceHandle const pathDs =
                        bindingSchema.GetPath()) {
                    return pathDs->GetTypedValue(0.0f);
                }
            }
        }
        return SdfPath::EmptyPath();
    }

    VtIntArray _UnionIntArray(VtIntArray const& a, VtIntArray const& b)
    {
        if (a.empty()) {
            return b;
        }
        if (b.empty()) {
            return a;
        }
        VtIntArray out = a;
        out.reserve(out.size() + b.size());
        for (int const v : b) {
            out.push_back(v);
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    // Mirrors the adapter's _GatherGeomSubsets: all subsets are direct children
    // of the geometry prim. Invisible subsets union into invisible
    // faces/points; visible ones with a material binding become face-set
    // subsets.
    void _GatherGeomSubsets(
        SdfPath const& parentPath,
        HdSceneIndexBaseRefPtr const& sceneIndex,
        TfToken const& materialBindingPurpose,
        HdMeshTopology* topology)
    {
        HdGeomSubsets subsets;
        for (SdfPath const& childPath :
             sceneIndex->GetChildPrimPaths(parentPath)) {
            HdSceneIndexPrim const& child = sceneIndex->GetPrim(childPath);
            if (child.primType != HdPrimTypeTokens->geomSubset ||
                child.dataSource == nullptr) {
                continue;
            }
            HdGeomSubsetSchema const& schema =
                HdGeomSubsetSchema::GetFromParent(child.dataSource);
            if (!schema.IsDefined()) {
                continue;
            }
            HdTokenDataSourceHandle const typeDs = schema.GetType();
            if (!typeDs) {
                continue;
            }
            TfToken const type = typeDs->GetTypedValue(0.0f);
            HdIntArrayDataSourceHandle const indicesDs = schema.GetIndices();
            static const VtIntArray emptyIndices;
            VtIntArray const indices =
                indicesDs ? indicesDs->GetTypedValue(0.0f) : emptyIndices;
            if (!_IsVisibleSource(child.dataSource)) {
                if (type == HdGeomSubsetSchemaTokens->typeFaceSet) {
                    topology->SetInvisibleFaces(
                        _UnionIntArray(topology->GetInvisibleFaces(), indices));
                }
                else if (type == HdGeomSubsetSchemaTokens->typePointSet) {
                    topology->SetInvisiblePoints(_UnionIntArray(
                        topology->GetInvisiblePoints(), indices));
                }
                continue;
            }
            SdfPath const materialId =
                _GetBoundMaterialPath(child.dataSource, materialBindingPurpose);
            if (materialId.IsEmpty()) {
                continue;
            }
            subsets.push_back(
                { HdGeomSubset::Type::TypeFaceSet,
                  childPath,
                  materialId,
                  indices });
        }
        topology->SetGeomSubsets(subsets);
    }

    VtDictionary _SchemaToDictionary(
        HdSampledDataSourceContainerSchema const& schema)
    {
        VtDictionary dict;
        for (TfToken const& name : schema.GetNames()) {
            if (HdSampledDataSourceHandle const valueDs = schema.Get(name)) {
                dict[name.GetString()] = valueDs->GetValue(0);
            }
        }
        return dict;
    }

    std::map<TfToken, VtValue> _GetHdParamsFromDataSource(
        HdMaterialNodeParameterContainerSchema const& containerSchema)
    {
        std::map<TfToken, VtValue> hdParams;
        if (!containerSchema) {
            return hdParams;
        }
        TfTokenVector const pNames = containerSchema.GetNames();
        for (TfToken const& pName : pNames) {
            HdMaterialNodeParameterSchema const paramSchema =
                containerSchema.Get(pName);
            if (!paramSchema) {
                continue;
            }
            // Parameter Value
            if (HdSampledDataSourceHandle const paramValueDS =
                    paramSchema.GetValue()) {
                hdParams[pName] = paramValueDS->GetValue(0);
            }
            // ColorSpace Metadata (key = "colorSpace:<param>", only when set)
            if (HdTokenDataSourceHandle const colorSpaceDS =
                    paramSchema.GetColorSpace()) {
                TfToken const cspName(
                    SdfPath::JoinIdentifier(
                        HdMaterialNodeParameterSchemaTokens->colorSpace,
                        pName));
                TfToken const colorSpaceToken = colorSpaceDS->GetTypedValue(0);
                if (!colorSpaceToken.IsEmpty()) {
                    hdParams[cspName] = VtValue(colorSpaceToken);
                }
            }
            // TypeName Metadata (key = "typeName:<param>", only when set)
            if (HdTokenDataSourceHandle const typeNameDs =
                    paramSchema.GetTypeName()) {
                TfToken const typName(
                    SdfPath::JoinIdentifier(
                        HdMaterialNodeParameterSchemaTokens->typeName, pName));
                TfToken const typeNameToken = typeNameDs->GetTypedValue(0);
                if (!typeNameToken.IsEmpty()) {
                    hdParams[typName] = VtValue(typeNameToken);
                }
            }
        }
        return hdParams;
    }

    void _WalkMaterialNetwork(
        SdfPath const& nodePath,
        HdMaterialNodeContainerSchema const& nodesSchema,
        TfTokenVector const& renderContexts,
        std::unordered_set<SdfPath, SdfPath::Hash>* visitedSet,
        HdMaterialNetwork* netHd)
    {
        if (visitedSet->find(nodePath) != visitedSet->end()) {
            return;
        }
        visitedSet->insert(nodePath);

        TfToken const nodePathTk(nodePath.GetToken());

        HdMaterialNodeSchema nodeSchema = nodesSchema.Get(nodePathTk);
        if (!nodeSchema.IsDefined()) {
            return;
        }

        TfToken nodeId;
        if (HdTokenDataSourceHandle const idDs =
                nodeSchema.GetNodeIdentifier()) {
            nodeId = idDs->GetTypedValue(0);
        }

        // Render-context specific node identifiers take precedence.
        if (!renderContexts.empty()) {
            if (HdContainerDataSourceHandle const idsDs =
                    nodeSchema.GetRenderContextNodeIdentifiers()) {
                for (TfToken const& name : renderContexts) {
                    if (name.IsEmpty() && !nodeId.IsEmpty()) {
                        break;
                    }
                    if (HdTokenDataSourceHandle const ds =
                            HdTokenDataSource::Cast(idsDs->Get(name))) {
                        TfToken const v = ds->GetTypedValue(0);
                        if (!v.IsEmpty()) {
                            nodeId = v;
                            break;
                        }
                    }
                }
            }
        }

        if (HdMaterialConnectionVectorContainerSchema const
                vectorContainerSchema = nodeSchema.GetInputConnections()) {
            TfTokenVector const connsNames = vectorContainerSchema.GetNames();
            for (TfToken const& connName : connsNames) {
                HdMaterialConnectionVectorSchema const vectorSchema =
                    vectorContainerSchema.Get(connName);
                if (!vectorSchema) {
                    continue;
                }
                for (size_t i = 0; i < vectorSchema.GetNumElements(); i++) {
                    HdMaterialConnectionSchema const connSchema =
                        vectorSchema.GetElement(i);
                    if (!connSchema.IsDefined()) {
                        continue;
                    }
                    // Upstream dereferences these without null checks; skipping
                    // malformed connections keeps a bad network from crashing
                    // the renderer instead of just rendering without them.
                    if (!connSchema.GetUpstreamNodePath() ||
                        !connSchema.GetUpstreamNodeOutputName()) {
                        continue;
                    }
                    TfToken const p =
                        connSchema.GetUpstreamNodePath()->GetTypedValue(0);
                    TfToken const n =
                        connSchema.GetUpstreamNodeOutputName()->GetTypedValue(
                            0);
                    _WalkMaterialNetwork(
                        SdfPath(p.GetString()),
                        nodesSchema,
                        renderContexts,
                        visitedSet,
                        netHd);

                    HdMaterialRelationship r;
                    r.inputId = SdfPath(p.GetString());
                    r.inputName = n;
                    r.outputId = nodePath;
                    r.outputName = connName;
                    netHd->relationships.push_back(r);
                }
            }
        }

        HdMaterialNode n;
        n.identifier = nodeId;
        n.path = nodePath;
        n.parameters = _GetHdParamsFromDataSource(nodeSchema.GetParameters());
        netHd->nodes.push_back(n);
    }

    HdMaterialNetworkMap _ToMaterialNetworkMap(
        HdMaterialNetworkSchema const& netSchema,
        TfTokenVector const& renderContexts)
    {
        bool includeDisconnectedNodes = false;
        if (HdContainerDataSourceHandle const netContainer =
                netSchema.GetContainer()) {
            static const TfToken key("includeDisconnectedNodes");
            if (HdBoolDataSourceHandle const ds =
                    HdBoolDataSource::Cast(netContainer->Get(key))) {
                includeDisconnectedNodes = ds->GetTypedValue(0.0f);
            }
        }

        HdMaterialNetworkMap matHd;
        std::unordered_set<SdfPath, SdfPath::Hash> visitedNodes;

        HdMaterialNodeContainerSchema const nodesSchema = netSchema.GetNodes();
        HdMaterialConnectionContainerSchema const terminalsSchema =
            netSchema.GetTerminals();
        TfTokenVector const names = terminalsSchema.GetNames();

        if (HdSampledDataSourceContainerSchema const config =
                netSchema.GetConfig()) {
            matHd.config = _SchemaToDictionary(config);
        }

        for (TfToken const& name : names) {
            visitedNodes.clear();

            HdMaterialConnectionSchema const connSchema =
                terminalsSchema.Get(name);
            if (!connSchema.IsDefined() || !connSchema.GetUpstreamNodePath()) {
                continue;
            }
            TfToken const pathTk =
                connSchema.GetUpstreamNodePath()->GetTypedValue(0);
            if (pathTk.IsEmpty()) {
                continue;
            }
            SdfPath const path(pathTk.GetString());
            matHd.terminals.push_back(path);

            HdMaterialNetwork& netHd = matHd.map[name];
            _WalkMaterialNetwork(
                path, nodesSchema, renderContexts, &visitedNodes, &netHd);

            if (includeDisconnectedNodes && nodesSchema) {
                for (TfToken const& nodeName : nodesSchema.GetNames()) {
                    _WalkMaterialNetwork(
                        SdfPath(nodeName.GetString()),
                        nodesSchema,
                        renderContexts,
                        &visitedNodes,
                        &netHd);
                }
            }
        }

        return matHd;
    }

}  // namespace

bool ReadPrimvar(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    TfToken const& name,
    VtValue* out)
{
    *out = VtValue();
    HdSceneIndexBaseRefPtr terminal = GetTerminalSceneIndex(sceneDelegate);
    if (!terminal) {
        return false;
    }
    HdSceneIndexPrim prim = terminal->GetPrim(id);
    if (!prim.dataSource) {
        return false;
    }
    // A legacy-emulation prim serves Get() from its underlying delegate (the
    // adapter's trailing fallback); defer to the legacy pull for those.
    if (prim.dataSource->Get(HdSceneIndexEmulationTokens->sceneDelegate)) {
        return false;
    }
    HdPrimvarsSchema primvars =
        HdPrimvarsSchema::GetFromParent(prim.dataSource);
    if (!primvars) {
        // No primvars container: the adapter's Get() returns empty for the
        // primvar. Served, not authored.
        return true;
    }
    HdPrimvarSchema primvar = primvars.GetPrimvar(name);
    if (!primvar) {
        return true;
    }
    if (HdSampledDataSourceHandle valueDs = primvar.GetPrimvarValue()) {
        *out = valueDs->GetValue(0.0f);
    }
    return true;
}

bool ReadLightParam(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    TfToken const& paramName,
    VtValue* out)
{
    *out = VtValue();
    HdSceneIndexBaseRefPtr terminal = GetTerminalSceneIndex(sceneDelegate);
    if (!terminal) {
        return false;
    }
    HdSceneIndexPrim prim = terminal->GetPrim(id);
    if (!prim.dataSource) {
        return false;
    }
    HdContainerDataSourceHandle light = HdContainerDataSource::Cast(
        prim.dataSource->Get(HdLightSchemaTokens->light));
    if (!light) {
        // GetLightParamValue has no other fallback in the adapter; an
        // unauthored parameter is served as empty.
        return true;
    }
    if (HdSampledDataSourceHandle valueDs =
            HdSampledDataSource::Cast(light->Get(paramName))) {
        *out = valueDs->GetValue(0);
    }
    return true;
}

bool ReadVolumeFieldParam(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    TfToken const& paramName,
    VtValue* out)
{
    *out = VtValue();
    HdSceneIndexBaseRefPtr terminal = GetTerminalSceneIndex(sceneDelegate);
    if (!terminal) {
        return false;
    }
    HdSceneIndexPrim prim = terminal->GetPrim(id);
    if (!prim.dataSource) {
        return false;
    }
    // The adapter's Get() serves the volumeField container only for
    // volume-field prim types; other types flow into its remaining branches,
    // so the caller must take the legacy path. HdLegacyPrimTypeIsVolumeField
    // is not exported from the monolithic usd_ms build, so the comparison is
    // inlined here, mirroring its body (dataSourceLegacyPrim.cpp).
    if (prim.primType != HdLegacyPrimTypeTokens->openvdbAsset &&
        prim.primType != HdLegacyPrimTypeTokens->field3dAsset) {
        return false;
    }
    HdContainerDataSourceHandle volumeField = HdContainerDataSource::Cast(
        prim.dataSource->Get(HdVolumeFieldSchemaTokens->volumeField));
    if (!volumeField) {
        return true;
    }
    if (HdSampledDataSourceHandle valueDs =
            HdSampledDataSource::Cast(volumeField->Get(paramName))) {
        *out = valueDs->GetValue(0);
    }
    return true;
}

bool ReadTransform(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    GfMatrix4d* outTransform)
{
    *outTransform = GfMatrix4d(1.0);
    HdSceneIndexBaseRefPtr terminal = GetTerminalSceneIndex(sceneDelegate);
    if (!terminal) {
        return false;
    }
    HdSceneIndexPrim prim = terminal->GetPrim(id);
    if (!prim.dataSource) {
        return false;
    }
    HdXformSchema xformSchema = HdXformSchema::GetFromParent(prim.dataSource);
    if (!xformSchema) {
        // Schema absent means "identity" to the adapter translation, which
        // is exactly what we pre-set above. This is a valid read.
        return true;
    }
    if (HdMatrixDataSourceHandle matrixDs = xformSchema.GetMatrix()) {
        *outTransform = matrixDs->GetTypedValue(0.0f);
        return true;
    }
    return true;
}

bool ReadMaterialId(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    SdfPath* outMaterialId)
{
    HdSceneIndexBaseRefPtr terminal = GetTerminalSceneIndex(sceneDelegate);
    if (!terminal) {
        return false;
    }
    HdSceneIndexPrim prim = terminal->GetPrim(id);
    if (!prim.dataSource) {
        return false;
    }
    HdMaterialBindingsSchema materialBindings =
        HdMaterialBindingsSchema::GetFromParent(prim.dataSource);
    if (!materialBindings) {
        return false;
    }
    TfToken const& purpose = sceneDelegate->GetRenderIndex()
                                 .GetRenderDelegate()
                                 ->GetMaterialBindingPurpose();
    HdMaterialBindingSchema materialBinding =
        materialBindings.GetMaterialBinding(purpose);
    if (HdPathDataSourceHandle const ds = materialBinding.GetPath()) {
        *outMaterialId = ds->GetTypedValue(0.0f);
        return true;
    }
    return false;
}

bool ReadPrimvarDescriptors(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    HdPrimvarDescriptorVector* out)
{
    out->clear();
    HdSceneIndexBaseRefPtr terminal = GetTerminalSceneIndex(sceneDelegate);
    if (!terminal) {
        return false;
    }
    HdSceneIndexPrim prim = terminal->GetPrim(id);
    if (!prim.dataSource) {
        return false;
    }
    HdPrimvarsSchema primvars =
        HdPrimvarsSchema::GetFromParent(prim.dataSource);
    if (!primvars) {
        return false;
    }
    for (TfToken const& name : primvars.GetPrimvarNames()) {
        HdPrimvarSchema primvar = primvars.GetPrimvar(name);
        if (!primvar) {
            continue;
        }
        HdPrimvarDescriptor desc = _PrimvarDescriptorFromSchema(name, primvar);
        if (desc.interpolation == HdInterpolationCount) {
            continue;
        }
        out->push_back(std::move(desc));
    }
    return true;
}

bool ReadPrimvarDescriptors(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    HdInterpolation interpolation,
    HdPrimvarDescriptorVector* out)
{
    HdPrimvarDescriptorVector all;
    if (!ReadPrimvarDescriptors(sceneDelegate, id, &all)) {
        return false;
    }
    out->clear();
    for (auto& desc : all) {
        if (desc.interpolation == interpolation) {
            out->push_back(std::move(desc));
        }
    }
    return true;
}

bool ReadVisible(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    bool* outVisible)
{
    HdSceneIndexBaseRefPtr terminal = GetTerminalSceneIndex(sceneDelegate);
    if (!terminal) {
        return false;
    }
    HdSceneIndexPrim prim = terminal->GetPrim(id);
    if (!prim.dataSource) {
        return false;
    }
    if (_IsLegacyInstancer(prim)) {
        *outVisible = true;
        return true;
    }
    HdVisibilitySchema visibilitySchema =
        HdVisibilitySchema::GetFromParent(prim.dataSource);
    if (!visibilitySchema.IsDefined()) {
        *outVisible = true;
        return true;
    }
    if (HdBoolDataSourceHandle const visDs = visibilitySchema.GetVisibility()) {
        *outVisible = visDs->GetTypedValue(0.0f);
        return true;
    }
    *outVisible = true;
    return true;
}

bool ReadInstanceIndices(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& instancerId,
    SdfPath const& prototypeId,
    VtIntArray* out)
{
    HdSceneIndexBaseRefPtr terminal = GetTerminalSceneIndex(sceneDelegate);
    if (!terminal) {
        return false;
    }
    HdSceneIndexPrim prim = terminal->GetPrim(instancerId);
    if (!prim.dataSource) {
        return false;
    }
    HdInstancerTopologySchema instancerTopology =
        HdInstancerTopologySchema::GetFromParent(prim.dataSource);
    if (!instancerTopology) {
        return false;
    }
    *out = instancerTopology.ComputeInstanceIndicesForProto(prototypeId);
    return true;
}

bool ReadMeshTopology(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    HdMeshTopology* out)
{
    HdSceneIndexBaseRefPtr terminal = GetTerminalSceneIndex(sceneDelegate);
    if (!terminal) {
        return false;
    }
    HdSceneIndexPrim prim = terminal->GetPrim(id);
    if (!prim.dataSource) {
        return false;
    }
    HdMeshSchema meshSchema = HdMeshSchema::GetFromParent(prim.dataSource);
    if (!meshSchema.IsDefined()) {
        return false;
    }
    HdMeshTopologySchema meshTopologySchema = meshSchema.GetTopology();
    if (!meshTopologySchema.IsDefined()) {
        return false;
    }
    HdIntArrayDataSourceHandle const faceVertexCountsDataSource =
        meshTopologySchema.GetFaceVertexCounts();
    HdIntArrayDataSourceHandle const faceVertexIndicesDataSource =
        meshTopologySchema.GetFaceVertexIndices();
    if (!faceVertexCountsDataSource || !faceVertexIndicesDataSource) {
        return false;
    }

    TfToken scheme = PxOsdOpenSubdivTokens->none;
    if (HdTokenDataSourceHandle schemeDs = meshSchema.GetSubdivisionScheme()) {
        scheme = schemeDs->GetTypedValue(0.0f);
    }

    VtIntArray holeIndices;
    if (HdIntArrayDataSourceHandle holeDs =
            meshTopologySchema.GetHoleIndices()) {
        holeIndices = holeDs->GetTypedValue(0.0f);
    }

    TfToken orientation = PxOsdOpenSubdivTokens->rightHanded;
    if (HdTokenDataSourceHandle orientDs =
            meshTopologySchema.GetOrientation()) {
        orientation = orientDs->GetTypedValue(0.0f);
    }

    *out = HdMeshTopology(
        scheme,
        orientation,
        faceVertexCountsDataSource->GetTypedValue(0.0f),
        faceVertexIndicesDataSource->GetTypedValue(0.0f),
        holeIndices);

    // Mirror the adapter's _geomSubsetParents hint: the SimSceneIndex
    // observer records parents that gained a geomSubset child, so the child
    // scan only runs for prims that can have subsets. Without a live
    // observer the hint is unknown and we scan unconditionally (correct,
    // just slower — the adapter sees every prim through its own observer,
    // making the hint merely a cheaper route to the same answer).
    if (ShouldScanForGeomSubsets(id)) {
        TfToken const& purpose = sceneDelegate->GetRenderIndex()
                                     .GetRenderDelegate()
                                     ->GetMaterialBindingPurpose();
        _GatherGeomSubsets(id, terminal, purpose, out);
    }
    return true;
}

bool ReadMaterialResource(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    HdMaterialNetworkMap* out)
{
    HdSceneIndexBaseRefPtr terminal = GetTerminalSceneIndex(sceneDelegate);
    if (!terminal) {
        return false;
    }
    HdSceneIndexPrim prim = terminal->GetPrim(id);
    if (!prim.dataSource) {
        return false;
    }
    HdMaterialSchema matSchema =
        HdMaterialSchema::GetFromParent(prim.dataSource);
    if (!matSchema.IsDefined()) {
        return false;
    }
    TfTokenVector const renderContexts = sceneDelegate->GetRenderIndex()
                                             .GetRenderDelegate()
                                             ->GetMaterialRenderContexts();
    HdMaterialNetworkSchema netSchema =
        matSchema.GetMaterialNetwork(renderContexts);
    if (!netSchema.IsDefined()) {
        return false;
    }
    *out = _ToMaterialNetworkMap(netSchema, renderContexts);
    return true;
}

// --- GeomSubset scan-hint registry ---------------------------------------
// Fed by Hd_RUZINO_SimSceneIndex (see simSceneIndex.cpp). Entries are only a
// performance hint: a stale entry causes an unnecessary child scan (which
// then simply finds no subsets), never a missed one.

namespace {

    std::mutex& _GeomSubsetHintMutex()
    {
        static std::mutex m;
        return m;
    }

    std::unordered_set<SdfPath, SdfPath::Hash>& _GeomSubsetParents()
    {
        static std::unordered_set<SdfPath, SdfPath::Hash> parents;
        return parents;
    }

    std::atomic<bool>& _GeomSubsetObserverActive()
    {
        static std::atomic<bool> active(false);
        return active;
    }

}  // namespace

void SetGeomSubsetObserverActive(bool active)
{
    _GeomSubsetObserverActive().store(active);
}

void NoteGeomSubsetParent(SdfPath const& parentPath)
{
    std::lock_guard<std::mutex> lock(_GeomSubsetHintMutex());
    _GeomSubsetParents().insert(parentPath);
}

void ForgetPrimSubtree(SdfPath const& removedPath)
{
    std::lock_guard<std::mutex> lock(_GeomSubsetHintMutex());
    auto& parents = _GeomSubsetParents();
    for (auto it = parents.begin(); it != parents.end();) {
        it = it->HasPrefix(removedPath) ? parents.erase(it) : std::next(it);
    }
}

bool ShouldScanForGeomSubsets(SdfPath const& parentPath)
{
    // No live observer means the hint is unknown: scan (the safe answer).
    if (!_GeomSubsetObserverActive().load()) {
        return true;
    }
    std::lock_guard<std::mutex> lock(_GeomSubsetHintMutex());
    return _GeomSubsetParents().count(parentPath) > 0;
}

bool ReadPrimType(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    TfToken* outType)
{
    HdSceneIndexBaseRefPtr terminal = GetTerminalSceneIndex(sceneDelegate);
    if (!terminal) {
        return false;
    }
    HdSceneIndexPrim prim = terminal->GetPrim(id);
    *outType = prim.primType;
    return true;
}

bool IsGprimType(TfToken const& type)
{
    return type == HdPrimTypeTokens->sphere || type == HdPrimTypeTokens->cube ||
           type == HdPrimTypeTokens->cylinder ||
           type == HdPrimTypeTokens->cone || type == HdPrimTypeTokens->capsule;
}

bool IsPurposeShown(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    bool* outShown)
{
    *outShown = true;
    HdSceneIndexBaseRefPtr terminal = GetTerminalSceneIndex(sceneDelegate);
    if (!terminal) {
        return false;
    }
    HdSceneIndexPrim prim = terminal->GetPrim(id);
    if (!prim.dataSource) {
        return false;
    }
    HdPurposeSchema const purposeSchema =
        HdPurposeSchema::GetFromParent(prim.dataSource);
    if (!purposeSchema.IsDefined()) {
        return true;
    }
    HdTokenDataSourceHandle const purposeDs = purposeSchema.GetPurpose();
    if (!purposeDs) {
        return true;
    }
    TfToken const purpose = purposeDs->GetTypedValue(0.0f);
    // Final-render semantics: default and render purpose are shown,
    // proxy and guide stand-ins are hidden.
    *outShown = purpose != HdRenderTagTokens->proxy &&
                purpose != HdRenderTagTokens->guide;
    return true;
}

namespace {

    constexpr int kGprimSlices = 32;     // longitude resolution
    constexpr int kGprimStacks = 24;     // latitude rings (sphere)
    constexpr int kGprimHemiStacks = 8;  // hemisphere rings (capsule)
    constexpr double kPi = 3.14159265358979323846;

    float _RadiusParam(HdSceneIndexPrim const& prim, TfToken const& type)
    {
        HdDoubleDataSourceHandle ds;
        if (type == HdPrimTypeTokens->sphere) {
            ds = HdSphereSchema::GetFromParent(prim.dataSource).GetRadius();
        }
        else if (type == HdPrimTypeTokens->cylinder) {
            ds = HdCylinderSchema::GetFromParent(prim.dataSource).GetRadius();
        }
        else if (type == HdPrimTypeTokens->cone) {
            ds = HdConeSchema::GetFromParent(prim.dataSource).GetRadius();
        }
        else if (type == HdPrimTypeTokens->capsule) {
            ds = HdCapsuleSchema::GetFromParent(prim.dataSource).GetRadius();
        }
        return ds ? float(ds->GetTypedValue(0.0f)) : 0.0f;
    }

    float _HeightParam(HdSceneIndexPrim const& prim, TfToken const& type)
    {
        HdDoubleDataSourceHandle ds;
        if (type == HdPrimTypeTokens->cylinder) {
            ds = HdCylinderSchema::GetFromParent(prim.dataSource).GetHeight();
        }
        else if (type == HdPrimTypeTokens->cone) {
            ds = HdConeSchema::GetFromParent(prim.dataSource).GetHeight();
        }
        else if (type == HdPrimTypeTokens->capsule) {
            ds = HdCapsuleSchema::GetFromParent(prim.dataSource).GetHeight();
        }
        return ds ? float(ds->GetTypedValue(0.0f)) : 0.0f;
    }

    // Cylinder/cone/capsule axis token ("X"/"Y"/"Z"). Absent when not
    // authored — the UsdGeom schema fallback for these prims is "Z", so the
    // caller must treat an empty token as Z, not Y.
    TfToken _AxisParam(HdSceneIndexPrim const& prim, TfToken const& type)
    {
        HdTokenDataSourceHandle ds;
        if (type == HdPrimTypeTokens->cylinder) {
            ds = HdCylinderSchema::GetFromParent(prim.dataSource).GetAxis();
        }
        else if (type == HdPrimTypeTokens->cone) {
            ds = HdConeSchema::GetFromParent(prim.dataSource).GetAxis();
        }
        else if (type == HdPrimTypeTokens->capsule) {
            ds = HdCapsuleSchema::GetFromParent(prim.dataSource).GetAxis();
        }
        return ds ? ds->GetTypedValue(0.0f) : TfToken();
    }

    // UV-sphere rings (north pole -> south pole) with outward winding.
    void _AppendSphereBands(
        double radius,
        int stacks,
        float centerY,
        VtVec3fArray* points,
        VtIntArray* counts,
        VtIntArray* indices)
    {
        int const north = int(points->size());
        points->push_back(GfVec3f(0.0f, centerY + float(radius), 0.0f));
        auto ringStart = [&](int ring) {
            return north + 1 + (ring - 1) * kGprimSlices;
        };
        for (int i = 1; i < stacks; ++i) {
            double const theta = kPi * i / stacks;
            float const y = centerY + float(radius * cos(theta));
            float const r = float(radius * sin(theta));
            for (int j = 0; j < kGprimSlices; ++j) {
                double const phi = 2.0 * kPi * j / kGprimSlices;
                points->push_back(
                    GfVec3f(float(r * cos(phi)), y, float(r * sin(phi))));
            }
        }
        int const south = north + 1 + (stacks - 1) * kGprimSlices;
        points->push_back(GfVec3f(0.0f, centerY - float(radius), 0.0f));

        for (int j = 0; j < kGprimSlices; ++j) {
            counts->push_back(3);
            indices->push_back(north);
            indices->push_back(ringStart(1) + (j + 1) % kGprimSlices);
            indices->push_back(ringStart(1) + j);
        }
        for (int i = 1; i < stacks - 1; ++i) {
            for (int j = 0; j < kGprimSlices; ++j) {
                int const a = ringStart(i) + j;
                int const b = ringStart(i) + (j + 1) % kGprimSlices;
                int const c = ringStart(i + 1) + (j + 1) % kGprimSlices;
                int const d = ringStart(i + 1) + j;
                counts->push_back(4);
                indices->insert(indices->end(), { a, b, c, d });
            }
        }
        for (int j = 0; j < kGprimSlices; ++j) {
            counts->push_back(3);
            indices->push_back(south);
            indices->push_back(ringStart(stacks - 1) + j);
            indices->push_back(ringStart(stacks - 1) + (j + 1) % kGprimSlices);
        }
    }

    // Hemisphere continuation of an EXISTING equator ring (kGprimSlices
    // verts at centerY, radius): stacks segments from the equator to the
    // pole, outward winding matching the sphere bands. Sharing the rim ring
    // (instead of duplicating it) keeps smooth normals seamless at the
    // tangent joint — the capsule's cylinder wall and hemisphere cap meet
    // with identical horizontal normals there.
    void _AppendHemisphereBands(
        double radius,
        int stacks,
        float centerY,
        bool up,
        int ringStart,
        VtVec3fArray* points,
        VtIntArray* counts,
        VtIntArray* indices)
    {
        float const sign = up ? 1.0f : -1.0f;
        int const newBase = int(points->size());
        for (int k = 1; k < stacks; ++k) {
            double const theta = (kPi / 2.0) * k / stacks;
            float const y = centerY + sign * float(radius * sin(theta));
            float const r = float(radius * cos(theta));
            for (int j = 0; j < kGprimSlices; ++j) {
                double const phi = 2.0 * kPi * j / kGprimSlices;
                points->push_back(
                    GfVec3f(float(r * cos(phi)), y, float(r * sin(phi))));
            }
        }
        int const pole = int(points->size());
        points->push_back(GfVec3f(0.0f, centerY + sign * float(radius), 0.0f));

        auto ring = [&](int k) {
            return k == 0 ? ringStart : newBase + (k - 1) * kGprimSlices;
        };
        int const last = stacks - 1;
        for (int k = 1; k < stacks; ++k) {
            for (int j = 0; j < kGprimSlices; ++j) {
                // Wind the pole-ward ring first (same order as the sphere
                // body bands) so edge1 x edge2 points outward.
                int const u0 = ring(k) + j;
                int const u1 = ring(k) + (j + 1) % kGprimSlices;
                int const l0 = ring(k - 1) + j;
                int const l1 = ring(k - 1) + (j + 1) % kGprimSlices;
                counts->push_back(4);
                if (up) {
                    indices->insert(indices->end(), { u0, u1, l1, l0 });
                }
                else {
                    indices->insert(indices->end(), { l0, l1, u1, u0 });
                }
            }
        }
        for (int j = 0; j < kGprimSlices; ++j) {
            counts->push_back(3);
            if (up) {
                indices->insert(
                    indices->end(),
                    { pole,
                      ring(last) + (j + 1) % kGprimSlices,
                      ring(last) + j });
            }
            else {
                indices->insert(
                    indices->end(),
                    { pole,
                      ring(last) + j,
                      ring(last) + (j + 1) % kGprimSlices });
            }
        }
    }

    // One quad ring band between an existing lower ring and upper ring,
    // outward winding (edge1 ~ +Y, edge2 ~ east).
    void _AppendCylinderSide(
        int lowerStart,
        int upperStart,
        VtIntArray* counts,
        VtIntArray* indices)
    {
        for (int j = 0; j < kGprimSlices; ++j) {
            int const b0 = lowerStart + j;
            int const b1 = lowerStart + (j + 1) % kGprimSlices;
            int const t0 = upperStart + j;
            int const t1 = upperStart + (j + 1) % kGprimSlices;
            counts->push_back(4);
            indices->insert(indices->end(), { b0, t0, t1, b1 });
        }
    }

    // Flat cap fan around an existing ring; +Up caps wind opposite to -Up.
    void _AppendCap(
        int centerIndex,
        int ringStart,
        bool up,
        VtIntArray* counts,
        VtIntArray* indices)
    {
        for (int j = 0; j < kGprimSlices; ++j) {
            int const a = ringStart + j;
            int const b = ringStart + (j + 1) % kGprimSlices;
            counts->push_back(3);
            if (up) {
                indices->insert(indices->end(), { centerIndex, b, a });
            }
            else {
                indices->insert(indices->end(), { centerIndex, a, b });
            }
        }
    }

    void _FinishMeshTopology(
        VtIntArray const& counts,
        VtIntArray const& indices,
        HdMeshTopology* out)
    {
        *out = HdMeshTopology(
            PxOsdOpenSubdivTokens->none,
            PxOsdOpenSubdivTokens->rightHanded,
            counts,
            indices);
    }

}  // namespace

bool ReadGprimMesh(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    HdMeshTopology* outTopology,
    VtVec3fArray* outPoints)
{
    HdSceneIndexBaseRefPtr terminal = GetTerminalSceneIndex(sceneDelegate);
    if (!terminal) {
        return false;
    }
    HdSceneIndexPrim prim = terminal->GetPrim(id);
    TfToken const& type = prim.primType;
    if (!IsGprimType(type) || !prim.dataSource) {
        return false;
    }

    VtIntArray counts;
    VtIntArray indices;

    if (type == HdPrimTypeTokens->sphere) {
        float const radius = _RadiusParam(prim, type);
        if (radius <= 0.0f) {
            return false;
        }
        _AppendSphereBands(
            radius, kGprimStacks, 0.0f, outPoints, &counts, &indices);
    }
    else if (type == HdPrimTypeTokens->cube) {
        HdDoubleDataSourceHandle const sizeDs =
            HdCubeSchema::GetFromParent(prim.dataSource).GetSize();
        if (!sizeDs) {
            return false;
        }
        float const h = float(sizeDs->GetTypedValue(0.0f)) * 0.5f;
        // 24 unique verts (4 per face) so the smooth-normal fallback yields
        // flat per-face normals.
        GfVec3f const face_verts[6][4] = {
            // +Z, -Z, +X, -X, +Y, -Y — each wound outward
            { GfVec3f(-h, -h, h),
              GfVec3f(h, -h, h),
              GfVec3f(h, h, h),
              GfVec3f(-h, h, h) },
            { GfVec3f(h, -h, -h),
              GfVec3f(-h, -h, -h),
              GfVec3f(-h, h, -h),
              GfVec3f(h, h, -h) },
            { GfVec3f(h, -h, h),
              GfVec3f(h, -h, -h),
              GfVec3f(h, h, -h),
              GfVec3f(h, h, h) },
            { GfVec3f(-h, -h, -h),
              GfVec3f(-h, -h, h),
              GfVec3f(-h, h, h),
              GfVec3f(-h, h, -h) },
            { GfVec3f(-h, h, h),
              GfVec3f(h, h, h),
              GfVec3f(h, h, -h),
              GfVec3f(-h, h, -h) },
            { GfVec3f(-h, -h, -h),
              GfVec3f(h, -h, -h),
              GfVec3f(h, -h, h),
              GfVec3f(-h, -h, h) },
        };
        for (auto const& face : face_verts) {
            int const base = int(outPoints->size());
            for (auto const& v : face) {
                outPoints->push_back(v);
            }
            counts.push_back(4);
            indices.insert(
                indices.end(), { base, base + 1, base + 2, base + 3 });
        }
    }
    else if (
        type == HdPrimTypeTokens->cylinder || type == HdPrimTypeTokens->cone ||
        type == HdPrimTypeTokens->capsule) {
        float const radius = _RadiusParam(prim, type);
        float const height = _HeightParam(prim, type);
        if (radius <= 0.0f || height <= 0.0f) {
            return false;
        }
        float const half = height * 0.5f;
        // Everything below is generated along +Y; the axis rotation at the
        // end of this branch maps it onto the authored axis.
        size_t const firstNewPoint = outPoints->size();
        if (type == HdPrimTypeTokens->capsule) {
            // Cylinder band from -half..+half with hemispherical caps of the
            // same radius centered at +-half. The caps reuse the band's rim
            // rings — no interior flat caps, no buried sphere halves.
            auto appendRing = [&](float y) {
                int const start = int(outPoints->size());
                for (int j = 0; j < kGprimSlices; ++j) {
                    double const phi = 2.0 * kPi * j / kGprimSlices;
                    outPoints->push_back(GfVec3f(
                        float(radius * cos(phi)), y, float(radius * sin(phi))));
                }
                return start;
            };
            int const bottom = appendRing(-half);
            int const top = appendRing(half);
            _AppendCylinderSide(bottom, top, &counts, &indices);
            _AppendHemisphereBands(
                radius,
                kGprimHemiStacks,
                half,
                true,
                top,
                outPoints,
                &counts,
                &indices);
            _AppendHemisphereBands(
                radius,
                kGprimHemiStacks,
                -half,
                false,
                bottom,
                outPoints,
                &counts,
                &indices);
        }
        else {
            // Side quads + flat caps; cone tapers to an apex at +half.
            int const bottom = int(outPoints->size());
            for (int j = 0; j < kGprimSlices; ++j) {
                double const phi = 2.0 * kPi * j / kGprimSlices;
                outPoints->push_back(GfVec3f(
                    float(radius * cos(phi)), -half, float(radius * sin(phi))));
            }
            if (type == HdPrimTypeTokens->cylinder) {
                int const top = int(outPoints->size());
                for (int j = 0; j < kGprimSlices; ++j) {
                    double const phi = 2.0 * kPi * j / kGprimSlices;
                    outPoints->push_back(GfVec3f(
                        float(radius * cos(phi)),
                        half,
                        float(radius * sin(phi))));
                }
                _AppendCylinderSide(bottom, top, &counts, &indices);
                _AppendCap(
                    int(outPoints->size()), bottom, false, &counts, &indices);
                outPoints->push_back(GfVec3f(0.0f, -half, 0.0f));
                _AppendCap(
                    int(outPoints->size()), top, true, &counts, &indices);
                outPoints->push_back(GfVec3f(0.0f, half, 0.0f));
            }
            else {  // cone
                int const apex = bottom + kGprimSlices;
                outPoints->push_back(GfVec3f(0.0f, half, 0.0f));
                for (int j = 0; j < kGprimSlices; ++j) {
                    counts.push_back(3);
                    indices.push_back(apex);
                    indices.push_back(bottom + (j + 1) % kGprimSlices);
                    indices.push_back(bottom + j);
                }
                _AppendCap(
                    int(outPoints->size()), bottom, false, &counts, &indices);
                outPoints->push_back(GfVec3f(0.0f, -half, 0.0f));
            }
        }

        // Map the +Y-generated geometry onto the authored axis. The UsdGeom
        // fallback for cylinder/cone/capsule is "Z" (_AxisParam returns an
        // empty token when unauthored). Rigid rotations keep the outward
        // winding intact.
        TfToken const axis = _AxisParam(prim, type);
        if (axis == TfToken("X")) {
            // +90 deg about Z: +Y -> +X.
            for (size_t i = firstNewPoint; i < outPoints->size(); ++i) {
                GfVec3f const p = (*outPoints)[i];
                (*outPoints)[i] = GfVec3f(p[1], -p[0], p[2]);
            }
        }
        else if (axis != TfToken("Y")) {
            // "Z" (or the unauthored fallback): +90 deg about X: +Y -> +Z.
            for (size_t i = firstNewPoint; i < outPoints->size(); ++i) {
                GfVec3f const p = (*outPoints)[i];
                (*outPoints)[i] = GfVec3f(p[0], -p[2], p[1]);
            }
        }
    }
    else {
        return false;
    }

    _FinishMeshTopology(counts, indices, outTopology);
    return true;
}

}  // namespace Ruzino_Hydra2

PXR_NAMESPACE_CLOSE_SCOPE
