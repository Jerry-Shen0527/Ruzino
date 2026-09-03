//
// Copyright 2026 Ruzino
//
// Licensed under the terms set forth in the LICENSE.txt file.
//
#ifndef Hd_RUZINO_SCENE_INDEX_PLUGIN_H
#define Hd_RUZINO_SCENE_INDEX_PLUGIN_H

#include "pxr/pxr.h"

#include "pxr/imaging/hd/sceneIndexPlugin.h"

PXR_NAMESPACE_OPEN_SCOPE

/// \class Hd_RUZINO_SceneIndexPlugin
///
/// Hydra 2.0 scene index plugin for the RUZINO renderer. Registered in the
/// plugin's plugInfo.json and self-registered (via
/// RegisterSceneIndexForRenderer) for the display name "RUZINO Renderer",
/// so that UsdImagingGLEngine appends it to the renderer-specific part of
/// the scene index chain (AppendSceneIndicesForRenderer) whenever our
/// renderer is active -- for both the interactive viewport and the offline
/// HydraRenderer.
///
/// Currently appends Hd_RUZINO_SimSceneIndex (pass-through observer, future
/// simulation-data injection point).
///
class Hd_RUZINO_SceneIndexPlugin : public HdSceneIndexPlugin
{
protected:
    /// Appends Hd_RUZINO_SimSceneIndex on top of \p inputScene.
    HdSceneIndexBaseRefPtr _AppendSceneIndex(
        const HdSceneIndexBaseRefPtr &inputScene,
        const HdContainerDataSourceHandle &inputArgs) override;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif  // Hd_RUZINO_SCENE_INDEX_PLUGIN_H
