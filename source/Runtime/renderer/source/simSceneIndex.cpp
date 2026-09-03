//
// Copyright 2026 Ruzino
//
// Licensed under the terms set forth in the LICENSE.txt file.
//
#include "simSceneIndex.h"

#include <spdlog/spdlog.h>

#include <atomic>

#include "hydra2Ingest.h"
#include "pxr/base/tf/envSetting.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/imaging/hd/tokens.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_ENV_SETTING(
    HD_RUZINO_SIM_SCENE_INDEX_DEBUG,
    false,
    "Log the prim notices flowing through the RUZINO renderer-specific "
    "scene index (Hydra 2.0 chain liveness probe).");

namespace {

bool _IsDebugEnabled()
{
    static const bool enabled =
        TfGetEnvSetting(HD_RUZINO_SIM_SCENE_INDEX_DEBUG);
    return enabled;
}

// Several engines (viewport + offline renderer) can each hold a chain link
// alive; the geomSubset hint registry is only authoritative while at least
// one observer is watching the chain.
std::atomic<int>& _LiveInstanceCount()
{
    static std::atomic<int> count{ 0 };
    return count;
}

}  // namespace

Hd_RUZINO_SimSceneIndex::Hd_RUZINO_SimSceneIndex(
    const HdSceneIndexBaseRefPtr& inputSceneIndex)
    : HdSingleInputFilteringSceneIndexBase(inputSceneIndex)
{
    if (_LiveInstanceCount().fetch_add(1) == 0) {
        Ruzino_Hydra2::SetGeomSubsetObserverActive(true);
    }
    if (_IsDebugEnabled()) {
        spdlog::info("[SimSceneIndex] created (pass-through observer)");
    }
}

Hd_RUZINO_SimSceneIndex::~Hd_RUZINO_SimSceneIndex()
{
    if (_LiveInstanceCount().fetch_sub(1) == 1) {
        Ruzino_Hydra2::SetGeomSubsetObserverActive(false);
    }
}

Hd_RUZINO_SimSceneIndexRefPtr Hd_RUZINO_SimSceneIndex::New(
    const HdSceneIndexBaseRefPtr& inputSceneIndex)
{
    return TfCreateRefPtr(new Hd_RUZINO_SimSceneIndex(inputSceneIndex));
}

HdSceneIndexPrim Hd_RUZINO_SimSceneIndex::GetPrim(const SdfPath& primPath) const
{
    return _GetInputSceneIndex()->GetPrim(primPath);
}

SdfPathVector Hd_RUZINO_SimSceneIndex::GetChildPrimPaths(
    const SdfPath& primPath) const
{
    return _GetInputSceneIndex()->GetChildPrimPaths(primPath);
}

void Hd_RUZINO_SimSceneIndex::_PrimsAdded(
    const HdSceneIndexBase& sender,
    const HdSceneIndexObserver::AddedPrimEntries& entries)
{
    if (_IsDebugEnabled()) {
        for (const auto& entry : entries) {
            spdlog::info(
                "[SimSceneIndex] + {} ({})",
                entry.primPath.GetString(),
                entry.primType.GetString());
        }
    }
    // Feed the geomSubset scan hint (mirrors the emulation adapter's
    // _geomSubsetParents, which the adapter populates the same way).
    for (const auto& entry : entries) {
        if (entry.primType == HdPrimTypeTokens->geomSubset) {
            Ruzino_Hydra2::NoteGeomSubsetParent(entry.primPath.GetParentPath());
        }
    }
    _SendPrimsAdded(entries);
}

void Hd_RUZINO_SimSceneIndex::_PrimsRemoved(
    const HdSceneIndexBase& sender,
    const HdSceneIndexObserver::RemovedPrimEntries& entries)
{
    if (_IsDebugEnabled()) {
        for (const auto& entry : entries) {
            spdlog::info("[SimSceneIndex] - {}", entry.primPath.GetString());
        }
    }
    // Removing a subtree removes any recorded geomSubset parents under it.
    // (Purely a staleness bound: a missed prune only causes an extra scan.)
    for (const auto& entry : entries) {
        Ruzino_Hydra2::ForgetPrimSubtree(entry.primPath);
    }
    _SendPrimsRemoved(entries);
}

void Hd_RUZINO_SimSceneIndex::_PrimsDirtied(
    const HdSceneIndexBase& sender,
    const HdSceneIndexObserver::DirtiedPrimEntries& entries)
{
    if (_IsDebugEnabled()) {
        for (const auto& entry : entries) {
            std::string locators;
            for (const auto& locator : entry.dirtyLocators) {
                if (!locators.empty()) {
                    locators += ",";
                }
                locators += locator.GetString();
            }
            spdlog::info(
                "[SimSceneIndex] ~ {} {}",
                entry.primPath.GetString(),
                locators);
        }
    }
    _SendPrimsDirtied(entries);
}

PXR_NAMESPACE_CLOSE_SCOPE
