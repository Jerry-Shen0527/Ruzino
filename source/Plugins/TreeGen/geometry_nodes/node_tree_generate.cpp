#include <cmath>
#include <cstdlib>
#include <functional>
#include <glm/gtx/rotate_vector.hpp>
#include <unordered_map>

#include "GCore/Components/CurveComponent.h"
#include "GCore/Components/MeshComponent.h"
#include "TreeGen/TreeGrowth.h"
#include "TreeGen/TreeParameters.h"
#include "TreeGen/TreeStructure.h"
#include "geom_node_base.h"

using namespace TreeGen;

NODE_DEF_OPEN_SCOPE

NODE_DECLARATION_FUNCTION(tree_generate)
{
    // Tree parameters
    b.add_input<int>("Growth Years").min(1).max(30).default_val(8);
    b.add_input<int>("Random Seed").min(0).max(10000).default_val(42);

    // Geometric parameters
    b.add_input<float>("Apical Angle Variance")
        .min(0.0f)
        .max(90.0f)
        .default_val(38.0f);
    b.add_input<int>("Lateral Buds").min(1).max(10).default_val(4);
    b.add_input<float>("Branch Angle").min(10.0f).max(90.0f).default_val(38.0f);
    b.add_input<float>("Growth Rate").min(0.1f).max(10.0f).default_val(0.98f);
    b.add_input<float>("Internode Length")
        .min(0.05f)
        .max(2.0f)
        .default_val(1.02f);
    b.add_input<float>("Apical Control").min(0.1f).max(5.0f).default_val(2.4f);

    // Bud fate parameters
    b.add_input<float>("Apical Dominance")
        .min(0.0f)
        .max(5.0f)
        .default_val(1.0f);
    b.add_input<float>("Light Factor").min(0.0f).max(2.0f).default_val(1.13f);

    // Environmental parameters (Plastic Trees - Pirk et al. 2012)
    b.add_input<bool>("Enable Plasticity").default_val(true);
    b.add_input<float>("Environmental Sensitivity")
        .min(0.0f)
        .max(1.0f)
        .default_val(0.5f);
    b.add_input<float>("Phototropism").min(0.0f).max(1.0f).default_val(0.3f);
    b.add_input<float>("Gravitropism").min(0.0f).max(1.0f).default_val(0.61f);
    b.add_input<float>("Branch Flexibility")
        .min(0.0f)
        .max(1.0f)
        .default_val(0.3f);
    b.add_input<float>("Min Illumination")
        .min(0.0f)
        .max(1.0f)
        .default_val(0.1f);
    b.add_input<float>("Cluster Translucency")
        .min(0.0f)
        .max(1.0f)
        .default_val(0.5f);

    // Leaf parameters
    b.add_input<bool>("Generate Leaves").default_val(true);
    b.add_input<bool>("Terminal Leaves Only").default_val(true);
    b.add_input<int>("Leaf Terminal Levels").min(1).max(10).default_val(3);
    b.add_input<int>("Leaves Per Internode").min(0).max(30).default_val(14);
    b.add_input<float>("Leaf Size").min(0.01f).max(1.0f).default_val(0.28f);
    b.add_input<float>("Leaf Aspect Ratio")
        .min(0.5f)
        .max(10.0f)
        .default_val(2.2f);
    b.add_input<float>("Leaf Inclination")
        .min(0.0f)
        .max(90.0f)
        .default_val(50.0f);
    b.add_input<float>("Leaf Phototropism")
        .min(0.0f)
        .max(1.0f)
        .default_val(0.5f);
    b.add_input<float>("Leaf Curvature").min(0.0f).max(1.0f).default_val(0.2f);

    // Output
    b.add_output<Geometry>("Tree Branches");
    b.add_output<Geometry>("Leaves");
}

NODE_EXECUTION_FUNCTION(tree_generate)
{
    // Get parameters from inputs
    TreeParameters tree_params;

    tree_params.growth_time = params.get_input<int>("Growth Years");
    tree_params.random_seed = params.get_input<int>("Random Seed");
    tree_params.apical_angle_variance =
        params.get_input<float>("Apical Angle Variance");
    tree_params.num_lateral_buds = params.get_input<int>("Lateral Buds");
    tree_params.branching_angle_mean = params.get_input<float>("Branch Angle");
    tree_params.growth_rate = params.get_input<float>("Growth Rate");
    tree_params.internode_base_length =
        params.get_input<float>("Internode Length");
    tree_params.apical_control = params.get_input<float>("Apical Control");
    tree_params.apical_dominance_base =
        params.get_input<float>("Apical Dominance");
    tree_params.lateral_light_factor = params.get_input<float>("Light Factor");

    // Plastic Trees parameters
    tree_params.enable_plasticity = params.get_input<bool>("Enable Plasticity");
    tree_params.environmental_sensitivity =
        params.get_input<float>("Environmental Sensitivity");
    tree_params.phototropism = params.get_input<float>("Phototropism");
    tree_params.gravitropism = params.get_input<float>("Gravitropism");
    tree_params.branch_flexibility =
        params.get_input<float>("Branch Flexibility");
    tree_params.min_illumination = params.get_input<float>("Min Illumination");
    tree_params.cluster_translucency =
        params.get_input<float>("Cluster Translucency");

    // Leaf parameters
    tree_params.generate_leaves = params.get_input<bool>("Generate Leaves");
    tree_params.leaves_on_terminal_only =
        params.get_input<bool>("Terminal Leaves Only");
    tree_params.leaf_terminal_levels =
        params.get_input<int>("Leaf Terminal Levels");
    tree_params.leaves_per_internode =
        params.get_input<int>("Leaves Per Internode");
    tree_params.leaf_size_base = params.get_input<float>("Leaf Size");
    tree_params.leaf_aspect_ratio =
        params.get_input<float>("Leaf Aspect Ratio");
    tree_params.leaf_inclination_mean =
        params.get_input<float>("Leaf Inclination");
    tree_params.leaf_phototropism =
        params.get_input<float>("Leaf Phototropism");
    tree_params.leaf_curvature = params.get_input<float>("Leaf Curvature");

    // Create tree growth system
    TreeGrowth growth(tree_params);

    // Initialize and grow tree, then place foliage along terminal branches
    // (Stava et al. 2014, Section 4.2)
    TreeStructure tree = growth.initialize_tree();
    growth.grow_tree(tree, tree_params.growth_time);
    growth.generate_foliage(tree);

    if (std::getenv("TREEGEN_STATS") != nullptr) {
        std::unordered_map<int, int> level_count;
        int terminals = 0;
        int bare_terminals = 0;
        float max_height = 0.0f;
        std::function<void(const std::shared_ptr<TreeBranch>&)> walk =
            [&](const std::shared_ptr<TreeBranch>& branch) {
                if (!branch)
                    return;
                level_count[branch->level]++;
                if (branch->children.empty()) {
                    terminals++;
                    if (branch->leaves.empty()) {
                        if (bare_terminals < 5) {
                            fprintf(stderr,
                                    "[treegen]   bare terminal lvl=%d "
                                    "len=%.2f age=%d pos=(%.2f,%.2f,%.2f)\n",
                                    branch->level, branch->length,
                                    branch->age, branch->end_position.x,
                                    branch->end_position.y,
                                    branch->end_position.z);
                        }
                        bare_terminals++;
                    }
                }
                max_height = std::max(max_height, branch->end_position.y);
                for (const auto& child : branch->children)
                    walk(child);
            };
        walk(tree.root);
        fprintf(stderr, "[treegen] age=%d branches=%zu terminals=%d "
                        "bare=%d leaves=%zu height=%.2f levels=%zu\n",
                tree.current_age, tree.all_branches.size(), terminals,
                bare_terminals, tree.all_leaves.size(), max_height,
                level_count.size());
        for (auto& [level, count] : level_count) {
            fprintf(stderr, "[treegen]   level %d: %d branches\n", level,
                    count);
        }
    }

    // Convert the tree to polylines for meshing: one polyline per maximal
    // unbranched shoot chain, with one width per point. Segment i tapers
    // from the width at its start point to the width at its end point, so
    // the swept tube is continuous through every node (no ring steps at
    // internode joints). At a fork the child chain starts with the parent's
    // radius, sealing the joint with a short cone.
    Geometry curve_geom = Geometry::CreateCurve();
    auto curve = curve_geom.get_component<CurveComponent>();

    std::vector<glm::vec3> curve_vertices;
    std::vector<int> curve_counts;
    std::vector<float> curve_widths;

    std::function<void(std::shared_ptr<TreeBranch>)> emit_chain;
    emit_chain = [&](std::shared_ptr<TreeBranch> branch) {
        if (!branch)
            return;

        bool chain_start =
            (branch->parent == nullptr) ||
            (branch->parent->children.size() >= 2);

        if (chain_start) {
            std::vector<glm::vec3> pts;
            std::vector<float> ws;

            pts.push_back(branch->start_position);
            ws.push_back(branch->parent ? branch->parent->radius
                                        : branch->radius);

            std::shared_ptr<TreeBranch> cur = branch;
            while (cur && cur->children.size() == 1) {
                if (glm::length(cur->end_position - pts.back()) > 1e-5f) {
                    pts.push_back(cur->end_position);
                    ws.push_back(cur->radius);
                }
                cur = cur->children[0];
            }
            if (cur) {
                if (glm::length(cur->end_position - pts.back()) > 1e-5f) {
                    pts.push_back(cur->end_position);
                    ws.push_back(cur->radius);
                }
            }

            curve_vertices.insert(curve_vertices.end(), pts.begin(), pts.end());
            curve_widths.insert(curve_widths.end(), ws.begin(), ws.end());
            curve_counts.push_back(static_cast<int>(pts.size()));
        }

        for (auto& child : branch->children) {
            emit_chain(child);
        }
    };

    if (tree.root) {
        emit_chain(tree.root);
    }

    curve->set_vertices(curve_vertices);
    curve->set_curve_counts(curve_counts);
    curve->set_widths(curve_widths);
    curve->set_periodic(false);

    params.set_output("Tree Branches", curve_geom);

    // Generate leaf geometry as mesh
    Geometry leaf_geom = Geometry::CreateMesh();
    auto leaf_mesh = leaf_geom.get_component<MeshComponent>();

    std::vector<glm::vec3> leaf_vertices;
    std::vector<int> leaf_face_counts;
    std::vector<int> leaf_face_indices;
    std::vector<glm::vec3> leaf_normals;

    // Generate all leaves
    for (const auto& branch : tree.all_branches) {
        if (!branch)
            continue;

        for (const auto& leaf : branch->leaves) {
            if (!leaf)
                continue;

            int base_idx = leaf_vertices.size();

            // Use new leaf properties: length and width
            float half_length = leaf->length * 0.5f;
            float half_width = leaf->width * 0.5f;

            // Use the complete coordinate system from leaf structure
            glm::vec3 leaf_normal =
                glm::normalize(leaf->normal);  // perpendicular to leaf surface
            glm::vec3 leaf_tangent =
                glm::normalize(leaf->tangent);  // along leaf length
            glm::vec3 leaf_binormal =
                glm::normalize(leaf->binormal);  // along leaf width

            // Apply inclination angle
            // Rotate the tangent and binormal around the binormal axis
            float incl = leaf->inclination;
            if (std::abs(incl) > 0.01f) {
                // Rotate tangent towards normal
                glm::vec3 rot_axis = leaf_binormal;
                leaf_tangent = glm::rotate(leaf_tangent, incl, rot_axis);
                leaf_normal = glm::rotate(leaf_normal, incl, rot_axis);
            }

            // Apply rotation around normal
            if (std::abs(leaf->rotation) > 0.01f) {
                leaf_tangent =
                    glm::rotate(leaf_tangent, leaf->rotation, leaf_normal);
                leaf_binormal =
                    glm::rotate(leaf_binormal, leaf->rotation, leaf_normal);
            }

            // Create leaf vertices in diamond/ellipse shape
            // More realistic leaf shape with curved edges
            glm::vec3 v0 = leaf->position + leaf_tangent * half_length;  // tip
            glm::vec3 v1 =
                leaf->position + leaf_binormal * half_width;  // right
            glm::vec3 v2 = leaf->position - leaf_tangent * half_length;  // base
            glm::vec3 v3 = leaf->position - leaf_binormal * half_width;  // left

            // Apply curvature (gentle lengthwise fold — strong folds read
            // as spiky arrows at crown scale)
            if (leaf->curvature > 0.01f) {
                float curve_amount = leaf->curvature * half_length * 0.12f;
                // Bend the tip and base towards the normal direction
                v0 += leaf_normal * curve_amount;
                v2 += leaf_normal * curve_amount * 0.5f;
            }

            leaf_vertices.push_back(v0);
            leaf_vertices.push_back(v1);
            leaf_vertices.push_back(v2);
            leaf_vertices.push_back(v3);

            // Two triangular faces for each side (front and back for
            // double-sided rendering) Front face (visible from normal
            // direction)
            leaf_face_indices.push_back(base_idx + 0);
            leaf_face_indices.push_back(base_idx + 1);
            leaf_face_indices.push_back(base_idx + 2);
            leaf_face_counts.push_back(3);

            leaf_face_indices.push_back(base_idx + 0);
            leaf_face_indices.push_back(base_idx + 2);
            leaf_face_indices.push_back(base_idx + 3);
            leaf_face_counts.push_back(3);

            // Back face (reversed winding for back-face culling)
            leaf_face_indices.push_back(base_idx + 0);
            leaf_face_indices.push_back(base_idx + 3);
            leaf_face_indices.push_back(base_idx + 2);
            leaf_face_counts.push_back(3);

            leaf_face_indices.push_back(base_idx + 0);
            leaf_face_indices.push_back(base_idx + 2);
            leaf_face_indices.push_back(base_idx + 1);
            leaf_face_counts.push_back(3);

            // Normals - use the computed leaf normal
            for (int i = 0; i < 4; ++i) {
                leaf_normals.push_back(leaf_normal);
            }
        }
    }

    leaf_mesh->set_vertices(leaf_vertices);
    leaf_mesh->set_face_vertex_counts(leaf_face_counts);
    leaf_mesh->set_face_vertex_indices(leaf_face_indices);
    leaf_mesh->set_normals(leaf_normals);

    params.set_output("Leaves", leaf_geom);

    return true;
}

NODE_DECLARATION_UI(tree_generate);

NODE_DEF_CLOSE_SCOPE
