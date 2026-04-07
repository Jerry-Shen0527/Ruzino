#include <Eigen/Dense>
#include <Eigen/IterativeLinearSolvers>
#include <Eigen/Sparse>
#include <glm/glm.hpp>
#include <set>

#include "Eigen/src/IterativeLinearSolvers/ConjugateGradient.h"
#include "Eigen/src/SparseCore/SparseMatrix.h"
#include "GCore/Components/MeshComponent.h"
#include "GCore/Components/PointsComponent.h"
#include "GCore/algorithms/intersection.h"
#include "GCore/geom_payload.hpp"
#include "nodes/core/def/node_def.hpp"
#include "nodes/core/io/json.hpp"
#include "spdlog/spdlog.h"

NODE_DEF_OPEN_SCOPE

// Storage for persistent simulation state
struct MassSpringImplicitStorage {
    constexpr static bool has_storage = false;

    std::vector<glm::vec3> velocities;
    std::vector<glm::vec3> rest_positions;
    std::vector<std::pair<int, int>> springs;
    std::vector<float> rest_lengths;
    std::vector<float> spring_stiffness;
    bool initialized = false;
};

// Helper: Initialize spring network from mesh edges
static void initialize_springs(
    MassSpringImplicitStorage& storage,
    const std::vector<glm::vec3>& positions,
    const std::vector<int>& face_vertex_indices,
    const std::vector<int>& face_counts,
    float stiffness)
{
    storage.springs.clear();
    storage.rest_lengths.clear();
    storage.spring_stiffness.clear();

    std::set<std::pair<int, int>> edge_set;

    // Extract edges from faces
    int idx = 0;
    for (int face_count : face_counts) {
        for (int i = 0; i < face_count; ++i) {
            int v0 = face_vertex_indices[idx + i];
            int v1 = face_vertex_indices[idx + (i + 1) % face_count];
            if (v0 > v1)
                std::swap(v0, v1);
            edge_set.insert({ v0, v1 });
        }
        idx += face_count;
    }

    // Add springs for coincident vertices
    float epsilon = 1e-6f;
    int num_particles = positions.size();
    for (int i = 0; i < num_particles; ++i) {
        for (int j = i + 1; j < num_particles; ++j) {
            if (glm::length(positions[i] - positions[j]) < epsilon) {
                edge_set.insert({ i, j });
            }
        }
    }

    // Create springs from edges
    for (const auto& edge : edge_set) {
        storage.springs.push_back(edge);
        float rest_len = glm::length(
            storage.rest_positions[edge.first] -
            storage.rest_positions[edge.second]);
        storage.rest_lengths.push_back(rest_len);
        storage.spring_stiffness.push_back(stiffness);
    }

    spdlog::debug("Initialized {} springs", storage.springs.size());
}

// Helper: Compute total energy
static double compute_energy(
    const Eigen::VectorXd& x_curr,
    const Eigen::VectorXd& x_tilde,
    const Eigen::VectorXd& M_diag,
    const Eigen::VectorXd& f_ext,
    const std::vector<std::pair<int, int>>& springs,
    const std::vector<float>& rest_lengths,
    const std::vector<float>& spring_stiffness,
    double dt)
{
    // Inertial energy: 0.5 * M * ||x - x_tilde||^2
    double E_inertial =
        0.5 * (M_diag.asDiagonal() * (x_curr - x_tilde)).dot(x_curr - x_tilde);

    // Spring energy
    double E_spring = 0.0;
    for (size_t s = 0; s < springs.size(); ++s) {
        int i = springs[s].first;
        int j = springs[s].second;
        double k = spring_stiffness[s];
        double l0 = rest_lengths[s];
        double l0_sq = l0 * l0;

        Eigen::Vector3d xi(x_curr(i * 3), x_curr(i * 3 + 1), x_curr(i * 3 + 2));
        Eigen::Vector3d xj(x_curr(j * 3), x_curr(j * 3 + 1), x_curr(j * 3 + 2));
        Eigen::Vector3d diff = xi - xj;
        double diff_sq = diff.squaredNorm();

        E_spring += 0.5 * k * l0_sq * std::pow(diff_sq / l0_sq - 1.0, 2);
    }

    // External force potential: -f_ext^T * x
    // For gravity, this is: -m*g*z, which increases (becomes less negative) as
    // object rises
    double E_ext = -f_ext.dot(x_curr);

    return E_inertial + dt * dt * E_spring + dt * dt * E_ext;
}

// Helper: Compute energy gradient
static void compute_gradient(
    Eigen::VectorXd& grad,
    const Eigen::VectorXd& x_curr,
    const Eigen::VectorXd& x_tilde,
    const Eigen::VectorXd& M_diag,
    const Eigen::VectorXd& f_ext,
    const std::vector<std::pair<int, int>>& springs,
    const std::vector<float>& rest_lengths,
    const std::vector<float>& spring_stiffness,
    double dt)
{
    grad = M_diag.asDiagonal() * (x_curr - x_tilde);

    for (size_t s = 0; s < springs.size(); ++s) {
        int i = springs[s].first;
        int j = springs[s].second;
        double k = spring_stiffness[s];
        double l0 = rest_lengths[s];
        double l0_sq = l0 * l0;

        Eigen::Vector3d xi(x_curr(i * 3), x_curr(i * 3 + 1), x_curr(i * 3 + 2));
        Eigen::Vector3d xj(x_curr(j * 3), x_curr(j * 3 + 1), x_curr(j * 3 + 2));
        Eigen::Vector3d diff = xi - xj;
        double diff_sq = diff.squaredNorm();

        Eigen::Vector3d g_diff = 2.0 * k * (diff_sq / l0_sq - 1.0) * diff;
        grad.segment<3>(i * 3) += dt * dt * g_diff;
        grad.segment<3>(j * 3) -= dt * dt * g_diff;
    }

    grad -= dt * dt * f_ext;
}

// Helper: Assemble Hessian matrix with PSD projection
static void assemble_hessian(
    Eigen::SparseMatrix<double>& H,
    const Eigen::VectorXd& x_curr,
    const Eigen::VectorXd& M_diag,
    const std::vector<std::pair<int, int>>& springs,
    const std::vector<float>& rest_lengths,
    const std::vector<float>& spring_stiffness,
    double dt,
    int num_particles)
{
    typedef Eigen::Triplet<double> T;
    std::vector<T> triplets;
    triplets.reserve(num_particles * 9 + springs.size() * 36);

    // Mass matrix with regularization
    double regularization = 1e-6;
    for (int i = 0; i < num_particles * 3; ++i) {
        triplets.push_back(T(i, i, M_diag(i) + regularization));
    }

    // Spring Hessian blocks
    for (size_t s = 0; s < springs.size(); ++s) {
        int vi = springs[s].first;
        int vj = springs[s].second;
        double k = spring_stiffness[s];
        double l0 = rest_lengths[s];

        if (l0 < 1e-10)
            continue;

        double l0_sq = l0 * l0;
        Eigen::Vector3d xi(
            x_curr(vi * 3), x_curr(vi * 3 + 1), x_curr(vi * 3 + 2));
        Eigen::Vector3d xj(
            x_curr(vj * 3), x_curr(vj * 3 + 1), x_curr(vj * 3 + 2));
        Eigen::Vector3d diff = xi - xj;
        double diff_sq = diff.squaredNorm();

        // H_diff = 2*k/l0^2 * (2*outer(diff,diff) + (diff_sq - l0^2)*I)
        Eigen::Matrix3d outer = diff * diff.transpose();
        Eigen::Matrix3d H_diff =
            2.0 * k / l0_sq *
            (2.0 * outer + (diff_sq - l0_sq) * Eigen::Matrix3d::Identity());

        // PSD projection
        Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eigensolver(H_diff);
        Eigen::Vector3d eigenvalues = eigensolver.eigenvalues();
        Eigen::Matrix3d eigenvectors = eigensolver.eigenvectors();
        for (int e = 0; e < 3; ++e) {
            if (eigenvalues(e) < 0.0)
                eigenvalues(e) = 0.0;
        }
        H_diff =
            eigenvectors * eigenvalues.asDiagonal() * eigenvectors.transpose();

        // Add 6x6 block
        double scale = dt * dt;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                double val = scale * H_diff(r, c);
                triplets.push_back(T(vi * 3 + r, vi * 3 + c, val));
                triplets.push_back(T(vi * 3 + r, vj * 3 + c, -val));
                triplets.push_back(T(vj * 3 + r, vi * 3 + c, -val));
                triplets.push_back(T(vj * 3 + r, vj * 3 + c, val));
            }
        }
    }

    H.setFromTriplets(triplets.begin(), triplets.end());
}

// Helper: Newton solver with gradient-based line search
static bool solve_newton(
    Eigen::VectorXd& x_new,
    const Eigen::VectorXd& x_tilde,
    const Eigen::VectorXd& M_diag,
    const Eigen::VectorXd& f_ext,
    const std::vector<std::pair<int, int>>& springs,
    const std::vector<float>& rest_lengths,
    const std::vector<float>& spring_stiffness,
    int num_particles,
    double dt,
    int max_iterations,
    float tolerance)
{
    for (int iter = 0; iter < max_iterations; ++iter) {
        // Compute gradient
        Eigen::VectorXd grad;
        compute_gradient(
            grad,
            x_new,
            x_tilde,
            M_diag,
            f_ext,
            springs,
            rest_lengths,
            spring_stiffness,
            dt);

        double grad_norm = grad.norm();

        if (!std::isfinite(grad_norm)) {
            spdlog::error("Gradient contains NaN/Inf at iteration {}", iter);
            return false;
        }

        if (grad_norm / dt < tolerance) {
            spdlog::info(
                "Converged at iteration {} with grad_norm={:.6e}",
                iter,
                grad_norm / dt);
            return true;
        }

        // Assemble Hessian
        Eigen::SparseMatrix<double> H(num_particles * 3, num_particles * 3);
        assemble_hessian(
            H,
            x_new,
            M_diag,
            springs,
            rest_lengths,
            spring_stiffness,
            dt,
            num_particles);

        spdlog::debug(
            "Assembling Hessian: {} particles, {} springs, nnz={}",
            num_particles,
            springs.size(),
            H.nonZeros());

        // Solve for Newton direction
        Eigen::ConjugateGradient<Eigen::SparseMatrix<double>> solver;
        solver.setMaxIterations(300);
        solver.setTolerance(1e-8);
        solver.compute(H);

        if (solver.info() != Eigen::Success) {
            spdlog::warn("Hessian factorization failed");
            return false;
        }

        Eigen::VectorXd p = solver.solve(-grad);

        if (solver.info() != Eigen::Success || !p.allFinite()) {
            spdlog::warn("Newton solve failed at iteration {}", iter);
            return false;
        }

        spdlog::debug(
            "Newton iter {}: grad_norm={:.6e}, solver_iters={}",
            iter,
            grad_norm / dt,
            solver.iterations());

        // Energy-based line search (like CUDA version)
        double E_current = compute_energy(
            x_new,
            x_tilde,
            M_diag,
            f_ext,
            springs,
            rest_lengths,
            spring_stiffness,
            dt);

        double alpha = 1.0;
        Eigen::VectorXd x_candidate = x_new + alpha * p;
        double E_candidate = compute_energy(
            x_candidate,
            x_tilde,
            M_diag,
            f_ext,
            springs,
            rest_lengths,
            spring_stiffness,
            dt);

        int ls_iter = 0;
        while (E_candidate > E_current && alpha > 1e-8 && ls_iter < 20) {
            alpha *= 0.5;
            x_candidate = x_new + alpha * p;
            E_candidate = compute_energy(
                x_candidate,
                x_tilde,
                M_diag,
                f_ext,
                springs,
                rest_lengths,
                spring_stiffness,
                dt);
            ls_iter++;
        }

        if (alpha < 1e-8) {
            spdlog::warn("Line search failed to reduce energy");
            return false;
        }
        else {
            spdlog::debug(
                "  Line search: alpha={:.3e}, E: {:.6e} -> {:.6e}",
                alpha,
                E_current,
                E_candidate);
            x_new = x_candidate;
        }
    }

    return true;
}

NODE_DECLARATION_FUNCTION(mass_spring_implicit)
{
    b.add_input<Geometry>("Geometry");
    b.add_input<float>("Mass").default_val(1.0f).min(0.01f).max(100.0f);
    b.add_input<float>("Stiffness")
        .default_val(1000.0f)
        .min(1.0f)
        .max(10000.0f);
    b.add_input<float>("Damping").default_val(1.0f).min(0.0f).max(1.0f);
    b.add_input<int>("Newton Iterations").default_val(3).min(1).max(20);
    b.add_input<float>("Newton Tolerance")
        .default_val(1e-3f)
        .min(1e-8f)
        .max(1e-2f);
    b.add_input<float>("Gravity").default_val(-9.81f).min(-20.0f).max(0.0f);
    b.add_input<float>("Ground Restitution")
        .default_val(0.3f)
        .min(0.0f)
        .max(1.0f);
    b.add_input<bool>("Flip Normal").default_val(false);

    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(mass_spring_implicit)
{
    auto& global_payload = params.get_global_payload<GeomPayload&>();
    auto& storage = params.get_storage<MassSpringImplicitStorage&>();

    // Get inputs
    auto input_geom = params.get_input<Geometry>("Geometry");
    input_geom.apply_transform();

    float mass = params.get_input<float>("Mass");
    float stiffness = params.get_input<float>("Stiffness");
    float damping = params.get_input<float>("Damping");
    int max_iterations = params.get_input<int>("Newton Iterations");
    float tolerance = params.get_input<float>("Newton Tolerance");
    tolerance = std::max(tolerance, 1e-8f);
    float gravity = params.get_input<float>("Gravity");
    float restitution = params.get_input<float>("Ground Restitution");
    bool flip_normal = params.get_input<bool>("Flip Normal");
    float dt = global_payload.delta_time;

    spdlog::debug(
        "[CPU] Params: mass={:.2f}, k={:.1f}, damp={:.3f}, maxIter={}, "
        "tol={:.2e}, g={:.2f}, rest={:.2f}, dt={:.6f}",
        mass,
        stiffness,
        damping,
        max_iterations,
        tolerance,
        gravity,
        restitution,
        dt);

    // Get mesh component
    auto mesh_component = input_geom.get_component<MeshComponent>();
    std::vector<glm::vec3> positions;
    std::vector<int> face_vertex_indices;

    if (mesh_component) {
        positions = mesh_component->get_vertices();
        face_vertex_indices = mesh_component->get_face_vertex_indices();
    }
    else {
        auto points_component = input_geom.get_component<PointsComponent>();
        if (!points_component) {
            params.set_output<Geometry>("Geometry", std::move(input_geom));
            return true;
        }
        positions = points_component->get_vertices();
    }

    int num_particles = positions.size();
    if (num_particles == 0) {
        params.set_output<Geometry>("Geometry", std::move(input_geom));
        return true;
    }

    // Initialize storage on first run
    if (!storage.initialized || storage.velocities.size() != num_particles) {
        storage.velocities.resize(num_particles, glm::vec3(0.0f));
        storage.rest_positions = positions;

        if (mesh_component && !face_vertex_indices.empty()) {
            auto face_counts = mesh_component->get_face_vertex_counts();
            initialize_springs(
                storage,
                positions,
                face_vertex_indices,
                face_counts,
                stiffness);
        }

        storage.initialized = true;
    }

    // Convert to Eigen format
    Eigen::VectorXd x(num_particles * 3);
    Eigen::VectorXd v(num_particles * 3);
    for (int i = 0; i < num_particles; ++i) {
        x(i * 3 + 0) = positions[i].x;
        x(i * 3 + 1) = positions[i].y;
        x(i * 3 + 2) = positions[i].z;
        v(i * 3 + 0) = storage.velocities[i].x;
        v(i * 3 + 1) = storage.velocities[i].y;
        v(i * 3 + 2) = storage.velocities[i].z;
    }

    // Inertia term: x̃ = x^n + Δt * v^n (implicit Euler predictive position)
    // Use UNDAMPED velocity for prediction
    Eigen::VectorXd x_tilde = x + dt * v;
    Eigen::VectorXd x_n = x;  // Save initial position

    // Mass matrix (diagonal)
    Eigen::VectorXd M_diag(num_particles * 3);
    M_diag.setConstant(mass);

    // External forces (gravity)
    Eigen::VectorXd f_ext(num_particles * 3);
    f_ext.setZero();
    for (int i = 0; i < num_particles; ++i) {
        f_ext(i * 3 + 2) = mass * gravity;  // gravity in z direction
    }

    spdlog::info(
        "Implicit solver: {} particles, {} springs",
        num_particles,
        storage.springs.size());

    // Solve using Newton's method
    Eigen::VectorXd x_new = x;
    bool converged = solve_newton(
        x_new,
        x_tilde,
        M_diag,
        f_ext,
        storage.springs,
        storage.rest_lengths,
        storage.spring_stiffness,
        num_particles,
        dt,
        max_iterations,
        tolerance);

    if (!converged) {
        spdlog::warn("Newton solver failed to converge");
    }

    // Update velocity BEFORE collision handling: v = (x_new - x_n) / dt
    // This captures the velocity based on Newton's solution
    v = (x_new - x_n) / dt;

    // Apply damping to velocity
    v *= damping;

    // Handle ground collision
    int num_collisions = 0;
    for (int i = 0; i < num_particles; ++i) {
        if (x_new(i * 3 + 2) < 0.0) {  // Penetrating ground
            // Project position to ground
            x_new(i * 3 + 2) = 0.0;

            // Apply collision response to velocity
            if (v(i * 3 + 2) < 0.0) {  // Moving downward
                v(i * 3 + 2) = -v(i * 3 + 2) * restitution;
                double friction = 0.8;
                v(i * 3 + 0) *= friction;
                v(i * 3 + 1) *= friction;
            }
            num_collisions++;
        }
    }

    // Convert back to glm format
    for (int i = 0; i < num_particles; ++i) {
        positions[i].x = x_new(i * 3 + 0);
        positions[i].y = x_new(i * 3 + 1);
        positions[i].z = x_new(i * 3 + 2);
        storage.velocities[i].x = v(i * 3 + 0);
        storage.velocities[i].y = v(i * 3 + 1);
        storage.velocities[i].z = v(i * 3 + 2);
    }

    if (num_collisions > 0) {
        spdlog::debug("Ground collisions: {} particles", num_collisions);
    }

    // Update geometry with new positions
    if (mesh_component) {
        mesh_component->set_vertices(positions);

        // Recalculate normals
        std::vector<glm::vec3> normals;
        normals.reserve(face_vertex_indices.size());

        auto face_counts = mesh_component->get_face_vertex_counts();
        int idx = 0;
        for (int face_count : face_counts) {
            if (face_count >= 3) {
                int i0 = face_vertex_indices[idx];
                int i1 = face_vertex_indices[idx + 1];
                int i2 = face_vertex_indices[idx + 2];

                glm::vec3 edge1 = positions[i1] - positions[i0];
                glm::vec3 edge2 = positions[i2] - positions[i0];
                glm::vec3 normal = glm::cross(
                    flip_normal ? edge1 : edge2, flip_normal ? edge2 : edge1);

                float length = glm::length(normal);
                if (length > 1e-8f) {
                    normal = normal / length;
                }
                else {
                    normal = glm::vec3(0.0f, 0.0f, 1.0f);
                }

                for (int i = 0; i < face_count; ++i) {
                    normals.push_back(normal);
                }
            }
            idx += face_count;
        }

        if (!normals.empty()) {
            mesh_component->set_normals(normals);
        }
    }
    else {
        auto points_component = input_geom.get_component<PointsComponent>();
        points_component->set_vertices(positions);
    }

    params.set_output<Geometry>("Geometry", std::move(input_geom));
    return true;
}

NODE_DECLARATION_UI(mass_spring);
NODE_DEF_CLOSE_SCOPE
