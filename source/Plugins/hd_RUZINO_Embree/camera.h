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
    virtual GfRay generateRay(
        GfVec2f pixel_center,
        const std::function<float()>& function) const;

    void update(const HdRenderPassStateSharedPtr& renderPassState) const;

    void attachFilm(Hd_RUZINO_RenderBuffer* new_film) const;

    mutable Hd_RUZINO_RenderBuffer* film;
    mutable GfRect2i _dataWindow;

   private:
    mutable GfMatrix4d _inverseProjMatrix;
    mutable GfMatrix4d _projMatrix;
    mutable GfMatrix4d _inverseViewMatrix;
    mutable GfMatrix4d _viewMatrix;
};

RUZINO_NAMESPACE_CLOSE_SCOPE
