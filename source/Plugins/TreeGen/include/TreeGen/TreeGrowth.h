#pragma once

#include <random>

#include "TreeParameters.h"
#include "TreeStructure.h"
#include "api.h"

namespace TreeGen {

class TREEGEN_API TreeGrowth {
   public:
    TreeGrowth(const TreeParameters& params);

    // Initialize a new tree with a single trunk
    TreeStructure initialize_tree();

    // Perform one growth cycle
    void grow_one_cycle(TreeStructure& tree);

    // Grow tree for multiple cycles
    void grow_tree(TreeStructure& tree, int cycles);

   private:
    TreeParameters params_;
    std::mt19937 rng_;
    std::normal_distribution<float> normal_dist_;
    std::uniform_real_distribution<float> uniform_dist_;

    // Initialize random number generator
    void init_random();

    // Get random value from normal distribution
    float random_normal(float mean, float stddev);

    // Get random value from uniform distribution
    float random_uniform(float min = 0.0f, float max = 1.0f);

    // ===== Bud Management =====

    // Update bud states (death, dormancy)
    void update_bud_states(TreeStructure& tree);

    // Calculate illumination for all buds (simplified)
    void calculate_illumination(TreeStructure& tree);

    // Calculate auxin levels for lateral buds
    void calculate_auxin_levels(TreeStructure& tree);

    // Determine which buds will flush (grow)
    void determine_bud_flushing(TreeStructure& tree);

    // Calculate flush probability for apical bud
    float calculate_apical_flush_probability(const TreeBud& bud);

    // Calculate flush probability for lateral bud
    float calculate_lateral_flush_probability(
        const TreeBud& bud,
        const TreeStructure& tree);

    // ===== Shoot Growth =====

    // Grow a new shoot from a bud
    void grow_shoot_from_bud(TreeStructure& tree, std::shared_ptr<TreeBud> bud);

    // Calculate growth rate for a shoot based on branch level
    float calculate_growth_rate(int branch_level, int tree_age);

    // Calculate internode length based on tree age
    float calculate_internode_length(int tree_age);

    // Create internodes along a shoot
    void create_internodes(
        TreeStructure& tree,
        std::shared_ptr<TreeBranch> parent,
        const glm::vec3& start_pos,
        const glm::vec3& initial_dir,
        int num_internodes,
        int branch_level);

    // ===== Direction and Tropism =====

    // Calculate new growth direction with tropisms
    glm::vec3 calculate_growth_direction(
        const glm::vec3& bud_direction,
        const glm::vec3& position,
        float illumination);

    // Apply phototropism (bending towards light)
    glm::vec3 apply_phototropism(const glm::vec3& direction, float strength);

    // Apply gravitropism (bending due to gravity)
    glm::vec3 apply_gravitropism(const glm::vec3& direction, float strength);

    // ===== Lateral Bud Creation =====

    // Create lateral buds along a branch
    void create_lateral_buds(std::shared_ptr<TreeBranch> branch);

    // Calculate branching direction for lateral bud
    glm::vec3 calculate_lateral_direction(
        const glm::vec3& parent_dir,
        int bud_index,
        int total_buds);

    // ===== Structural Updates =====

    // Update branch radii based on pipe model
    void update_branch_radii(TreeStructure& tree);

    // Apply structural bending due to weight
    void apply_structural_bending(TreeStructure& tree);

    // Prune branches based on light and height
    void prune_branches(TreeStructure& tree);

    // ===== Leaf Generation =====

    // Create leaves on a branch
    void create_leaves(std::shared_ptr<TreeBranch> branch);

    // Calculate leaf normal direction with phototropism
    glm::vec3 calculate_leaf_normal(
        const glm::vec3& branch_dir,
        const glm::vec3& position,
        int leaf_index);

    // Calculate leaf orientation (tangent, binormal) from normal
    void calculate_leaf_orientation(
        const glm::vec3& normal,
        const glm::vec3& branch_dir,
        glm::vec3& tangent,
        glm::vec3& binormal);

    // Calculate leaf inclination angle based on position and light
    float calculate_leaf_inclination(
        const glm::vec3& position,
        float height_factor);

    // Check if branch should have leaves (terminal branch check)
    bool should_have_leaves(
        std::shared_ptr<TreeBranch> branch,
        int max_tree_depth);

    // ===== Plastic Trees Methods (Pirk et al. 2012) =====

    // Create leaf clusters for efficient illumination computation
    void create_leaf_clusters(TreeStructure& tree);

    // Calculate illumination using leaf cluster shadows
    void calculate_illumination_with_clusters(TreeStructure& tree);

    // Calculate illumination at a point considering cluster shadows
    float calculate_point_illumination(
        const glm::vec3& point,
        const TreeStructure& tree,
        const glm::vec3& light_dir);

    // Apply environmental bending (phototropism + gravitropism + obstacle
    // avoidance)
    void apply_environmental_bending(TreeStructure& tree);

    // Calculate inverse tropism - determine original direction from bent shape
    glm::vec3 calculate_inverse_tropism(
        const glm::vec3& bent_direction,
        const glm::vec3& position,
        float branch_length,
        float illumination);

    // Apply plasticity transformations to adapt to environment
    void apply_plasticity_transform(TreeStructure& tree);

    // ===== Utility Functions =====

    // Calculate distance along branch path between two buds
    float calculate_branch_distance(const TreeBud& bud1, const TreeBud& bud2);

    // Rotate vector around axis
    glm::vec3
    rotate_vector(const glm::vec3& vec, const glm::vec3& axis, float angle);

    // Get perpendicular vector
    glm::vec3 get_perpendicular(const glm::vec3& vec);
};

}  // namespace TreeGen
