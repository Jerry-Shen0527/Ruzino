//
// Copyright 2026 Ruzino
//
// Licensed under the terms set forth in the LICENSE.txt file.
//
// Hydra 2.0 migration (Track A): readers that pull prim data straight from
// the terminal scene index's data source tree, mirroring the exact
// translations HdSceneIndexAdapterSceneDelegate performs for the legacy
// HdSceneDelegate::Get* surface. Every reader returns false when the
// data-source path cannot serve the request (no terminal scene index,
// missing prim/data source, or a case the adapter would route through its
// own remaining Get() branches), so callers fall back to the legacy
// delegate pull and behavior stays identical in any context where the
// data-source path is unavailable. A true result means the value was served
// by the data-source path — it may legitimately be empty when the
// underlying parameter is not authored (same as the adapter's answer, so no
// fallback fires and absent optional parameters cost a single walk).
//
// (An env gate that forced the legacy path for A/B validation,
// RZ_HYDRA2_PREFER_LEGACY=1, was removed after the direct-read migration
// passed pixel-parity validation on 2026-09-02.)
//
// Declarations only; the implementation (and its schema-header include
// hazards: generated headers requiring staticTokens first, and the Windows
// COM "#define interface struct" neutralization) lives in hydra2Ingest.cpp
// so that consumer TUs don't inherit any of it.
//
#ifndef Hd_RUZINO_HYDRA2_INGEST_H
#define Hd_RUZINO_HYDRA2_INGEST_H

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/value.h"
#include "pxr/imaging/hd/enums.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/hd/meshTopology.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/usd/sdf/path.h"

PXR_NAMESPACE_OPEN_SCOPE

namespace Ruzino_Hydra2 {

// Mirrors HdSceneIndexAdapterSceneDelegate::Get(id, key) for the "primvars"
// container, value at time 0. Returns true when the data-source path can
// serve the prim: *out then holds the primvar value, which may legitimately
// be empty when the primvar is not authored (identical to what the adapter
// would return, so no fallback is needed). Returns false when the caller
// should fall back to the legacy delegate pull: no terminal scene index, the
// prim has no data source, or the prim is a legacy-emulation prim whose
// Get() the adapter serves from an underlying delegate (mirroring the
// adapter's trailing sceneDelegate fallback).
bool ReadPrimvar(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    TfToken const& name,
    VtValue* out);

// Mirrors HdSceneIndexAdapterSceneDelegate::GetLightParamValue: the "light"
// container, sampled value at time 0. Same true/false contract as
// ReadPrimvar; the adapter has no other fallback for light params, so a
// served-but-unauthored parameter yields true with an empty value.
bool ReadLightParam(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    TfToken const& paramName,
    VtValue* out);

// Mirrors the volume-field branch of HdSceneIndexAdapterSceneDelegate::Get:
// the "volumeField" container, sampled value at time 0. The branch is gated
// on the prim type exactly like the adapter (HdLegacyPrimTypeIsVolumeField)
// — a non-field prim falls through to the adapter's remaining Get()
// branches, so this returns false and the caller falls back.
bool ReadVolumeFieldParam(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    TfToken const& paramName,
    VtValue* out);

// Mirrors HdSceneIndexAdapterSceneDelegate::GetTransform: HdXformSchema
// matrix at time 0, identity when absent. Returns false when the data-source
// path could not serve a transform (caller falls back to the delegate).
bool ReadTransform(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    GfMatrix4d* outTransform);

// Mirrors HdSceneIndexAdapterSceneDelegate::GetMaterialId: the material
// binding for the render delegate's binding purpose. Returns false when the
// data-source path could not serve a binding.
bool ReadMaterialId(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    SdfPath* outMaterialId);

// Mirrors HdSceneIndexAdapterSceneDelegate::GetPrimvarDescriptors: the full
// descriptor list built from the primvars container (all interpolation
// buckets, ordered by GetPrimvarNames). Primvars with an invalid/unmapped
// interpolation token are dropped, mirroring the adapter's skip. Returns
// false when the data-source path could not serve a list (caller falls
// back).
bool ReadPrimvarDescriptors(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    HdPrimvarDescriptorVector* out);

// Convenience: descriptors of one interpolation bucket, mirroring
// GetPrimvarDescriptors(id, interpolation).
bool ReadPrimvarDescriptors(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    HdInterpolation interpolation,
    HdPrimvarDescriptorVector* out);

// Mirrors HdSceneIndexAdapterSceneDelegate::GetVisible: legacy instancers
// (isLegacyInstancer flag) are unconditionally visible; otherwise the
// HdVisibilitySchema bool, defaulting to visible when the schema (or its
// value) is absent. Returns false only when the data-source path cannot
// serve the prim at all.
bool ReadVisible(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    bool* outVisible);

// The prim's flattened prim type from the terminal scene index. Returns
// false when there is no terminal scene index (type stays empty).
bool ReadPrimType(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    TfToken* outType);

// True for the analytic gprim types the ingest can tessellate into a mesh:
// sphere / cube / cylinder / cone / capsule.
bool IsGprimType(TfToken const& type);

// Final-render purpose gate: *outShown is false when the prim's flattened
// purpose is proxy or guide, true for default/render or when no purpose is
// authored. Returns false when the data-source path cannot serve a purpose
// (caller keeps its current visibility).
bool IsPurposeShown(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    bool* outShown);

// Tessellates analytic gprims (UsdGeomSphere/Cube/Cylinder/Cone/Capsule read
// through their Hd schemas) into triangle-mesh topology + points so the mesh
// rprim path can render them. Cylinder/cone/capsule honor the authored axis
// token (UsdGeom fallback "Z"; geometry is built along +Y and rotated).
// Normals are left to the mesh's smooth-normal fallback. Returns
// false when the prim is not a gprim or lacks its size parameter.
bool ReadGprimMesh(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    HdMeshTopology* outTopology,
    VtVec3fArray* outPoints);

// Mirrors HdSceneIndexAdapterSceneDelegate::GetInstanceIndices.
bool ReadInstanceIndices(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& instancerId,
    SdfPath const& prototypeId,
    VtIntArray* out);

// Mirrors HdSceneIndexAdapterSceneDelegate::GetMeshTopology, including the
// _GatherGeomSubsets pass over geomSubset children (invisible subsets union
// into invisible faces/points; visible ones with a material binding become
// face-set subsets, hard-coded TypeFaceSet exactly like the adapter).
// Returns false when the data-source path could not serve a topology (caller
// falls back to the legacy delegate pull).
bool ReadMeshTopology(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    HdMeshTopology* out);

// Mirrors HdSceneIndexAdapterSceneDelegate::GetMaterialResource, including
// the _ToMaterialNetworkMap + _Walk + _GetHdParamsFromDataSource +
// _ToDictionary translations. Returns false when the data-source path could
// not serve a network (caller falls back to the legacy delegate pull).
bool ReadMaterialResource(
    HdSceneDelegate* sceneDelegate,
    SdfPath const& id,
    HdMaterialNetworkMap* out);

// GeomSubset scan-hint registry, fed by the renderer-specific chain link
// (Hd_RUZINO_SimSceneIndex). Mirrors the emulation adapter's
// _geomSubsetParents set: topology reads only need to scan child prims for
// subsets when a live observer has actually seen a geomSubset child under
// the prim. With no live observer the answer is unknown and callers scan
// unconditionally (correct, just slower).
void SetGeomSubsetObserverActive(bool active);
void NoteGeomSubsetParent(SdfPath const& parentPath);
void ForgetPrimSubtree(SdfPath const& removedPath);
bool ShouldScanForGeomSubsets(SdfPath const& parentPath);

}  // namespace Ruzino_Hydra2

PXR_NAMESPACE_CLOSE_SCOPE

#endif  // Hd_RUZINO_HYDRA2_INGEST_H
