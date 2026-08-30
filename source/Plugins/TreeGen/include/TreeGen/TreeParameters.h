#pragma once

#include <glm/glm.hpp>

namespace TreeGen {

// Tree generation parameters based on Stava et al. 2014
// "Inverse Procedural Modelling of Trees"
// Default values chosen within the ranges of the paper's Table 2 (the six
// example trees F6a-F6f) unless noted otherwise.
struct TreeParameters {
    // ========== Geometric Parameters ==========

    // Apical angle variance (controls branching randomness)
    float apical_angle_variance = 38.0f;  // degrees (Table 2 F6a)

    // Number of lateral buds per internode
    int num_lateral_buds = 4;  // (Table 2: 1-4 for deciduous)

    // Branching angle mean and variance
    float branching_angle_mean = 38.0f;  // degrees (Table 2 F6a)
    float branching_angle_variance = 2.0f;  // (F6a)

    // Roll angle mean and variance (per-internode phyllotaxis increment)
    float roll_angle_mean = 91.0f;  // degrees (Table 2 F6a)
    float roll_angle_variance = 1.0f;  // (F6a)

    // Growth rate (number of internodes per shoot)
    float growth_rate = 0.98f;  // (Table 2 F6a)

    // Internode base length
    float internode_base_length = 1.02f;  // (Table 2 F6a)

    // Internode length age factor (length decay)
    float internode_length_age_factor = 0.97f;  // (F6a)

    // Apical control level (trunk dominance)
    float apical_control = 2.4f;  // (Table 2 F6a)

    // Apical control age factor
    float apical_control_age_factor = 0.85f;  // (F6a)

    // ========== Bud Fate Parameters ==========

    // Apical bud extinction rate
    float apical_bud_death = 0.0f;  // (Table 2: ~0)

    // Lateral bud extinction rate
    float lateral_bud_death = 0.21f;  // (Table 2 F6a)

    // Apical light factor (light influence on apical buds)
    float apical_light_factor = 0.39f;  // (Table 2 F6a)

    // Lateral light factor (light influence on lateral buds)
    float lateral_light_factor = 1.13f;  // (Table 2 F6a)

    // Apical dominance base factor (auxin production)
    float apical_dominance_base = 3.13f;  // (Table 2 F6a)

    // Apical dominance distance factor (auxin decay, phi_ADDF^d)
    float apical_dominance_distance = 0.13f;  // (Table 2 F6a)

    // Apical dominance age factor
    float apical_dominance_age = 0.82f;  // (Table 2 F6a)

    // ========== Environmental Parameters ==========

    // Phototropism strength (bending towards light)
    float phototropism = 0.29f;  // (Table 2: 0.05-0.42)

    // Gravitropism strength (bending due to gravity)
    float gravitropism = 0.61f;  // (Table 2 F6a)

    // Obstacle avoidance strength
    float obstacle_avoidance = 0.5f;

    // Pruning factor (shadow-induced branch shedding threshold)
    float pruning_factor = 0.05f;  // (Table 2 F6a)

    // Low branch pruning factor (height below which lateral branches shed)
    float low_branch_pruning_factor = 1.3f;  // (Table 2 F6a)

    // Gravity bending strength (structural bending)
    float gravity_bending_strength = 0.73f;  // (Table 2 F6a)

    // Gravity bending angle factor
    float gravity_bending_angle = 0.05f;  // (Table 2 F6a)

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

    // ========== Leaf Parameters (Section 4.2: end-of-growth foliage) ==========

    // Generate leaves on terminal branches
    bool generate_leaves = true;

    // Only generate leaves on terminal branches (last 2-3 levels)
    bool leaves_on_terminal_only = true;

    // Number of terminal levels to generate leaves on
    int leaf_terminal_levels = 3;

    // Leaves per terminal-shoot internode
    int leaves_per_internode = 14;

    // Leaf size base
    float leaf_size_base = 0.28f;

    // Leaf size variation
    float leaf_size_variance = 0.04f;

    // Leaf length to width ratio
    float leaf_aspect_ratio = 2.2f;  // length / width

    // Minimum branch level to generate leaves when terminal-only is off
    int min_leaf_level = 1;

    // Leaf rotation randomness (degrees)
    float leaf_rotation_variance = 25.0f;

    // Leaf inclination angle from its branch (0=along branch, 90=straight out)
    float leaf_inclination_mean = 50.0f;  // degrees
    float leaf_inclination_variance = 10.0f;

    // Leaf bending factor (0=flat, 1=curved)
    float leaf_curvature = 0.2f;

    // Phototropic response for leaves (plane tilts towards light)
    float leaf_phototropism = 0.5f;

    // ========== Simulation Parameters ==========

    // Growth time (years/iterations)
    int growth_time = 8;  // (Table 2 F6a)

    // Initial (tip) branch radius for the pipe model
    float initial_radius = 0.02f;

    // Branch thickness ratio (kept for API compatibility; radii come from
    // the pipe model r^2 = sum(child r^2))
    float thickness_ratio = 0.7f;

    // Light direction (normalized)
    glm::vec3 light_direction = glm::vec3(0.0f, 1.0f, 0.0f);

    // Gravity direction (normalized)
    glm::vec3 gravity_direction = glm::vec3(0.0f, -1.0f, 0.0f);

    // Random seed
    int random_seed = 42;
};

}  // namespace TreeGen
