#pragma once
#include "api.h"
#include "pxr/base/gf/ray.h"
#include "pxr/base/gf/rect2i.h"
#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hdx/renderSetupTask.h"
#include "pxr/pxr.h"
#include "renderBuffer.h"
RUZINO_NAMESPACE_OPEN_SCOPE
using namespace pxr;
class Hd_RUZINO_Camera : public HdCamera {
   public:
    explicit Hd_RUZINO_Camera(const SdfPath& id) : HdCamera(id)
    {
    }

    void Sync(
        HdSceneDelegate* sceneDelegate,
        HdRenderParam* renderParam,
        HdDirtyBits* dirtyBits) override;

    void update(const HdRenderPassStateSharedPtr& renderPassState) const;

    mutable GfRect2i _dataWindow;
    mutable GfMatrix4d _inverseProjMatrix;
    mutable GfMatrix4d _projMatrix;
    mutable GfMatrix4d _inverseViewMatrix;
    mutable GfMatrix4d _viewMatrix;
};

RUZINO_NAMESPACE_CLOSE_SCOPE
