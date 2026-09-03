//
// Copyright 2026 Ruzino
//
// Licensed under the terms set forth in the LICENSE.txt file.
//
#ifndef Hd_RUZINO_SIM_SCENE_INDEX_H
#define Hd_RUZINO_SIM_SCENE_INDEX_H

#include "pxr/base/tf/declarePtrs.h"
#include "pxr/imaging/hd/filteringSceneIndex.h"
#include "pxr/imaging/hd/sceneIndexObserver.h"
#include "pxr/pxr.h"
#include "pxr/usd/sdf/path.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_DECLARE_REF_PTRS(Hd_RUZINO_SimSceneIndex);

/// \class Hd_RUZINO_SimSceneIndex
///
/// Hydra 2.0 scene index appended for the "RUZINO Renderer" by
/// Hd_RUZINO_SceneIndexPlugin. It sits after the UsdImaging chain
/// (flattened/resolved data) and just before the terminal scene index
/// consumed by the render delegate.
///
/// Milestone 1 (this class): pass-through observer. Every prim notice from
/// upstream is forwarded unchanged; with HD_RUZINO_SIM_SCENE_INDEX_DEBUG=1
/// the flow through the renderer-specific chain link is logged, proving the
/// chain is live for both the interactive viewport and offline renders.
/// While observing, it also feeds hydra2Ingest's geomSubset scan-hint
/// registry (see Ruzino_Hydra2::NoteGeomSubsetParent) so topology reads can
/// skip scanning prims that never gained a geomSubset child.
///
/// Milestone 2 (future): injection point for simulation data produced by the
/// node-graph runtime (e.g. points prims backed by the shared GPU buffer
/// registry, zero-copy), fed into Hydra through AddPrims/Dirtied on a
/// retained scene index merged in here.
///
class Hd_RUZINO_SimSceneIndex : public HdSingleInputFilteringSceneIndexBase {
   public:
    /// Creates a new pass-through scene index observing \p inputSceneIndex.
    static Hd_RUZINO_SimSceneIndexRefPtr New(
        const HdSceneIndexBaseRefPtr& inputSceneIndex);

    HdSceneIndexPrim GetPrim(const SdfPath& primPath) const override;

    SdfPathVector GetChildPrimPaths(const SdfPath& primPath) const override;

    ~Hd_RUZINO_SimSceneIndex() override;

   protected:
    Hd_RUZINO_SimSceneIndex(const HdSceneIndexBaseRefPtr& inputSceneIndex);

    void _PrimsAdded(
        const HdSceneIndexBase& sender,
        const HdSceneIndexObserver::AddedPrimEntries& entries) override;

    void _PrimsRemoved(
        const HdSceneIndexBase& sender,
        const HdSceneIndexObserver::RemovedPrimEntries& entries) override;

    void _PrimsDirtied(
        const HdSceneIndexBase& sender,
        const HdSceneIndexObserver::DirtiedPrimEntries& entries) override;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif  // Hd_RUZINO_SIM_SCENE_INDEX_H
