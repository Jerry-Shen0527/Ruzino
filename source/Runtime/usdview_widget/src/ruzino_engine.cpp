#include "widgets/usdview/ruzino_engine.h"

#include <pxr/base/tf/notice.h>
#include <pxr/usd/usdShade/material.h>

#include <spdlog/spdlog.h>

RUZINO_NAMESPACE_OPEN_SCOPE

RuzinoEngine::RuzinoEngine(
    const pxr::UsdImagingGLEngine::Parameters& params,
    pxr::UsdStageRefPtr stage)
    : pxr::UsdImagingGLEngine(params), _stage(std::move(stage))
{
    if (_stage) {
        _noticeKey = pxr::TfNotice::Register(
            pxr::TfCreateWeakPtr(this),
            &RuzinoEngine::_OnObjectsChanged,
            pxr::UsdStageWeakPtr(_stage));
    }
}

RuzinoEngine::~RuzinoEngine()
{
    if (_noticeKey.IsValid()) {
        pxr::TfNotice::Revoke(_noticeKey);
    }
}

void RuzinoEngine::_OnObjectsChanged(
    const pxr::UsdNotice::ObjectsChanged& notice,
    const pxr::UsdStageWeakPtr& sender)
{
    if (!_stage) {
        return;
    }

    // Collect material prim paths that had config:* property changes.
    // In USD 26.x, config:* attributes are forwarded to
    // HdMaterialNetworkMap::config (data works) but do NOT trigger
    // Invalidate() (no dirty notification). We work around this by
    // toggling a UsdShadeInput on the material, which triggers the
    // built-in IsInterfaceInputName() dirty path.
    std::unordered_set<pxr::SdfPath, pxr::SdfPath::Hash> dirtyMaterials;

    for (const pxr::SdfPath& path : notice.GetChangedInfoOnlyPaths()) {
        if (!path.IsPropertyPath()) {
            continue;
        }

        const std::string propNameStr = path.GetNameToken().GetString();
        if (propNameStr.size() < 7 ||
            propNameStr.substr(0, 7) != "config:") {
            continue;
        }

        const pxr::SdfPath primPath = path.GetPrimPath();
        const pxr::UsdPrim prim = _stage->GetPrimAtPath(primPath);
        if (!prim || !prim.IsA<pxr::UsdShadeMaterial>()) {
            continue;
        }

        dirtyMaterials.insert(primPath);
    }

    if (dirtyMaterials.empty()) {
        return;
    }

    for (const pxr::SdfPath& primPath : dirtyMaterials) {
        const pxr::UsdPrim prim = _stage->GetPrimAtPath(primPath);
        if (!prim) {
            continue;
        }

        pxr::UsdShadeMaterial material(prim);
        auto dirtyInput = material.GetInput(pxr::TfToken("__ruzino_dirty"));
        if (!dirtyInput) {
            dirtyInput = material.CreateInput(
                pxr::TfToken("__ruzino_dirty"),
                pxr::SdfValueTypeNames->Bool);
        }

        // Toggle value to ensure a change is detected
        bool currentVal = false;
        dirtyInput.Get(&currentVal);
        dirtyInput.Set(!currentVal);
    }
}

RUZINO_NAMESPACE_CLOSE_SCOPE
