#ifdef GEOM_USD_EXTENSION

#include "GCore/Components/MaterialComponent.h"
#include "GCore/MaterialObject.h"
#include "geom_node_base.h"
#include "spdlog/spdlog.h"
NODE_DEF_OPEN_SCOPE
NODE_DECLARATION_FUNCTION(set_material)
{
    b.add_input<Geometry>("Geometry");
    b.add_input<MaterialObject>("Material").optional(true);
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(set_material)
{
    auto geometry = params.get_input<Geometry>("Geometry");

    if (!params.has_input("Material")) {
        // Nothing wired — pass the geometry through unchanged.
        spdlog::warn(
            "[set_material] no Material connected, passing geometry "
            "through");
        params.set_output("Geometry", std::move(geometry));
        return true;
    }
    auto material_object = params.get_input<MaterialObject>("Material");

    auto component = geometry.get_component<MaterialComponent>();
    if (!component) {
        component = std::make_shared<MaterialComponent>(&geometry);
    }
    component->material_object = std::move(material_object);
    geometry.attach_component(component);

    params.set_output("Geometry", std::move(geometry));
    return true;
}

NODE_DECLARATION_UI(set_material);
NODE_DEF_CLOSE_SCOPE

#endif
