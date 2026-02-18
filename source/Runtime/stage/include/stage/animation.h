#pragma once
#include <pxr/usd/usd/prim.h>
#include <stage/api.h>
#include <mutex>

#include "nodes/core/node_exec.hpp"
#include "nodes/system/node_system.hpp"

namespace Ruzino {
class Stage;
}

RUZINO_NAMESPACE_OPEN_SCOPE
namespace animation {

class STAGE_API WithDynamicLogic {
   public:
    WithDynamicLogic(Stage* stage);
    virtual ~WithDynamicLogic() = default;
    virtual void update(float delta_time) const = 0;

   protected:
    Stage* stage_;
};

class STAGE_API WithDynamicLogicPrim : public WithDynamicLogic {
   public:
    WithDynamicLogicPrim(Stage* stage) : WithDynamicLogic(stage)
    {
    }
    WithDynamicLogicPrim(const pxr::UsdPrim& prim, Stage* stage);

    WithDynamicLogicPrim(const WithDynamicLogicPrim& prim);
    WithDynamicLogicPrim& operator=(const WithDynamicLogicPrim& prim);

    void update(float delta_time) const override;
    static bool is_animatable(const pxr::UsdPrim& prim);

    // Each prim independently decides whether simulation should proceed
    bool should_simulate() const
    {
        return prim_render_time >= prim_current_time;
    }

    pxr::UsdTimeCode get_prim_current_time() const
    {
        return prim_current_time;
    }
    void set_prim_current_time(pxr::UsdTimeCode time)
    {
        prim_current_time = time;
    }

    pxr::UsdTimeCode get_prim_render_time() const
    {
        return prim_render_time;
    }
    void set_prim_render_time(pxr::UsdTimeCode time)
    {
        prim_render_time = time;
    }

    void clear_time_samples(const pxr::UsdPrim& prim) const;

   private:
    mutable bool simulation_begun = false;

    pxr::UsdPrim prim;

    std::shared_ptr<NodeTree> node_tree;
    std::unique_ptr<NodeTreeExecutor> node_tree_executor;
    mutable std::string tree_desc_cache;

    // Each prim's independent time code
    mutable pxr::UsdTimeCode prim_current_time = pxr::UsdTimeCode(0.0f);
    mutable pxr::UsdTimeCode prim_render_time = pxr::UsdTimeCode(0.0f);

    static std::shared_ptr<NodeTreeDescriptor> node_tree_descriptor;
    static std::once_flag init_once;
};

}  // namespace animation

RUZINO_NAMESPACE_CLOSE_SCOPE
