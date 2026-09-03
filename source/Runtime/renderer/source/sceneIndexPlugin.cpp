//
// Copyright 2026 Ruzino
//
// Licensed under the terms set forth in the LICENSE.txt file.
//
#include "sceneIndexPlugin.h"

#include "simSceneIndex.h"

#include "pxr/base/tf/registryManager.h"
#include "pxr/base/tf/staticTokens.h"
#include "pxr/base/tf/type.h"
#include "pxr/imaging/hd/sceneIndexPluginRegistry.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    ((sceneIndexPluginName, "Hd_RUZINO_SceneIndexPlugin"))
    ((rendererDisplayName, "RUZINO Renderer")));

TF_REGISTRY_FUNCTION(TfType)
{
    HdSceneIndexPluginRegistry::Define<Hd_RUZINO_SceneIndexPlugin>();
}

// Self-register for our renderer's display name (same pattern as upstream
// HdsiDebuggingSceneIndexPlugin, which lives in the already-loaded hdsi
// library). NOTE: since OpenUSD 26.08 the registry's default ordering
// policy is Hybrid, which composes the chain from plugInfo JSON
// declarations and merges C++ registrations in by plugin id — a pure C++
// registration without the matching "loadWithRenderer" entry in
// resources/plugInfo.json is silently dropped. Both registrations below
// and the plugInfo entry are required; keep them in sync.
TF_REGISTRY_FUNCTION(HdSceneIndexPlugin)
{
    HdSceneIndexPluginRegistry::GetInstance().RegisterSceneIndexForRenderer(
        /* rendererDisplayName = */ _tokens->rendererDisplayName.GetString(),
        _tokens->sceneIndexPluginName,
        /* inputArgs = */ nullptr,
        /* insertionPhase = */ 0,
        HdSceneIndexPluginRegistry::InsertionOrderAtEnd);
}

HdSceneIndexBaseRefPtr
Hd_RUZINO_SceneIndexPlugin::_AppendSceneIndex(
    const HdSceneIndexBaseRefPtr &inputScene,
    const HdContainerDataSourceHandle &inputArgs)
{
    return Hd_RUZINO_SimSceneIndex::New(inputScene);
}

PXR_NAMESPACE_CLOSE_SCOPE
