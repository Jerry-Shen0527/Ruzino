#pragma once
#include "api.h"
#include "camera.h"
#include "pxr/imaging/hd/aov.h"
#include "pxr/imaging/hd/renderThread.h"
#include "pxr/pxr.h"

RUZINO_NAMESPACE_OPEN_SCOPE
class Hd_RUZINO_RenderParam;
using namespace pxr;
class Hd_RUZINO_Renderer {
   public:
    explicit Hd_RUZINO_Renderer(Hd_RUZINO_RenderParam* render_param);

    virtual ~Hd_RUZINO_Renderer();
    void SetAovBindings(const HdRenderPassAovBindingVector& aovBindings);
    virtual void Render(HdRenderThread* render_thread);
    void Clear();

    void MarkAovBuffersUnconverged();

    void renderTimeUpdateCamera(
        const HdRenderPassStateSharedPtr& renderPassState);
    bool nodetree_modified();
    bool nodetree_modified(bool new_status);

   protected:
    void _RenderTiles(
        HdRenderThread* renderThread,
        size_t tileStart,
        size_t tileEnd);
    static GfVec4f _GetClearColor(const VtValue& clearValue);

    bool _enableSceneColors;
    std::atomic<int> _completedSamples;

    int _ambientOcclusionSamples = 16;
    // A callback that interprets embree error codes and injects them into
    // the hydra logging system.

    // The bound aovs for this renderer.
    HdRenderPassAovBindingVector _aovBindings;
    // Parsed AOV name tokens.
    HdParsedAovTokenVector _aovNames;
    // Do the aov bindings need to be re-validated?
    bool _aovBindingsNeedValidation = true;
    // Are the aov bindings valid?
    bool _aovBindingsValid = false;

    const Hd_RUZINO_Camera* camera_ = nullptr;
    Hd_RUZINO_RenderParam* render_param;
};

RUZINO_NAMESPACE_CLOSE_SCOPE
