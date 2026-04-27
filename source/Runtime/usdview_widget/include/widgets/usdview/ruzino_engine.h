#pragma once

#include <pxr/base/tf/weakPtr.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/usd/usd/notice.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>

#include "widgets/api.h"

RUZINO_NAMESPACE_OPEN_SCOPE

/// Extends UsdImagingGLEngine to handle config:* attribute dirty notifications.
///
/// In USD 26.x, config:* attributes on material prims are forwarded to
/// HdMaterialNetworkMap::config (data works), but they do NOT trigger
/// Invalidate() / Sync() because IsInterfaceInputName() only matches
/// inputs:*. This class listens for UsdNotice::ObjectsChanged and manually
/// marks material Sprims dirty when config:* properties change.
class USDVIEW_WIDGET_API RuzinoEngine : public pxr::UsdImagingGLEngine,
                                       public pxr::TfWeakBase {
   public:
    RuzinoEngine(const pxr::UsdImagingGLEngine::Parameters& params,
                 pxr::UsdStageRefPtr stage);

    ~RuzinoEngine();

   private:
    void _OnObjectsChanged(
        const pxr::UsdNotice::ObjectsChanged& notice,
        const pxr::UsdStageWeakPtr& sender);

    pxr::TfNotice::Key _noticeKey;
    pxr::UsdStageRefPtr _stage;
};

RUZINO_NAMESPACE_CLOSE_SCOPE
