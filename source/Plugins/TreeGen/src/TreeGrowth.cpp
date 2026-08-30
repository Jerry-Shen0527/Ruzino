#include "TreeGen/TreeGrowth.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/rotate_vector.hpp>
#include <unordered_map>
#include <unordered_set>

namespace TreeGen {

namespace {
constexpr float kPi = 3.14159265358979f;
constexpr float kGoldenAngle = 137.5077640500379f;
}  // namespace

TreeGrowth::TreeGrowth(const TreeParameters& params)
    : params_(params),
      normal_dist_(0.0f, 1.0f),
      uniform_dist_(0.0f, 1.0f)
{
    init_random();
}

void TreeGrowth::init_random()
{
    rng_.seed(params_.random_seed);
}

float TreeGrowth::random_normal(float mean, float stddev)
{
    return mean + stddev * normal_dist_(rng_);
}

float TreeGrowth::random_uniform(float min, float max)
{
    return min + (max - min) * uniform_dist_(rng_);
}

TreeStructure TreeGrowth::initialize_tree()
{
    TreeStructure tree;
    tree.current_age = 0;

    // Create root branch (initial trunk segment)
    auto root = std::make_shared<TreeBranch>();
    root->start_position = glm::vec3(0.0f, 0.0f, 0.0f);
    root->end_position = glm::vec3(0.0f, params_.internode_base_length, 0.0f);
    root->direction = glm::normalize(root->end_position - root->start_position);
    root->length = params_.internode_base_length;
    root->radius = params_.initial_radius;
    root->level = 0;
    root->age = 0;
    root->parent = nullptr;

    // Create apical bud at the top node of the root internode
    auto apical_bud = std::make_shared<TreeBud>();
    apical_bud->type = BudType::Apical;
    apical_bud->state = BudState::Active;
    apical_bud->position = root->end_position;
    apical_bud->direction = root->direction;
    apical_bud->level = 0;
    apical_bud->age = 0;
    apical_bud->illumination = 1.0f;
    apical_bud->along_branch = root->length;
    apical_bud->parent_branch = root.get();

    root->apical_bud = apical_bud;

    tree.root = root;
    tree.all_branches.push_back(root);
    tree.all_buds.push_back(apical_bud);

    return tree;
}

void TreeGrowth::grow_tree(TreeStructure& tree, int cycles)
{
    for (int i = 0; i < cycles; ++i) {
        grow_one_cycle(tree);
        tree.current_age++;
    }
}

void TreeGrowth::grow_one_cycle(TreeStructure& tree)
{
    // Step 1: bud extinction (phi_ABD / phi_LBD) and aging
    update_bud_states(tree);

    // Step 2: illumination for every living bud and branch (Pirk et al. 2012
    // style cluster shadows when plasticity is on, height model otherwise)
    if (params_.enable_plasticity) {
        create_leaf_clusters(tree);
        calculate_illumination_with_clusters(tree);
    } else {
        calculate_illumination(tree);
    }

    // Step 3: auxin received by lateral buds from last cycle's flushing buds
    calculate_auxin_levels(tree);

    // Step 4: decide which buds flush (Eqs. 2 and 3)
    determine_bud_flushing(tree);

    // Step 5: grow shoots from flushing buds
    std::vector<std::shared_ptr<TreeBud>> flushing_buds;
    for (auto& bud : tree.all_buds) {
        bud->flushed_this_cycle = false;
        if (bud->state == BudState::Active && !bud->has_ever_flushed) {
            flushing_buds.push_back(bud);
        }
    }
    for (auto& bud : flushing_buds) {
        grow_shoot_from_bud(tree, bud);
    }

    // Step 6: pipe-model radii, then gravity bending (Appendix A)
    update_branch_radii(tree);
    apply_structural_bending(tree);

    // Step 7: shedding — remove low and shadowed branches for real
    prune_branches(tree);

    // Step 8: rebuild flat lists (living buds only, Dormant included)
    tree.all_branches.clear();
    tree.collect_branches(tree.root);
    tree.collect_living_buds();
    tree.collect_all_leaves();
}

void TreeGrowth::update_bud_states(TreeStructure& tree)
{
    for (auto& bud : tree.all_buds) {
        if (bud->state == BudState::Dead)
            continue;

        float death_prob = (bud->type == BudType::Apical)
                               ? params_.apical_bud_death
                               : params_.lateral_bud_death;

        if (random_uniform() < death_prob) {
            bud->state = BudState::Dead;
        }

        bud->age++;
    }
}

void TreeGrowth::calculate_illumination(TreeStructure& tree)
{
    // Height-based approximation used when the cluster light model is off
    float max_height = 0.0f;
    for (auto& b : tree.all_buds) {
        if (b->state == BudState::Dead)
            continue;
        max_height = std::max(max_height, b->position.y);
    }

    for (auto& bud : tree.all_buds) {
        if (bud->state == BudState::Dead)
            continue;

        if (max_height > 1e-5f) {
            bud->illumination =
                0.3f + 0.7f * std::max(0.0f, bud->position.y) / max_height;
        } else {
            bud->illumination = 1.0f;
        }
    }
}

float TreeGrowth::compute_auxin_for_bud(const TreeBud& bud,
                                        const TreeStructure& tree)
{
    (void)tree;
    // Sum over buds that flushed during the previous cycle (delta_j = 1),
    // located ABOVE the given lateral bud, weighted by the branch-wise
    // distance d(b_i, b_j):
    //   auxin += phi_ADBF * phi_ADAF^t * phi_ADDF^d
    // "Above" means: buds further out on the same branch, plus the apical
    // buds of every ancestor branch (children attach at the parent's end,
    // so only the ancestor's apical bud lies above the fork point).
    float auxin = 0.0f;
    float age_factor = std::pow(params_.apical_dominance_age,
                                static_cast<float>(tree.current_age));
    float addf = glm::clamp(params_.apical_dominance_distance, 1e-3f, 0.999f);

    const TreeBranch* branch = bud.parent_branch;
    float fork_along = bud.along_branch;
    float path_length = 0.0f;

    int guard = 0;
    while (branch && guard++ < 256) {
        for (const auto& other : branch->lateral_buds) {
            if (other->state == BudState::Dead || !other->flushed_this_cycle)
                continue;
            if (other->along_branch <= fork_along + 1e-4f)
                continue;
            float d = path_length + (other->along_branch - fork_along);
            auxin += params_.apical_dominance_base * age_factor *
                     std::pow(addf, d);
        }
        const auto& apical = branch->apical_bud;
        if (apical && apical->flushed_this_cycle &&
            apical.get() != &bud && branch->length > fork_along - 1e-4f) {
            float d = path_length +
                      std::max(0.0f, branch->length - fork_along);
            auxin +=
                params_.apical_dominance_base * age_factor * std::pow(addf, d);
        }

        path_length += std::max(0.0f, branch->length - fork_along);
        fork_along = branch->length;
        branch = branch->parent;
    }

    return auxin;
}

void TreeGrowth::calculate_auxin_levels(TreeStructure& tree)
{
    for (auto& bud : tree.all_buds) {
        if (bud->type != BudType::Lateral)
            continue;
        if (bud->state == BudState::Dead)
            continue;
        bud->auxin_level = compute_auxin_for_bud(*bud, tree);
    }
}

void TreeGrowth::determine_bud_flushing(TreeStructure& tree)
{
    for (auto& bud : tree.all_buds) {
        if (bud->state == BudState::Dead)
            continue;
        if (bud->has_ever_flushed)
            continue;  // a bud that became a shoot never flushes again

        float flush_prob =
            (bud->type == BudType::Apical)
                ? std::pow(glm::clamp(bud->illumination, 0.0f, 1.0f),
                           params_.apical_light_factor)
                : std::pow(glm::clamp(bud->illumination, 0.0f, 1.0f),
                           params_.lateral_light_factor) *
                      std::exp(-bud->auxin_level);

        if (random_uniform() < flush_prob) {
            bud->state = BudState::Active;  // will grow in step 5
        } else {
            bud->state = BudState::Dormant;  // stays in the pool
        }
    }
}

float TreeGrowth::calculate_growth_rate(int branch_level, int tree_age)
{
    // Eq. 1: phi'_GR = phi_GR / phi_AC^zeta_k          (phi_AC > 1)
    //              = phi_GR / phi_AC^(zeta_k - zeta_max) otherwise
    // where zeta_k is the level of the parent branch and phi_AC decays with
    // tree age via the apical control age factor.
    float ac = params_.apical_control *
               std::pow(params_.apical_control_age_factor,
                        static_cast<float>(tree_age));

    if (ac <= 1e-3f)
        return params_.growth_rate;

    if (ac > 1.0f) {
        return params_.growth_rate /
               std::pow(ac, static_cast<float>(std::max(0, branch_level)));
    }

    int zeta_max = std::max(1, branch_level);
    return params_.growth_rate /
           std::pow(ac, static_cast<float>(branch_level - zeta_max));
}

float TreeGrowth::calculate_internode_length(int tree_age)
{
    return params_.internode_base_length *
           std::pow(params_.internode_length_age_factor,
                    static_cast<float>(tree_age));
}

void TreeGrowth::grow_shoot_from_bud(TreeStructure& tree,
                                     std::shared_ptr<TreeBud> bud)
{
    if (bud->state != BudState::Active || bud->has_ever_flushed)
        return;

    // Eq. 1 uses zeta_k = the level of the PARENT branch (the branch the bud
    // sits on), not the shoot level the bud will create
    int parent_level =
        bud->parent_branch ? bud->parent_branch->level : bud->level;
    int num_internodes = static_cast<int>(
        std::round(calculate_growth_rate(parent_level, tree.current_age)));
    if (num_internodes < 1) {
        bud->state = BudState::Dormant;
        return;
    }

    // Find the parent branch object for the new shoot chain
    std::shared_ptr<TreeBranch> parent_branch = nullptr;
    if (bud->parent_branch) {
        for (auto& branch : tree.all_branches) {
            if (branch.get() == bud->parent_branch) {
                parent_branch = branch;
                break;
            }
        }
    }

    create_internodes(tree,
                      parent_branch,
                      bud->position,
                      bud->direction,
                      num_internodes,
                      bud->level,
                      bud->illumination,
                      bud->light_direction);

    // The bud became a shoot: it is spent and produces auxin next cycle
    bud->has_ever_flushed = true;
    bud->flushed_this_cycle = true;
    bud->state = BudState::Dormant;
}

void TreeGrowth::create_internodes(
    TreeStructure& tree,
    std::shared_ptr<TreeBranch> parent,
    const glm::vec3& start_pos,
    const glm::vec3& initial_dir,
    int num_internodes,
    int branch_level,
    float bud_illumination,
    const glm::vec3& bud_light_dir)
{
    glm::vec3 current_pos = start_pos;
    glm::vec3 current_dir = glm::normalize(initial_dir);
    std::shared_ptr<TreeBranch> current_parent = parent;

    float internode_len = calculate_internode_length(tree.current_age);

    for (int i = 0; i < num_internodes; ++i) {
        // Tropism bends EVERY internode (Palubicki et al. 2009 approach),
        // not just the first one of the shoot
        current_dir = apply_tropism(current_dir, bud_illumination, bud_light_dir);
        current_dir = glm::normalize(current_dir);

        auto branch = std::make_shared<TreeBranch>();
        branch->start_position = current_pos;
        branch->direction = current_dir;
        branch->length = internode_len;
        branch->end_position = current_pos + current_dir * internode_len;
        branch->level = branch_level;
        branch->age = 0;
        branch->parent = current_parent.get();
        branch->radius = params_.initial_radius;

        if (current_parent) {
            current_parent->children.push_back(branch);
        }

        // Lateral buds at the node on TOP of this internode
        create_lateral_buds(branch, i);

        if (i == num_internodes - 1) {
            // Only the shoot TIP carries a flushable apical bud — one shoot,
            // one apical bud. Interior nodes get none (the old code created a
            // flushable bud per internode, so every node of the trunk
            // sprouted its own shoot and the tree collapsed into a thicket).
            glm::vec3 apical_dir = apply_apical_spherical_angle(current_dir);

            auto apical_bud = std::make_shared<TreeBud>();
            apical_bud->type = BudType::Apical;
            apical_bud->state = BudState::Active;
            apical_bud->position = branch->end_position;
            apical_bud->direction = apical_dir;
            apical_bud->level = branch_level;
            apical_bud->age = 0;
            apical_bud->illumination = bud_illumination;
            apical_bud->light_direction = bud_light_dir;
            apical_bud->along_branch = branch->length;
            apical_bud->parent_branch = branch.get();
            branch->apical_bud = apical_bud;

            current_dir = apical_dir;
        }

        current_pos = branch->end_position;
        current_parent = branch;

        // Safety: a shoot without a parent becomes its own root
        if (i == 0 && !parent) {
            tree.root = branch;
        }
    }
}

glm::vec3 TreeGrowth::apply_tropism(const glm::vec3& direction,
                                    float illumination,
                                    const glm::vec3& local_light_dir)
{
    // Phototropism bends towards the bud's LOCAL sky-opening direction
    // (scaled by how much light the source bud sees), gravitropism bends
    // away from gravity
    glm::vec3 dir = direction;
    glm::vec3 light_dir = (glm::length(local_light_dir) > 1e-5f)
                              ? glm::normalize(local_light_dir)
                              : glm::normalize(params_.light_direction);
    glm::vec3 gravity_dir = glm::normalize(params_.gravity_direction);

    dir += light_dir * params_.phototropism *
           (params_.enable_plasticity ? glm::clamp(illumination, 0.2f, 1.0f)
                                      : 1.0f);
    dir -= gravity_dir * params_.gravitropism;

    return dir;
}

glm::vec3 TreeGrowth::apply_apical_spherical_angle(const glm::vec3& direction)
{
    float theta = random_normal(0.0f, glm::radians(params_.apical_angle_variance));
    float phi = random_uniform(0.0f, 2.0f * kPi);

    // Random azimuth picks the tilt plane, theta tilts within it (the old
    // code rotated the vector around itself for phi, a no-op that biased
    // all apical buds into one deterministic plane)
    glm::vec3 dir = glm::normalize(direction);
    glm::vec3 perp = glm::rotate(get_perpendicular(dir), phi, dir);
    return glm::normalize(glm::rotate(dir, theta, perp));
}

void TreeGrowth::create_lateral_buds(std::shared_ptr<TreeBranch> branch,
                                     int node_index)
{
    int num_buds = params_.num_lateral_buds;

    for (int i = 0; i < num_buds; ++i) {
        auto lateral_bud = std::make_shared<TreeBud>();
        lateral_bud->type = BudType::Lateral;
        lateral_bud->state = BudState::Active;

        // Buds sit at the node on top of the internode
        lateral_bud->position = branch->end_position;
        lateral_bud->along_branch = branch->length;

        lateral_bud->direction = calculate_lateral_direction(
            branch->direction, i, num_buds, node_index);

        lateral_bud->level = branch->level + 1;
        lateral_bud->age = 0;
        lateral_bud->illumination = branch->illumination;
        lateral_bud->auxin_level = 0.0f;
        lateral_bud->parent_branch = branch.get();

        branch->lateral_buds.push_back(lateral_bud);
    }
}

glm::vec3 TreeGrowth::calculate_lateral_direction(const glm::vec3& parent_dir,
                                                  int bud_index,
                                                  int total_buds,
                                                  int node_index)
{
    // Roll: per-node increment phi_RAM (orientation difference between
    // successive internodes) + even spacing around the node + variance
    float roll = glm::radians(params_.roll_angle_mean) *
                     static_cast<float>(node_index + 1) +
                 static_cast<float>(bud_index) * (2.0f * kPi /
                                                  static_cast<float>(total_buds)) +
                 glm::radians(random_normal(0.0f, params_.roll_angle_variance));

    // Branching angle from the parent shoot direction
    float branch_angle = glm::radians(glm::clamp(
        random_normal(params_.branching_angle_mean,
                      params_.branching_angle_variance),
        1.0f, 179.0f));

    glm::vec3 dir = glm::normalize(parent_dir);
    glm::vec3 radial = glm::rotate(get_perpendicular(dir), roll, dir);
    glm::vec3 axis = glm::normalize(glm::cross(dir, radial));
    return glm::normalize(glm::rotate(dir, branch_angle, axis));
}

void TreeGrowth::update_branch_radii(TreeStructure& tree)
{
    // Pipe model, tips-to-root: r_parent^2 = sum(r_child^2); terminal
    // internodes keep the base tip radius
    std::function<void(std::shared_ptr<TreeBranch>)> update_radius;
    update_radius = [&](std::shared_ptr<TreeBranch> branch) {
        if (!branch)
            return;

        for (auto& child : branch->children) {
            update_radius(child);
        }

        if (!branch->children.empty()) {
            float child_area_sum = 0.0f;
            for (auto& child : branch->children) {
                child_area_sum += child->radius * child->radius;
            }
            branch->radius = std::sqrt(child_area_sum);
        } else {
            branch->radius = params_.initial_radius;
        }

        branch->radius = std::max(branch->radius, 1e-3f);
    };

    if (tree.root) {
        update_radius(tree.root);
    }
}

void TreeGrowth::apply_structural_bending(TreeStructure& tree)
{
    // Appendix A of Stava et al. 2014. At each node p0 with orientation h:
    //   f_b      = phi_GBS * m_c * |(c - p0) . h_H| * (1 - |h . g|)
    //   beta_max = (pi/2) * phi_GBA^d
    //   beta     = max(0, beta_max * exp(-|f_b| / beta_max) - beta^(t-1))
    //   beta^(t) = beta^(t-1) + beta
    // m_c is the supported subtree mass with centroid c, h_H the horizontal
    // part of h, d the branch thickness. The branch direction rotates
    // towards gravity by beta around the h x g axis.
    if (params_.gravity_bending_strength <= 0.0f ||
        params_.gravity_bending_angle <= 0.0f || !tree.root) {
        return;
    }

    glm::vec3 gravity_dir = glm::normalize(params_.gravity_direction);

    // Mass in units of "one tip internode of wood" so phi_GBS is scale-free
    // (the paper's m_c is a physical mass; without normalization the raw
    // len*r^2 proxy is vanishingly small for thin twigs and the bending
    // never engages)
    const float m_ref = std::max(
        params_.internode_base_length * params_.initial_radius *
            params_.initial_radius,
        1e-10f);
    // Softening constant for the saturating force->angle response
    const float kBetaSoftening = 10.0f;

    // Post-order pass: subtree mass (len * r^2 proxy) and its centroid
    std::function<float(const std::shared_ptr<TreeBranch>&, glm::vec3&)>
        subtree_mass = [&](const std::shared_ptr<TreeBranch>& branch,
                           glm::vec3& centroid) -> float {
        if (!branch)
            return 0.0f;

        float own_mass = branch->length * branch->radius * branch->radius;
        glm::vec3 own_center =
            (branch->start_position + branch->end_position) * 0.5f;

        float total = own_mass;
        glm::vec3 weighted = own_center * own_mass;
        for (auto& child : branch->children) {
            glm::vec3 child_centroid(0.0f);
            float child_mass = subtree_mass(child, child_centroid);
            total += child_mass;
            weighted += child_centroid * child_mass;
        }

        centroid = (total > 1e-9f) ? weighted / total : own_center;
        return total;
    };

    // Pre-order pass: rotate each branch, hand the new end position down
    std::function<void(const std::shared_ptr<TreeBranch>&, const glm::vec3&)>
        bend = [&](const std::shared_ptr<TreeBranch>& branch,
                   const glm::vec3& start) {
            if (!branch)
                return;

            branch->start_position = start;
            glm::vec3 h = glm::normalize(branch->direction);
            glm::vec3 centroid(0.0f);
            float mass = subtree_mass(branch, centroid);

            glm::vec3 h_horizontal(h.x, 0.0f, h.z);
            float h_h_len = glm::length(h_horizontal);
            float verticality = 1.0f - std::abs(glm::dot(h, gravity_dir));

            // Level-0 (trunk chain) segments are exempt: once the trunk
            // tilts even slightly the bending force feeds back on itself
            // (tilt -> verticality grows -> more torque) and kinks the trunk
            if (branch->level > 0 && mass > 1e-9f && h_h_len > 1e-4f &&
                verticality > 1e-3f) {
                float arm = std::abs(glm::dot(centroid - branch->start_position,
                                              h_horizontal / h_h_len));
                float f_b = params_.gravity_bending_strength * (mass / m_ref) *
                            arm * verticality;

                float beta_max =
                    (kPi * 0.5f) *
                    std::pow(glm::clamp(params_.gravity_bending_angle,
                                        1e-3f, 0.999f),
                             branch->radius);
                // Force-driven saturation: zero force bends nothing, force
                // asymptotically approaches beta_max. (The paper's literal
                // form beta_max*exp(-|f_b|/beta_max) is maximal at ZERO
                // force — clearly inverted — so we use the saturating form
                // that preserves the documented semantics: force bends the
                // branch, thickness caps the angle, accumulation hardens it.)
                float beta_target =
                    beta_max *
                    (1.0f - std::exp(-f_b /
                                     (beta_max * kBetaSoftening)));
                float beta = std::max(0.0f, beta_target -
                                                branch->accumulated_bending);
                branch->accumulated_bending += beta;
                branch->accumulated_bending =
                    std::min(branch->accumulated_bending, kPi * 0.5f);

                if (beta > 1e-5f) {
                    glm::vec3 axis =
                        glm::normalize(glm::cross(h, gravity_dir));
                    if (glm::length(axis) > 1e-4f) {
                        h = glm::rotate(h, beta, axis);
                    }
                }
            }

            branch->direction = h;
            branch->end_position =
                branch->start_position + h * branch->length;

            for (auto& child : branch->children) {
                bend(child, branch->end_position);
            }
        };

    bend(tree.root, tree.root->start_position);
}

void TreeGrowth::prune_branches(TreeStructure& tree)
{
    // Shedding (phi_PF): branches receiving less light than the pruning
    // factor are shed. Low-branch pruning (phi_LBPF): all LATERAL branches
    // below this height are shed — the trunk (level 0) is never removed.
    std::unordered_set<TreeBranch*> removed;
    std::function<void(std::shared_ptr<TreeBranch>)> mark;
    mark = [&](std::shared_ptr<TreeBranch> branch) {
        if (!branch || branch->is_pruned)
            return;

        bool shed = false;
        if (branch->level > 0) {
            if (branch->start_position.y < params_.low_branch_pruning_factor) {
                shed = true;
            } else if (branch->illumination < params_.pruning_factor &&
                       params_.enable_plasticity) {
                shed = true;
            }
        }

        if (shed) {
            removed.insert(branch.get());
            branch->is_pruned = true;
            // Descendants die with their parent
            std::function<void(std::shared_ptr<TreeBranch>)> mark_subtree =
                [&](std::shared_ptr<TreeBranch> node) {
                    if (!node)
                        return;
                    removed.insert(node.get());
                    node->is_pruned = true;
                    for (auto& child : node->children)
                        mark_subtree(child);
                };
            for (auto& child : branch->children)
                mark_subtree(child);
            return;
        }

        for (auto& child : branch->children)
            mark(child);
    };
    if (tree.root) {
        mark(tree.root);
    }

    if (removed.empty())
        return;

    // Detach removed branches from their (surviving) parents
    std::function<void(std::shared_ptr<TreeBranch>)> detach;
    detach = [&](std::shared_ptr<TreeBranch> branch) {
        if (!branch)
            return;
        if (!branch->children.empty()) {
            std::vector<std::shared_ptr<TreeBranch>> kept;
            kept.reserve(branch->children.size());
            for (auto& child : branch->children) {
                if (removed.count(child.get()))
                    continue;
                kept.push_back(child);
            }
            branch->children = std::move(kept);
        }
        for (auto& child : branch->children)
            detach(child);
    };
    if (tree.root) {
        detach(tree.root);
    }
}

void TreeGrowth::generate_foliage(TreeStructure& tree)
{
    // Section 4.2: foliage is generated procedurally AT THE END of the
    // growth cycle, located along the terminal branches. A "terminal branch"
    // is the whole outermost unbranched shoot chain: from each branch with
    // no children we walk up through single-child ancestors (up to 3
    // internodes) and distribute leaves along that span, so bare twigs don't
    // end in an isolated tuft.
    if (!params_.generate_leaves)
        return;

    tree.all_leaves.clear();

    glm::vec3 light_dir = glm::normalize(params_.light_direction);
    glm::vec3 up(0.0f, 1.0f, 0.0f);

    std::function<void(std::shared_ptr<TreeBranch>)> place;
    place = [&](std::shared_ptr<TreeBranch> branch) {
        if (!branch)
            return;

        bool terminal = branch->children.empty();
        bool eligible =
            terminal ? (branch->level >= 0)
                     : (!params_.leaves_on_terminal_only &&
                        branch->level >= params_.min_leaf_level);

        if (eligible && branch->length > 1e-4f) {
            // Collect the terminal chain: this branch plus up to five
            // ancestors (root-side first). Forks do NOT interrupt the chain:
            // the parent limb continues through them, and its outer
            // internodes carry foliage in real trees too. Stop before the
            // trunk (level 0).
            std::vector<std::shared_ptr<TreeBranch>> chain;
            chain.push_back(branch);
            TreeBranch* node = branch->parent;
            while (node && chain.size() < 6 && node->level > 0) {
                std::shared_ptr<TreeBranch> node_shared;
                if (node->parent) {
                    for (auto& c : node->parent->children) {
                        if (c.get() == node) {
                            node_shared = c;
                            break;
                        }
                    }
                } else if (tree.root.get() == node) {
                    node_shared = tree.root;
                }
                if (!node_shared)
                    break;
                chain.push_back(node_shared);
                node = node->parent;
            }
            std::reverse(chain.begin(), chain.end());

            float chain_length = 0.0f;
            for (auto& c : chain)
                chain_length += c->length;

            // Leaves along the whole chain span
            float internodes = std::max(
                1.0f,
                chain_length / std::max(0.05f, params_.internode_base_length));
            int num_leaves = static_cast<int>(std::lround(
                internodes * static_cast<float>(params_.leaves_per_internode)));
            num_leaves = glm::clamp(num_leaves, 1, 48);

            // Leaf-branch tilt: 0 deg = along the branch, 90 deg = straight
            // out; driven by the inclination parameter
            float tilt = glm::radians(glm::clamp(
                random_normal(params_.leaf_inclination_mean,
                              params_.leaf_inclination_variance),
                5.0f, 85.0f));

            float phyl_offset = random_uniform(0.0f, 360.0f);

            // Distribute leaves over the OUTER ~3 internodes of the chain
            // only — the inner span runs through the crown interior where
            // foliage would leave bare tufted tips sticking out
            float outer_span = std::min(
                chain_length, 3.0f * std::max(0.05f,
                                              params_.internode_base_length));
            float s_start = chain_length - outer_span * 0.94f;
            float s_end = chain_length - outer_span * 0.06f;

            for (int i = 0; i < num_leaves; ++i) {
                auto leaf = std::make_shared<TreeLeaf>();

                // Arc-length position along the chain polyline
                float s = s_start +
                          (s_end - s_start) *
                              ((static_cast<float>(i) + 0.5f) /
                               static_cast<float>(num_leaves));
                std::shared_ptr<TreeBranch> seg;
                for (auto& c : chain) {
                    if (s <= c->length || c.get() == chain.back().get()) {
                        seg = c;
                        break;
                    }
                    s -= c->length;
                }
                glm::vec3 dir = glm::normalize(seg->direction);
                leaf->position = seg->start_position + dir * std::min(s, seg->length);

                float phyl = glm::radians(phyl_offset + kGoldenAngle *
                                                             static_cast<float>(i));
                glm::vec3 radial =
                    glm::rotate(get_perpendicular(dir), phyl, dir);

                // Leaf length axis: out of the branch at the tilt angle
                glm::vec3 tangent = glm::normalize(dir * std::cos(tilt) +
                                                   radial * std::sin(tilt));

                // Leaf plane normal: outward, biased up and towards light
                glm::vec3 normal = glm::normalize(
                    radial * 0.7f + up * 0.45f +
                    light_dir * params_.leaf_phototropism * 0.35f);
                glm::vec3 binormal =
                    glm::normalize(glm::cross(normal, tangent));
                normal = glm::cross(tangent, binormal);

                // Keep the plane facing the light
                if (glm::dot(normal, light_dir) < 0.0f) {
                    normal = -normal;
                    binormal = -binormal;
                }

                leaf->tangent = tangent;
                leaf->normal = normal;
                leaf->binormal = binormal;

                leaf->size = std::max(
                    0.02f,
                    random_normal(params_.leaf_size_base,
                                  params_.leaf_size_variance));
                leaf->length = leaf->size * params_.leaf_aspect_ratio;
                leaf->width = leaf->size;

                // Residual per-leaf tilt around the binormal (radians)
                leaf->inclination =
                    glm::radians(random_normal(0.0f, 8.0f));
                leaf->rotation =
                    glm::radians(random_normal(
                        0.0f, params_.leaf_rotation_variance));
                leaf->curvature = glm::clamp(
                    random_normal(params_.leaf_curvature, 0.1f), 0.0f, 1.0f);

                leaf->age = 0;
                leaf->parent_level = branch->level;
                leaf->parent_branch = branch.get();
                leaf->attachment_direction = dir;

                branch->leaves.push_back(leaf);
            }
        }

        for (auto& child : branch->children)
            place(child);
    };

    if (tree.root) {
        place(tree.root);
    }

    tree.collect_all_leaves();
}

// ===== Illumination (Pirk et al. 2012 style) =====

void TreeGrowth::create_leaf_clusters(TreeStructure& tree)
{
    // Proxy foliage during growth: one translucent cluster per terminal
    // branch. Outer shoots shade the inner crown, which drives bud flushing
    // and shedding towards the paper's self-organizing behaviour.
    tree.leaf_clusters.clear();

    std::function<void(std::shared_ptr<TreeBranch>)> collect;
    collect = [&](std::shared_ptr<TreeBranch> branch) {
        if (!branch)
            return;
        if (branch->children.empty()) {
            auto cluster = std::make_shared<LeafCluster>();
            cluster->center = (branch->start_position +
                               branch->end_position) * 0.5f;
            cluster->radius = params_.leaf_cluster_radius +
                              branch->length * 0.5f;
            cluster->translucency =
                glm::clamp(params_.cluster_translucency, 0.05f, 1.0f);
            cluster->parent_branch = branch.get();
            tree.leaf_clusters.push_back(cluster);
            return;
        }
        for (auto& child : branch->children)
            collect(child);
    };

    if (tree.root) {
        collect(tree.root);
    }
}

void TreeGrowth::calculate_illumination_with_clusters(TreeStructure& tree)
{
    if (tree.leaf_clusters.empty()) {
        calculate_illumination(tree);
        return;
    }

    glm::vec3 light_dir = glm::normalize(params_.light_direction);

    for (auto& bud : tree.all_buds) {
        if (bud->state == BudState::Dead)
            continue;
        glm::vec3 local_light(0.0f, 1.0f, 0.0f);
        bud->illumination = calculate_point_illumination(
            bud->position, tree, light_dir, &local_light);
        bud->light_direction = local_light;
    }

    for (auto& branch : tree.all_branches) {
        glm::vec3 mid_pos =
            (branch->start_position + branch->end_position) * 0.5f;
        branch->illumination =
            calculate_point_illumination(mid_pos, tree, light_dir);
    }
}

float TreeGrowth::calculate_point_illumination(
    const glm::vec3& point,
    const TreeStructure& tree,
    const glm::vec3& light_dir,
    glm::vec3* out_light_direction)
{
    // Hemisphere sampling with cluster transmittance (Eq. 3 of Pirk et al.
    // 2012, simplified). The sky is a uniform dome (overcast-sky assumption,
    // as in PHL*09): every upward sample carries equal weight, so a bud's
    // illumination is its mean sky visibility and the visibility-weighted
    // average direction points at the local sky opening — outward for
    // crown-edge buds, which is what makes phototropism spread the crown.
    const int num_samples = 16;
    float total_illumination = 0.0f;
    glm::vec3 weighted_direction(0.0f);

    for (int i = 0; i < num_samples; ++i) {
        float theta =
            (i / static_cast<float>(num_samples)) * kPi * 0.5f;
        float phi = (i * kGoldenAngle);

        glm::vec3 sample_dir(std::sin(theta) * std::cos(phi),
                             std::cos(theta),
                             std::sin(theta) * std::sin(phi));

        glm::vec3 up(0.0f, 1.0f, 0.0f);
        if (std::abs(glm::dot(light_dir, up)) > 0.99f) {
            up = glm::vec3(1.0f, 0.0f, 0.0f);
        }
        glm::vec3 right = glm::normalize(glm::cross(up, light_dir));
        glm::vec3 forward = glm::normalize(glm::cross(right, light_dir));

        glm::vec3 direction = sample_dir.x * right + sample_dir.y * light_dir +
                              sample_dir.z * forward;
        direction = glm::normalize(direction);

        float visibility = 1.0f;
        for (const auto& cluster : tree.leaf_clusters) {
            // Skip the cluster the point sits inside (no self-shading)
            if (glm::length(cluster->center - point) < cluster->radius)
                continue;

            glm::vec3 to_cluster = cluster->center - point;
            float proj = glm::dot(to_cluster, direction);
            if (proj <= 0.0f)
                continue;

            glm::vec3 closest = point + direction * proj;
            float dist = glm::length(closest - cluster->center);
            if (dist < cluster->radius) {
                // Fraction of the ray inside the cluster
                float chord = 2.0f * std::sqrt(
                    std::max(0.0f, cluster->radius * cluster->radius -
                                       dist * dist));
                float overlap =
                    glm::clamp(chord / (2.0f * cluster->radius), 0.0f, 1.0f);
                visibility *= std::pow(cluster->translucency, overlap);
            }
        }

        total_illumination += visibility;
        weighted_direction += direction * visibility;
    }

    if (out_light_direction) {
        if (glm::length(weighted_direction) > 1e-4f) {
            *out_light_direction = glm::normalize(weighted_direction);
        } else {
            *out_light_direction = light_dir;
        }
    }

    return glm::clamp(total_illumination / static_cast<float>(num_samples),
                      0.0f, 1.0f);
}

glm::vec3 TreeGrowth::rotate_vector(const glm::vec3& vec,
                                    const glm::vec3& axis,
                                    float angle)
{
    return glm::rotate(vec, angle, axis);
}

glm::vec3 TreeGrowth::get_perpendicular(const glm::vec3& vec)
{
    glm::vec3 arbitrary = (std::abs(vec.x) < 0.9f)
                              ? glm::vec3(1.0f, 0.0f, 0.0f)
                              : glm::vec3(0.0f, 1.0f, 0.0f);

    return glm::normalize(glm::cross(vec, arbitrary));
}

}  // namespace TreeGen
