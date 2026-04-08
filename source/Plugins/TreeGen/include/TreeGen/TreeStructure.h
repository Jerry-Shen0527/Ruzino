#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace TreeGen {

// Forward declarations
struct TreeBud;
struct TreeBranch;
struct TreeLeaf;

// Bud type
enum class BudType {
    Apical,  // Terminal bud
    Lateral  // Side bud
};

// Bud state
enum class BudState {
    Active,   // Growing
    Dormant,  // Not growing but alive
    Dead      // Dead
};

// Tree bud structure
struct TreeBud {
    BudType type = BudType::Apical;
    BudState state = BudState::Active;

    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 direction = glm::vec3(0.0f, 1.0f, 0.0f);

    float illumination = 1.0f;  // Light received
    float auxin_level = 0.0f;   // Hormone concentration

    int age = 0;    // Age in growth cycles
    int level = 0;  // Branch level (0=trunk)

    TreeBranch* parent_branch = nullptr;
};

// Tree leaf structure
struct TreeLeaf {
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 normal =
        glm::vec3(0.0f, 1.0f, 0.0f);  // Points out of leaf surface
    glm::vec3 tangent = glm::vec3(1.0f, 0.0f, 0.0f);   // Along leaf length
    glm::vec3 binormal = glm::vec3(0.0f, 0.0f, 1.0f);  // Along leaf width

    float size = 1.0f;         // Overall scale
    float width = 1.0f;        // Width factor
    float length = 1.0f;       // Length factor
    float rotation = 0.0f;     // Rotation around normal (radians)
    float inclination = 0.0f;  // Angle from horizontal (radians)
    float curvature = 0.0f;    // Bending factor

    int age = 0;           // Age in growth cycles
    int parent_level = 0;  // Level of parent branch

    // Attachment point direction (for proper orientation)
    glm::vec3 attachment_direction = glm::vec3(0.0f, 1.0f, 0.0f);

    TreeBranch* parent_branch = nullptr;
};

// Tree branch structure (internode)
struct TreeBranch {
    glm::vec3 start_position = glm::vec3(0.0f);
    glm::vec3 end_position = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 direction = glm::vec3(0.0f, 1.0f, 0.0f);

    float length = 1.0f;
    float radius = 0.05f;

    int level = 0;  // Branch level
    int age = 0;    // Age in growth cycles

    TreeBranch* parent = nullptr;
    std::vector<std::shared_ptr<TreeBranch>> children;
    std::vector<std::shared_ptr<TreeBud>> lateral_buds;
    std::vector<std::shared_ptr<TreeLeaf>> leaves;
    std::shared_ptr<TreeBud> apical_bud;

    // For structural bending
    float accumulated_weight = 0.0f;

    // Plastic Trees specific - environmental adaptation
    glm::vec3 original_direction =
        glm::vec3(0.0f, 1.0f, 0.0f);    // Direction without tropisms
    float illumination = 1.0f;          // Current light level
    float environmental_stress = 0.0f;  // Accumulated environmental stress
    bool is_pruned = false;             // Whether branch is marked for removal

    // Check if this is a terminal branch (has no children or only young
    // children)
    bool is_terminal() const
    {
        if (children.empty())
            return true;
        // A branch is terminal if all children are very young
        for (const auto& child : children) {
            if (child->age > 1)
                return false;
        }
        return true;
    }

    // Get the maximum depth from this branch
    int max_depth() const
    {
        if (children.empty())
            return 0;
        int max_d = 0;
        for (const auto& child : children) {
            max_d = std::max(max_d, child->max_depth());
        }
        return max_d + 1;
    }
};

// Leaf cluster for efficient light computation (Plastic Trees)
struct LeafCluster {
    glm::vec3 center = glm::vec3(0.0f);
    float radius = 0.5f;
    float translucency = 0.5f;  // Base translucency for light transmission
    std::vector<std::shared_ptr<TreeLeaf>> leaves;  // Leaves in this cluster
    TreeBranch* parent_branch = nullptr;
};

// Tree structure - root container
struct TreeStructure {
    std::shared_ptr<TreeBranch> root;
    std::vector<std::shared_ptr<TreeBranch>> all_branches;
    std::vector<std::shared_ptr<TreeBud>> all_buds;
    std::vector<std::shared_ptr<TreeLeaf>> all_leaves;
    std::vector<std::shared_ptr<LeafCluster>>
        leaf_clusters;  // For Plastic Trees illumination

    int current_age = 0;

    // Environmental state
    glm::vec3 light_direction =
        glm::vec3(0.0f, 1.0f, 0.0f);   // Primary light direction
    std::vector<glm::vec3> obstacles;  // Obstacle positions for avoidance

    // Helper to collect all branches recursively
    void collect_branches(std::shared_ptr<TreeBranch> branch)
    {
        if (!branch)
            return;
        all_branches.push_back(branch);
        for (auto& child : branch->children) {
            collect_branches(child);
        }
    }

    // Helper to collect all active buds
    void collect_active_buds()
    {
        all_buds.clear();
        for (auto& branch : all_branches) {
            if (branch->apical_bud &&
                branch->apical_bud->state == BudState::Active) {
                all_buds.push_back(branch->apical_bud);
            }
            for (auto& bud : branch->lateral_buds) {
                if (bud->state == BudState::Active) {
                    all_buds.push_back(bud);
                }
            }
        }
    }

    // Helper to collect all leaves
    void collect_all_leaves()
    {
        all_leaves.clear();
        for (auto& branch : all_branches) {
            for (auto& leaf : branch->leaves) {
                all_leaves.push_back(leaf);
            }
        }
    }
};

}  // namespace TreeGen
