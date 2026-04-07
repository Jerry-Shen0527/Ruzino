#pragma once

#include <glm/glm.hpp>

namespace TreeGen {

// Tree generation parameters based on Stava et al. 2014
// "Inverse Procedural Modelling of Trees"
struct TreeParameters {
    // ========== Geometric Parameters ==========

    // Apical angle variance (controls branching randomness)
    float apical_angle_variance = 38.0f;  // degrees

    // Number of lateral buds per internode
    int num_lateral_buds = 4;

    // Branching angle mean and variance
    float branching_angle_mean = 45.0f;  // degrees
    float branching_angle_variance = 10.0f;

    // Roll angle mean and variance (phyllotaxis)
    float roll_angle_mean = 137.5f;  // golden angle
    float roll_angle_variance = 5.0f;

    // Growth rate (number of internodes per shoot)
    float growth_rate = 3.0f;

    // Internode base length
    float internode_base_length = 0.3f;

    // Internode length age factor (length decay)
    float internode_length_age_factor = 0.95f;

    // Apical control level (trunk dominance)
    float apical_control = 2.0f;

    // Apical control age factor
    float apical_control_age_factor = 0.98f;

    // ========== Bud Fate Parameters ==========

    // Apical bud death probability
    float apical_bud_death = 0.01f;

    // Lateral bud death probability
    float lateral_bud_death = 0.05f;

    // Apical light factor (light influence on apical buds)
    float apical_light_factor = 0.7f;

    // Lateral light factor (light influence on lateral buds)
    float lateral_light_factor = 0.5f;

    // Apical dominance base factor (auxin production)
    float apical_dominance_base = 1.0f;

    // Apical dominance distance factor (auxin decay)
    float apical_dominance_distance = 0.1f;

    // Apical dominance age factor
    float apical_dominance_age = 0.99f;

    // ========== Environmental Parameters (Plastic Trees - Pirk et al. 2012)
    // ==========

    // Phototropism strength (bending towards light)
    float phototropism = 0.3f;

    // Gravitropism strength (bending due to gravity)
    float gravitropism = 0.2f;

    // Obstacle avoidance strength
    float obstacle_avoidance = 0.5f;

    // Pruning factor (shadow-induced branch death)
    float pruning_factor = 0.1f;

    // Low branch pruning factor (height below which branches are removed)
    float low_branch_pruning_factor = 0.5f;

    // Gravity bending strength (structural bending)
    float gravity_bending_strength = 0.1f;

    // Gravity bending angle factor
    float gravity_bending_angle = 0.05f;

    // ========== Plastic Trees Specific Parameters ==========

    // Enable dynamic environmental adaptation
    bool enable_plasticity = true;

    // Environmental sensitivity (how much environment affects growth)
    float environmental_sensitivity = 0.5f;

    // Light clustering radius for leaf cluster generation
    float leaf_cluster_radius = 0.5f;

    // Cluster translucency (0=opaque, 1=transparent)
    float cluster_translucency = 0.5f;

    // Minimum illumination threshold for branch survival
    float min_illumination = 0.1f;

    // Branch flexibility (how easily branches bend to environment)
    float branch_flexibility = 0.3f;

    // ========== Leaf Parameters ==========

    // Generate leaves on branches
    bool generate_leaves = true;

    // Only generate leaves on terminal branches (last 2-3 levels)
    bool leaves_on_terminal_only = true;

    // Number of terminal levels to generate leaves on
    int leaf_terminal_levels = 3;

    // Number of leaves per internode
    int leaves_per_internode = 4;

    // Leaf size base (scales with branch level)
    float leaf_size_base = 0.15f;

    // Leaf size variation
    float leaf_size_variance = 0.03f;

    // Leaf length to width ratio
    float leaf_aspect_ratio = 2.0f;  // length / width

    // Minimum branch level to generate leaves (0=trunk)
    int min_leaf_level = 1;

    // Leaf rotation randomness (degrees)
    float leaf_rotation_variance = 30.0f;

    // Leaf phyllotaxis angle (like branches)
    float leaf_phyllotaxis_angle = 137.5f;  // golden angle

    // Leaf inclination angle from horizontal (0=horizontal, 90=vertical)
    float leaf_inclination_mean = 45.0f;  // degrees
    float leaf_inclination_variance = 15.0f;

    // Leaf bending factor (0=flat, 1=curved)
    float leaf_curvature = 0.2f;

    // Phototropic response for leaves (bend towards light)
    float leaf_phototropism = 0.5f;

    // ========== Simulation Parameters ==========

    // Growth time (years/iterations)
    int growth_time = 10;

    // Initial branch radius
    float initial_radius = 0.05f;

    // Branch thickness ratio (child/parent)
    float thickness_ratio = 0.7f;

    // Light direction (normalized)
    glm::vec3 light_direction = glm::vec3(0.0f, 1.0f, 0.0f);

    // Gravity direction (normalized)
    glm::vec3 gravity_direction = glm::vec3(0.0f, -1.0f, 0.0f);

    // Random seed
    int random_seed = 42;
};

}  // namespace TreeGen
