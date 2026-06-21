#include "GCore/Components/PointsComponent.h"
#include "GCore/geom_payload.hpp"
#include "geom_node_base.h"
#include "nodes/core/io/json.hpp"
#include "pxr/base/tf/ostreamMethods.h"
#include "pxr/base/vt/typeHeaders.h"
NODE_DEF_OPEN_SCOPE

struct AddedPoints {
    std::vector<glm::vec3> points;

    static constexpr bool has_storage = false;
};

NODE_DECLARATION_FUNCTION(geom_add_point)
{
    b.add_input<float>("Width").min(0.001).max(1).default_val(0.1f);

    b.add_output<Geometry>("Points");
}

NODE_EXECUTION_FUNCTION(geom_add_point)
{
    auto& storage = params.get_storage<AddedPoints&>();

    auto pick = params.get_global_payload<GeomPayload>().pick;
    if (pick) {
        storage.points.push_back(glm::vec3(pick->point));
    }

    params.set_storage(storage);

    auto width = params.get_input<float>("Width");

    auto geometry = Geometry();
    auto points_component = std::make_shared<PointsComponent>(&geometry);
    geometry.attach_component(points_component);

    std::vector widths(storage.points.size(), width);

    points_component->set_vertices(storage.points);
    points_component->set_width(widths);

    params.set_output("Points", geometry);
    return true;
}

NODE_DECLARATION_UI(geom_add_point);
NODE_DECLARATION_ALWAYS_DIRTY(geom_add_point);
// geom_add_point must always re-cook, like brush_capture: its real input is
// the live pick event in GeomPayload, not a graph socket, so it does not
// propagate dirty state through the executor. Without ALWAYS_DIRTY the node
// is cooked once and its per-cook storage reset, so only the last pick is
// kept instead of accumulating.

NODE_DEF_CLOSE_SCOPE
