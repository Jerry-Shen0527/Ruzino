#include "stage/animation.h"

#include <filesystem>

#include "../../../Editor/geometry/include/GCore/geom_payload.hpp"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usdGeom/xform.h"
#include "spdlog/spdlog.h"
#include "stage/stage.hpp"

#ifdef GEOM_USD_EXTENSION
#include "GCore/usd_extension.h"
#endif

RUZINO_NAMESPACE_OPEN_SCOPE
namespace animation {

std::once_flag WithDynamicLogicPrim::init_once;
std::shared_ptr<NodeTreeDescriptor> WithDynamicLogicPrim::node_tree_descriptor =
    nullptr;

WithDynamicLogic::WithDynamicLogic(Stage* stage) : stage_(stage)
{
}

WithDynamicLogicPrim::WithDynamicLogicPrim(
    const pxr::UsdPrim& prim,
    Stage* stage)
    : WithDynamicLogic(stage),
      prim(prim)
{
    std::call_once(init_once, [&] {
        // Only using this to initialize the node descriptor
        std::shared_ptr<NodeSystem> node_system =
            create_dynamic_loading_system();

        auto loaded = node_system->load_configuration("geometry_nodes.json");
        loaded = node_system->load_configuration("basic_nodes.json");

        // Load all plugin configurations
        auto plugin_path = std::filesystem::path("./Plugins");
        if (std::filesystem::exists(plugin_path)) {
            for (auto& p : std::filesystem::directory_iterator(plugin_path)) {
                if (p.path().extension() == ".json") {
                    node_system->load_configuration(p.path().string());
                }
            }
        }

        node_tree_descriptor = node_system->node_tree_descriptor();
    });

    node_tree = std::make_shared<NodeTree>(node_tree_descriptor);
    NodeTreeExecutorDesc executor_desc;
    executor_desc.policy = NodeTreeExecutorDesc::Policy::Eager;

    node_tree_executor = create_node_tree_executor(executor_desc);

    // Load modifier stack (modifier_0_json, modifier_1_json, etc.)
    modifier_stack_ = load_modifier_stack(prim);

    // If no modifier stack found, check for legacy node_json and convert to
    // single modifier
    if (modifier_stack_.empty()) {
        auto json_path = prim.GetAttribute(pxr::TfToken("node_json"));
        if (json_path) {
            auto json = pxr::VtValue();
            json_path.Get(&json);

            ModifierInfo mod;
            mod.name = "Main";
            mod.order = 0;
            mod.enabled = true;
            mod.node_graph_json = json.Get<std::string>();
            modifier_stack_.modifiers.push_back(mod);

            tree_desc_cache = mod.node_graph_json;
            node_tree->deserialize(tree_desc_cache);
        }
        else {
            return;  // No node graph at all
        }
    }

    // Always use modifier mode for non-destructive editing
    use_modifier_mode = true;
}

WithDynamicLogicPrim::WithDynamicLogicPrim(const WithDynamicLogicPrim& prim)
    : WithDynamicLogic(prim.stage_)
{
    this->prim = prim.prim;
    this->node_tree = prim.node_tree;
    this->use_modifier_mode = prim.use_modifier_mode;
    this->modifier_stack_ = prim.modifier_stack_;
    NodeTreeExecutorDesc executor_desc;

    executor_desc.policy = NodeTreeExecutorDesc::Policy::Eager;

    this->node_tree_executor = create_node_tree_executor(executor_desc);
}

WithDynamicLogicPrim& WithDynamicLogicPrim::operator=(
    const WithDynamicLogicPrim& prim)
{
    this->prim = prim.prim;
    this->node_tree = prim.node_tree;
    this->use_modifier_mode = prim.use_modifier_mode;
    this->modifier_stack_ = prim.modifier_stack_;
    NodeTreeExecutorDesc executor_desc;

    executor_desc.policy = NodeTreeExecutorDesc::Policy::Eager;

    this->node_tree_executor = create_node_tree_executor(executor_desc);
    return *this;
}

void WithDynamicLogicPrim::update(float delta_time) const
{
    update_modifier_stack(delta_time);
}

void WithDynamicLogicPrim::update_single_modifier(float delta_time) const
{
    // This function is kept for backwards compatibility but now uses modifier
    // mode
    auto json_path = prim.GetAttribute(pxr::TfToken("node_json"));
    if (!json_path) {
        return;
    }

    auto json = pxr::VtValue();
    json_path.Get(&json);

    auto new_tree_desc = json.Get<std::string>();

    if (tree_desc_cache != new_tree_desc) {
        tree_desc_cache = new_tree_desc;
        node_tree->deserialize(tree_desc_cache);
        node_tree_executor->mark_tree_structure_changed();

        // Clear old time sample data
        clear_time_samples(prim);

        // Reset this prim's own time state
        prim_current_time = pxr::UsdTimeCode(0.0f);
        prim_render_time = pxr::UsdTimeCode(0.0f);

        stage_->set_render_time(0.0f);
        simulation_begun = false;

        // Ensure modifier layer exists
        ensure_modifier_layer();
    }

    // Get current render time from Stage, update prim's render time
    prim_render_time = stage_->get_render_time();

    // Use this prim's own should_simulate to decide
    if (!should_simulate())
        return;

    assert(node_tree);
    assert(node_tree_executor);

    ensure_modifier_layer();

    auto& payload = node_tree_executor->get_global_payload<GeomPayload&>();
    payload.delta_time = delta_time;
#ifdef GEOM_USD_EXTENSION
    payload.stage = prim.GetStage();
    payload.prim_path = prim.GetPath();
    payload.is_modifier_mode = true;  // Now always use modifier mode
    payload.modifier_layer = modifier_stack_.modifier_layer;
    payload.current_time = prim_current_time;
    payload.current_modifier_index = 0;
    payload.modifier_input_path = prim.GetPath();  // Read from original prim
    payload.modifier_output_path =
        prim.GetPath();  // Write to same prim path (as over spec)
#endif
    payload.has_simulation = false;
    payload.is_simulating = simulation_begun;
    if (!simulation_begun) {
        simulation_begun = true;
    }

    node_tree_executor->execute(node_tree.get());

    // Update this prim's simulation time
    auto current = prim_current_time.GetValue();
    current += delta_time;
    prim_current_time = pxr::UsdTimeCode(current);
}

void WithDynamicLogicPrim::update_modifier_stack(float delta_time) const
{
    auto new_stack = load_modifier_stack(prim);

    // If no modifier_*_json found, check for legacy node_json
    if (new_stack.empty()) {
        auto json_path = prim.GetAttribute(pxr::TfToken("node_json"));
        if (json_path) {
            pxr::VtValue json_value;
            json_path.Get(&json_value);

            ModifierInfo mod;
            mod.name = "Main";
            mod.order = 0;
            mod.enabled = true;
            mod.node_graph_json = json_value.Get<std::string>();
            new_stack.modifiers.push_back(mod);
        }
    }

    bool stack_changed = false;

    if (!modifier_stack_.modifiers.empty()) {
        stack_changed =
            (new_stack.modifiers.size() != modifier_stack_.modifiers.size());

        if (!stack_changed) {
            for (size_t i = 0; i < new_stack.modifiers.size(); ++i) {
                if (new_stack.modifiers[i].node_graph_json !=
                    modifier_stack_.modifiers[i].node_graph_json) {
                    stack_changed = true;
                    break;
                }
            }
        }
    }

    if (stack_changed) {
        modifier_stack_ = new_stack;
        ensure_modifier_layer();

        // Clear modifier layer's over specs when modifier stack changes
        // This ensures original geometry is shown when modifier graph is
        // changed
        if (modifier_stack_.modifier_layer) {
            // Remove the prim spec from modifier layer completely
            auto prim_spec =
                modifier_stack_.modifier_layer->GetPrimAtPath(prim.GetPath());

            if (prim_spec) {
                // Collect property specs first (can't iterate while removing)
                std::vector<pxr::SdfPropertySpecHandle> props_to_remove;
                for (auto it = prim_spec->GetProperties().begin();
                     it != prim_spec->GetProperties().end();
                     ++it) {
                    if (*it) {
                        props_to_remove.push_back(*it);
                    }
                }

                // Remove each property
                for (auto& prop : props_to_remove) {
                    prim_spec->RemoveProperty(prop);
                }

                // Clear the type name too - this is critical!
                prim_spec->SetTypeName(pxr::TfToken());
            }
        }

        // Reset time state
        prim_current_time = pxr::UsdTimeCode(0.0f);
        prim_render_time = pxr::UsdTimeCode(0.0f);
        stage_->set_render_time(0.0f);
        simulation_begun = false;
    }

    prim_render_time = stage_->get_render_time();

    if (!should_simulate())
        return;

    ensure_modifier_layer();

    assert(node_tree);
    assert(node_tree_executor);

    // Execute each modifier in order (chain execution)
    for (size_t i = 0; i < modifier_stack_.modifiers.size(); ++i) {
        const auto& mod = modifier_stack_.modifiers[i];

        if (!mod.enabled) {
            continue;
        }

        // Load this modifier's node graph
        node_tree->deserialize(mod.node_graph_json);
        node_tree_executor->mark_tree_structure_changed();

        auto& payload = node_tree_executor->get_global_payload<GeomPayload&>();
        payload.delta_time = delta_time;
#ifdef GEOM_USD_EXTENSION
        payload.stage = prim.GetStage();
        payload.prim_path = prim.GetPath();
        payload.is_modifier_mode = true;
        payload.modifier_layer = modifier_stack_.modifier_layer;
        payload.current_time = prim_current_time;
        payload.current_modifier_index = static_cast<int>(i);

        // For single modifier (node_json case), write to the prim path itself
        // as over spec For multiple modifiers, use separate output paths
        if (modifier_stack_.modifiers.size() == 1) {
            payload.modifier_output_path = prim.GetPath();
            payload.modifier_input_path = prim.GetPath();
        }
        else {
            payload.modifier_output_path =
                get_modifier_output_path(prim.GetPath(), static_cast<int>(i));

            // Set input path: first modifier reads from original prim,
            // subsequent modifiers read from previous modifier's output
            if (i == 0) {
                payload.modifier_input_path = prim.GetPath();
            }
            else {
                payload.modifier_input_path = get_modifier_output_path(
                    prim.GetPath(), static_cast<int>(i - 1));
            }
        }
#endif
        payload.has_simulation = false;
        payload.is_simulating = simulation_begun;

        // Execute this modifier
        node_tree_executor->execute(node_tree.get());
    }

    if (!simulation_begun) {
        simulation_begun = true;
    }

    // Update simulation time
    auto current = prim_current_time.GetValue();
    current += delta_time;
    prim_current_time = pxr::UsdTimeCode(current);
}

ModifierStack WithDynamicLogicPrim::load_modifier_stack(
    const pxr::UsdPrim& prim) const
{
    ModifierStack stack;

    // Look for modifier_0_json, modifier_1_json, etc.
    int index = 0;
    while (true) {
        std::string attr_name = "modifier_" + std::to_string(index) + "_json";
        auto attr = prim.GetAttribute(pxr::TfToken(attr_name.c_str()));

        if (!attr) {
            break;  // No more modifiers
        }

        pxr::VtValue json_value;
        attr.Get(&json_value);

        ModifierInfo mod;
        mod.name = "Modifier_" + std::to_string(index);
        mod.order = index;
        mod.enabled = true;  // Could add enabled attribute check
        mod.node_graph_json = json_value.Get<std::string>();

        stack.modifiers.push_back(mod);
        index++;
    }

    if (!stack.empty()) {
        spdlog::debug(
            "[load_modifier_stack] Found {} modifiers for prim: {}",
            stack.size(),
            prim.GetPath().GetString());
    }

    return stack;
}

void WithDynamicLogicPrim::ensure_modifier_layer() const
{
    if (!modifier_stack_.modifier_layer) {
        if (stage_) {
            modifier_stack_.modifier_layer = stage_->get_modifier_layer();
            spdlog::debug(
                "[ensure_modifier_layer] Using stage modifier layer for prim: "
                "{}",
                prim.GetPath().GetString());
        }
        else {
            spdlog::warn(
                "[ensure_modifier_layer] No stage available for modifier "
                "layer");
        }
    }
}

bool WithDynamicLogicPrim::is_animatable(const pxr::UsdPrim& prim)
{
    auto animatable = prim.GetAttribute(pxr::TfToken("Animatable"));

    if (!animatable) {
        return false;
    }
    bool is_animatable = false;
    animatable.Get(&is_animatable);
    return is_animatable;
}

void WithDynamicLogicPrim::clear_time_samples(const pxr::UsdPrim& prim) const
{
    if (!prim.IsValid()) {
        return;
    }

    // Recursively clear time samples for all child prims
    for (const auto& child : prim.GetChildren()) {
        clear_time_samples(child);
    }

    // Clear time samples for all attributes of current prim
    for (const auto& attr : prim.GetAttributes()) {
        // Skip attributes that shouldn't be cleared (configuration-related
        // attributes)
        const auto& attr_name = attr.GetName();
        if (attr_name == pxr::TfToken("node_json") ||
            attr_name == pxr::TfToken("Animatable")) {
            continue;
        }

        // Also skip modifier attributes
        std::string name_str = attr_name.GetString();
        if (name_str.find("modifier_") == 0 &&
            name_str.find("_json") != std::string::npos) {
            continue;
        }

        // Check if attribute has time samples
        if (attr.GetNumTimeSamples() > 0) {
            // Clear all time samples
            attr.Clear();
        }
    }
}
}  // namespace animation

RUZINO_NAMESPACE_CLOSE_SCOPE
