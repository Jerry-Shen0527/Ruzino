#include <cusparse.h>
#include <stdio.h>
#include <thrust/device_vector.h>
#include <thrust/inner_product.h>
#include <thrust/reduce.h>
#include <thrust/sort.h>
#include <thrust/transform.h>
#include <thrust/transform_reduce.h>
#include <thrust/tuple.h>
#include <thrust/unique.h>
#include <cuda/std/functional>  // ::cuda::std::plus (replaces deprecated thrust::plus)

#include <Eigen/Dense>
#include <RHI/cuda.hpp>

#include "rzsim_cuda/adjacency_map.cuh"
#include "rzsim_cuda/neo_hookean.cuh"

RUZINO_NAMESPACE_OPEN_SCOPE

namespace rzsim_cuda {

// ============================================================================
// Basic Operations
// ============================================================================

void explicit_step_nh_gpu(
    cuda::CUDALinearBufferHandle x,
    cuda::CUDALinearBufferHandle v,
    float dt,
    int num_particles,
    cuda::CUDALinearBufferHandle x_tilde)
{
    const float* x_ptr = x->get_device_ptr<float>();
    const float* v_ptr = v->get_device_ptr<float>();
    float* x_tilde_ptr = x_tilde->get_device_ptr<float>();

    cuda::GPUParallelFor(
        "explicit_step_nh", num_particles * 3, [=] __device__(int i) {
            x_tilde_ptr[i] = x_ptr[i] + dt * v_ptr[i];
        });
}

// Compute lumped mass matrix from density and element volumes
// For each tetrahedral element: m_elem = density * volume
// Distribute equally to 4 vertices using lumped mass matrix
void compute_lumped_mass_matrix_gpu(
    const VolumeAdjacencyMap& volume_adjacency,
    cuda::CUDALinearBufferHandle volumes,
    float density,
    int num_particles,
    int num_elements,
    cuda::CUDALinearBufferHandle mass_matrix)
{
    // Zero out mass matrix first
    cudaMemset(
        mass_matrix->get_device_ptr<float>(),
        0,
        num_particles * 3 * sizeof(float));

    const unsigned* adjacency =
        volume_adjacency.adjacency_buffer()->get_device_ptr<unsigned>();
    const unsigned* offsets =
        volume_adjacency.offsets_buffer()->get_device_ptr<unsigned>();
    const int* element_to_vertex =
        volume_adjacency.element_to_vertex_buffer()->get_device_ptr<int>();
    const int* element_to_local_face =
        volume_adjacency.element_to_local_face_buffer()->get_device_ptr<int>();
    const float* volumes_ptr = volumes->get_device_ptr<float>();
    float* mass_matrix_ptr = mass_matrix->get_device_ptr<float>();

    cuda::GPUParallelFor(
        "compute_lumped_mass", num_elements, [=] __device__(int elem_idx) {
            int v_apex = element_to_vertex[elem_idx];
            int local_face_idx = element_to_local_face[elem_idx];

            unsigned offset = offsets[v_apex];
            unsigned face_a = adjacency[offset + 1 + local_face_idx * 3 + 0];
            unsigned face_b = adjacency[offset + 1 + local_face_idx * 3 + 1];
            unsigned face_c = adjacency[offset + 1 + local_face_idx * 3 + 2];

            int tet[4] = { v_apex, (int)face_a, (int)face_b, (int)face_c };
            float volume = volumes_ptr[elem_idx];

            // Element mass distributed equally to 4 vertices (lumped mass
            // matrix)
            float elem_mass = density * volume;
            float mass_per_vertex = elem_mass / 4.0f;

            // Accumulate to each vertex (3 DOFs per vertex)
            for (int i = 0; i < 4; i++) {
                atomicAdd(&mass_matrix_ptr[tet[i] * 3 + 0], mass_per_vertex);
                atomicAdd(&mass_matrix_ptr[tet[i] * 3 + 1], mass_per_vertex);
                atomicAdd(&mass_matrix_ptr[tet[i] * 3 + 2], mass_per_vertex);
            }
        });
}

// Setup external forces using FEM integration
// For body force f = density * g, the force on each vertex is:
// F_i = ∫_Ω N_i * ρ * g dV
// For linear tetrahedron with lumped integration: F_i = (ρ * V / 4) * g
void setup_external_forces_fem_gpu(
    const VolumeAdjacencyMap& volume_adjacency,
    cuda::CUDALinearBufferHandle volumes,
    float density,
    float gravity,
    int num_particles,
    int num_elements,
    cuda::CUDALinearBufferHandle f_ext)
{
    // Zero out force vector first
    cudaMemset(
        f_ext->get_device_ptr<float>(), 0, num_particles * 3 * sizeof(float));

    const unsigned* adjacency =
        volume_adjacency.adjacency_buffer()->get_device_ptr<unsigned>();
    const unsigned* offsets =
        volume_adjacency.offsets_buffer()->get_device_ptr<unsigned>();
    const int* element_to_vertex =
        volume_adjacency.element_to_vertex_buffer()->get_device_ptr<int>();
    const int* element_to_local_face =
        volume_adjacency.element_to_local_face_buffer()->get_device_ptr<int>();
    const float* volumes_ptr = volumes->get_device_ptr<float>();
    float* f_ext_ptr = f_ext->get_device_ptr<float>();

    cuda::GPUParallelFor(
        "setup_external_forces_fem",
        num_elements,
        [=] __device__(int elem_idx) {
            int v_apex = element_to_vertex[elem_idx];
            int local_face_idx = element_to_local_face[elem_idx];

            unsigned offset = offsets[v_apex];
            unsigned face_a = adjacency[offset + 1 + local_face_idx * 3 + 0];
            unsigned face_b = adjacency[offset + 1 + local_face_idx * 3 + 1];
            unsigned face_c = adjacency[offset + 1 + local_face_idx * 3 + 2];

            int tet[4] = { v_apex, (int)face_a, (int)face_b, (int)face_c };
            float volume = volumes_ptr[elem_idx];

            // Force per vertex from gravity (lumped)
            // f = (ρ * V / 4) * g, where g = [0, 0, gravity]
            float force_z = (density * volume / 4.0f) * gravity;

            // Accumulate to each vertex (only z component)
            for (int i = 0; i < 4; i++) {
                atomicAdd(&f_ext_ptr[tet[i] * 3 + 2], force_z);
            }
        });
}

void setup_external_forces_nh_gpu(
    float mass,
    float gravity,
    int num_particles,
    cuda::CUDALinearBufferHandle f_ext)
{
    float* f_ext_ptr = f_ext->get_device_ptr<float>();

    cuda::GPUParallelFor(
        "setup_external_forces_nh", num_particles * 3, [=] __device__(int i) {
            // Gravity acts on z-component (i % 3 == 2)
            f_ext_ptr[i] = (i % 3 == 2) ? mass * gravity : 0.0f;
        });
}

// ============================================================================
// Neo-Hookean Energy Model
// ============================================================================

// Compute deformation gradient F = Ds * Dm_inv
// Ds = [x1-x0, x2-x0, x3-x0] (3x3 matrix from current positions)
// Dm_inv is precomputed from rest positions
__device__ void compute_deformation_gradient(
    const float* x,
    const int* tet,
    const float* Dm_inv,
    Eigen::Matrix3f& F)
{
    // Get vertex positions
    Eigen::Vector3f x0(x[tet[0] * 3 + 0], x[tet[0] * 3 + 1], x[tet[0] * 3 + 2]);
    Eigen::Vector3f x1(x[tet[1] * 3 + 0], x[tet[1] * 3 + 1], x[tet[1] * 3 + 2]);
    Eigen::Vector3f x2(x[tet[2] * 3 + 0], x[tet[2] * 3 + 1], x[tet[2] * 3 + 2]);
    Eigen::Vector3f x3(x[tet[3] * 3 + 0], x[tet[3] * 3 + 1], x[tet[3] * 3 + 2]);

    // Compute Ds = [x1-x0, x2-x0, x3-x0]
    Eigen::Matrix3f Ds;
    Ds.col(0) = x1 - x0;
    Ds.col(1) = x2 - x0;
    Ds.col(2) = x3 - x0;

    // Load Dm_inv (stored in column-major order)
    Eigen::Matrix3f Dm_inv_mat;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            Dm_inv_mat(i, j) = Dm_inv[j * 3 + i];  // Column-major
        }
    }

    // F = Ds * Dm_inv
    F = Ds * Dm_inv_mat;
}

// Neo-Hookean energy density: Ψ = μ/2 * (tr(F^T F) - 3) - μ log(J) + λ/2 *
// log(J)^2
__device__ float
neo_hookean_energy_density(const Eigen::Matrix3f& F, float mu, float lambda)
{
    float J = F.determinant();
    if (J <= 0.0f) {
        // Inverted element - use large penalty
        return 1e10f;
    }

    Eigen::Matrix3f FtF = F.transpose() * F;
    float I1 = FtF.trace();  // tr(F^T F)
    float log_J = logf(J);

    float psi =
        0.5f * mu * (I1 - 3.0f) - mu * log_J + 0.5f * lambda * log_J * log_J;
    return psi;
}

// First Piola-Kirchhoff stress: P = ∂Ψ/∂F = μ(F - F^-T) + λ log(J) F^-T
__device__ void compute_pk1_stress(
    const Eigen::Matrix3f& F,
    float mu,
    float lambda,
    Eigen::Matrix3f& P)
{
    float J = F.determinant();
    if (fabsf(J) <= 1e-6f || !isfinite(J)) {
        // Degenerate/inverted case
        P.setZero();
        return;
    }

    Eigen::Matrix3f F_inv = F.inverse();
    Eigen::Matrix3f F_inv_T = F_inv.transpose();

    // Check inverse validity
    bool valid = true;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            if (!isfinite(F_inv_T(i, j))) {
                valid = false;
                break;
            }
        }
        if (!valid)
            break;
    }

    if (!valid) {
        P.setZero();
        return;
    }

    float log_J = logf(fabsf(J));

    if (!isfinite(log_J)) {
        P.setZero();
        return;
    }

    P = mu * (F - F_inv_T) + lambda * log_J * F_inv_T;

    // Check result and clamp stress to prevent numerical explosion
    const float max_stress = 1e6f;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            if (!isfinite(P(i, j))) {
                P.setZero();
                return;
            }
            // Clamp extreme values
            if (fabsf(P(i, j)) > max_stress) {
                P(i, j) = (P(i, j) > 0.0f) ? max_stress : -max_stress;
            }
        }
    }
}

// Compute gradient contribution from one tetrahedron
// Now using Eigen properly - it works fine in CUDA device code!
__device__ void add_element_gradient(
    const float* x_curr,
    const int* tet,
    const float* Dm_inv,
    float volume,
    float mu,
    float lambda,
    float* grad_local)
{
    // Compute F using Eigen
    Eigen::Matrix3f F;
    compute_deformation_gradient(x_curr, tet, Dm_inv, F);

    // Compute PK1 stress using the existing function
    Eigen::Matrix3f P;
    compute_pk1_stress(F, mu, lambda, P);

    // Check if stress is valid
    bool valid = true;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            if (!isfinite(P(i, j))) {
                valid = false;
                break;
            }
        }
        if (!valid)
            break;
    }

    if (!valid) {
        for (int i = 0; i < 12; i++)
            grad_local[i] = 0.0f;
        return;
    }

    // Load Dm_inv as Eigen matrix
    Eigen::Matrix3f Dm_inv_mat;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            Dm_inv_mat(i, j) = Dm_inv[j * 3 + i];  // Column-major
        }
    }

    // H = V * P * Dm_inv^T (gradient of elastic energy w.r.t. vertex positions)
    Eigen::Matrix3f H = volume * P * Dm_inv_mat.transpose();

    // Forces on vertices 1, 2, 3 (columns of H)
    for (int i = 0; i < 3; i++) {
        grad_local[1 * 3 + i] = H(i, 0);
        grad_local[2 * 3 + i] = H(i, 1);
        grad_local[3 * 3 + i] = H(i, 2);
    }

    // Force on vertex 0 (equilibrium: sum of forces = 0)
    for (int i = 0; i < 3; i++) {
        grad_local[0 * 3 + i] = -(H(i, 0) + H(i, 1) + H(i, 2));
    }
}

// Gradient kernel - only inertial term

// Accumulate elastic forces from elements (now includes gravity)

void compute_neg_gradient_nh_gpu(
    cuda::CUDALinearBufferHandle x_curr,
    cuda::CUDALinearBufferHandle x_tilde,
    cuda::CUDALinearBufferHandle M_diag,
    const VolumeAdjacencyMap& volume_adjacency,
    cuda::CUDALinearBufferHandle Dm_inv,
    cuda::CUDALinearBufferHandle volumes,
    float mu,
    float lambda,
    float density,
    float gravity,
    float dt,
    int num_particles,
    int num_elements,
    cuda::CUDALinearBufferHandle neg_grad)
{
    const float* x_curr_ptr = x_curr->get_device_ptr<float>();
    const float* x_tilde_ptr = x_tilde->get_device_ptr<float>();
    const float* M_diag_ptr = M_diag->get_device_ptr<float>();
    float* neg_grad_ptr = neg_grad->get_device_ptr<float>();

    // First pass: negative inertial term only
    cuda::GPUParallelFor(
        "compute_gradient_nh", num_particles, [=] __device__(int i) {
            // Initialize with negative inertial term: -M * (x - x_tilde)
            neg_grad_ptr[i * 3 + 0] =
                -M_diag_ptr[i * 3 + 0] *
                (x_curr_ptr[i * 3 + 0] - x_tilde_ptr[i * 3 + 0]);
            neg_grad_ptr[i * 3 + 1] =
                -M_diag_ptr[i * 3 + 1] *
                (x_curr_ptr[i * 3 + 1] - x_tilde_ptr[i * 3 + 1]);
            neg_grad_ptr[i * 3 + 2] =
                -M_diag_ptr[i * 3 + 2] *
                (x_curr_ptr[i * 3 + 2] - x_tilde_ptr[i * 3 + 2]);
        });

    const unsigned* adjacency =
        volume_adjacency.adjacency_buffer()->get_device_ptr<unsigned>();
    const unsigned* offsets =
        volume_adjacency.offsets_buffer()->get_device_ptr<unsigned>();
    const int* element_to_vertex =
        volume_adjacency.element_to_vertex_buffer()->get_device_ptr<int>();
    const int* element_to_local_face =
        volume_adjacency.element_to_local_face_buffer()->get_device_ptr<int>();
    const float* Dm_inv_ptr = Dm_inv->get_device_ptr<float>();
    const float* volumes_ptr = volumes->get_device_ptr<float>();

    // Second pass: negative elastic forces + gravity
    cuda::GPUParallelFor(
        "accumulate_elastic_forces",
        num_elements,
        [=] __device__(int elem_idx) {
            // Step 1: Test mapping lookup
            int v_apex = element_to_vertex[elem_idx];
            int local_face_idx = element_to_local_face[elem_idx];

            if (v_apex < 0 || v_apex >= num_particles) {
                return;
            }

            unsigned offset = offsets[v_apex];
            unsigned count = adjacency[offset];

            if (local_face_idx < 0 || local_face_idx >= (int)count) {
                return;
            }

            unsigned face_a = adjacency[offset + 1 + local_face_idx * 3 + 0];
            unsigned face_b = adjacency[offset + 1 + local_face_idx * 3 + 1];
            unsigned face_c = adjacency[offset + 1 + local_face_idx * 3 + 2];

            if (face_a >= (unsigned)num_particles ||
                face_b >= (unsigned)num_particles ||
                face_c >= (unsigned)num_particles) {
                return;
            }

            int tet[4] = { v_apex, (int)face_a, (int)face_b, (int)face_c };
            const float* Dm_inv_local = &Dm_inv_ptr[elem_idx * 9];
            float volume = volumes_ptr[elem_idx];

            if (volume <= 0.0f || !isfinite(volume)) {
                return;
            }

            // Compute gradient using raw float arrays (Eigen version causes
            // device errors)
            float grad_local[12] = { 0 };
            add_element_gradient(
                x_curr_ptr, tet, Dm_inv_local, volume, mu, lambda, grad_local);

            // Check for NaN/Inf in grad_local
            bool valid = true;
            for (int i = 0; i < 12; i++) {
                if (!isfinite(grad_local[i])) {
                    valid = false;
                    break;
                }
            }

            if (!valid) {
                return;
            }

            // Compute gravity force per vertex: F_g = (density * volume / 4) *
            // gravity (lumped FEM)
            float gravity_force_z = (density * volume / 4.0f) * gravity;

            // Add negative elastic forces + gravity to negative gradient
            // gradient includes dt² scaling for both elastic and gravity terms
            for (int i = 0; i < 4; i++) {
                for (int j = 0; j < 3; j++) {
                    int idx = tet[i] * 3 + j;
                    if (idx >= 0 && idx < num_particles * 3) {
                        // Negative elastic forces (already scaled by dt²)
                        float force = -dt * dt * grad_local[i * 3 + j];
                        // Add negative gravity force (positive z direction for
                        // negative gradient)
                        if (j == 2) {
                            force += dt * dt * gravity_force_z;
                        }
                        atomicAdd(&neg_grad_ptr[idx], force);
                    }
                }
            }
        });
}

// ============================================================================
// Hessian Construction
// ============================================================================

// Custom 3x3 symmetric eigenvalue decomposition (same as mass-spring)
__device__ void eigen_decomposition_3x3_nh(
    const Eigen::Matrix3f& A,
    Eigen::Vector3f& eigenvalues,
    Eigen::Matrix3f& eigenvectors)
{
    const int MAX_ITER = 50;
    const float EPSILON = 1e-10f;

    eigenvectors.setIdentity();
    Eigen::Matrix3f a = A;

    for (int iter = 0; iter < MAX_ITER; iter++) {
        // Find largest off-diagonal element
        int p = 0, q = 1;
        float max_val = fabsf(a(0, 1));

        if (fabsf(a(0, 2)) > max_val) {
            p = 0;
            q = 2;
            max_val = fabsf(a(0, 2));
        }
        if (fabsf(a(1, 2)) > max_val) {
            p = 1;
            q = 2;
            max_val = fabsf(a(1, 2));
        }

        if (max_val < EPSILON)
            break;

        // Compute rotation angle
        float theta = 0.5f * atan2f(2.0f * a(p, q), a(q, q) - a(p, p));
        float c = cosf(theta);
        float s = sinf(theta);

        // Apply Givens rotation
        Eigen::Matrix3f G;
        G.setIdentity();
        G(p, p) = c;
        G(q, q) = c;
        G(p, q) = s;
        G(q, p) = -s;

        a = G.transpose() * a * G;
        eigenvectors = eigenvectors * G;
    }

    eigenvalues(0) = a(0, 0);
    eigenvalues(1) = a(1, 1);
    eigenvalues(2) = a(2, 2);
}

// Project matrix to PSD
__device__ Eigen::Matrix3f project_psd_nh(const Eigen::Matrix3f& H)
{
    Eigen::Vector3f eigenvalues;
    Eigen::Matrix3f eigenvectors;

    eigen_decomposition_3x3_nh(H, eigenvalues, eigenvectors);

    // Find max eigenvalue for relative thresholding
    float max_eig = fmaxf(
        fmaxf(fabsf(eigenvalues(0)), fabsf(eigenvalues(1))),
        fabsf(eigenvalues(2)));

    // Use relative threshold: clamp to at least 0.1% of max eigenvalue
    // This is less aggressive than absolute threshold
    float min_eigenvalue = fmaxf(1e-8f, 0.001f * max_eig);

    for (int i = 0; i < 3; i++) {
        if (eigenvalues(i) < min_eigenvalue)
            eigenvalues(i) = min_eigenvalue;
    }

    Eigen::Matrix3f result =
        eigenvectors * eigenvalues.asDiagonal() * eigenvectors.transpose();
    return result;
}

// Compute element Hessian (12x12) for Neo-Hookean model
// Using St. Venant-Kirchhoff (SVK) Hessian as approximation
// This is standard practice: use Neo-Hookean for gradient (forces)
// but SVK for Hessian (stiffness matrix) because it's simpler and SPD
__device__ void compute_element_hessian(
    const float* x_curr,
    const int* tet,
    const float* Dm_inv,
    float volume,
    float mu,
    float lambda,
    Eigen::Matrix<float, 12, 12>& K_elem)
{
    Eigen::Matrix3f F;
    compute_deformation_gradient(x_curr, tet, Dm_inv, F);

    float J = F.determinant();
    if (J <= 1e-10f || !isfinite(J)) {
        K_elem.setZero();
        return;
    }

    // Load Dm_inv
    Eigen::Matrix3f Dm_inv_mat;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            Dm_inv_mat(i, j) = Dm_inv[j * 3 + i];
        }
    }

    // St. Venant-Kirchhoff (SVK) elasticity tensor for tetrahedral FEM
    // This gives a symmetric positive-definite Hessian
    // C_ijkl = lambda * delta_ij * delta_kl + mu * (delta_ik * delta_jl +
    // delta_il * delta_jk)

    K_elem.setZero();

    // Build element stiffness matrix
    // K_ab^(alpha,beta) = V * sum_{i,j,k,l} dF_ij/dx_a^alpha * C_ijkl *
    // dF_kl/dx_b^beta For linear tetrahedron: dF/dx relates to Dm_inv

    for (int a = 0; a < 4; a++) {
        for (int b = 0; b < 4; b++) {
            Eigen::Matrix3f K_ab;
            K_ab.setZero();

            // Shape function gradients in reference configuration
            Eigen::Vector3f grad_Na, grad_Nb;
            if (a == 0) {
                grad_Na = -(
                    Dm_inv_mat.col(0) + Dm_inv_mat.col(1) + Dm_inv_mat.col(2));
            }
            else {
                grad_Na = Dm_inv_mat.col(a - 1);
            }

            if (b == 0) {
                grad_Nb = -(
                    Dm_inv_mat.col(0) + Dm_inv_mat.col(1) + Dm_inv_mat.col(2));
            }
            else {
                grad_Nb = Dm_inv_mat.col(b - 1);
            }

            // Compute 3x3 stiffness block using SVK constitutive model
            // For linear FEM: K_ab^(alpha,beta) = V * sum_{i,j} grad_Na^i C_{i
            // alpha j beta} grad_Nb^j where C_{ijkl} = lambda * delta_ij *
            // delta_kl + mu * (delta_ik * delta_jl + delta_il * delta_jk)

            // Correct formula for isotropic linear elasticity:
            // K_ab^(α,β) = V * [λ (∇N_a · ∇N_b) δ_αβ + μ (∇N_a^α ∇N_b^β +
            // ∇N_a^β ∇N_b^α)] In matrix form: K_ab = V * [λ (∇N_a · ∇N_b) I + μ
            // (∇N_a ⊗ ∇N_b + ∇N_b ⊗ ∇N_a)]
            float dot_product = grad_Na.dot(grad_Nb);
            K_ab = volume * lambda * dot_product * Eigen::Matrix3f::Identity();
            K_ab +=
                volume * mu *
                (grad_Na * grad_Nb.transpose() + grad_Nb * grad_Na.transpose());

            // Do NOT project individual blocks - this breaks symmetry
            // PSD projection will be done on the full assembled matrix if
            // needed

            // Fill into 12x12 matrix
            for (int alpha = 0; alpha < 3; alpha++) {
                for (int beta = 0; beta < 3; beta++) {
                    K_elem(a * 3 + alpha, b * 3 + beta) = K_ab(alpha, beta);
                }
            }
        }
    }
}

// Binary search for entry position (same as mass-spring)
__device__ int find_entry_position_nh(
    const int* unique_rows,
    const int* unique_cols,
    int nnz,
    int target_row,
    int target_col)
{
    int left = 0;
    int right = nnz - 1;

    while (left <= right) {
        int mid = (left + right) / 2;
        int row = unique_rows[mid];
        int col = unique_cols[mid];

        if (row == target_row && col == target_col) {
            return mid;
        }
        else if (row < target_row || (row == target_row && col < target_col)) {
            left = mid + 1;
        }
        else {
            right = mid - 1;
        }
    }

    return -1;
}

// Build CSR structure for Neo-Hookean Hessian
NeoHookeanCSRStructure build_hessian_structure_nh_gpu(
    const VolumeAdjacencyMap& volume_adjacency,
    int num_particles,
    int num_elements)
{
    NeoHookeanCSRStructure structure;
    int n = num_particles * 3;

    const unsigned* adj_ptr =
        volume_adjacency.adjacency_buffer()->get_device_ptr<unsigned>();
    const unsigned* off_ptr =
        volume_adjacency.offsets_buffer()->get_device_ptr<unsigned>();
    const int* elem_to_v_ptr =
        volume_adjacency.element_to_vertex_buffer()->get_device_ptr<int>();
    const int* elem_to_lf_ptr =
        volume_adjacency.element_to_local_face_buffer()->get_device_ptr<int>();

    // Each tetrahedron contributes 12x12 = 144 entries
    // For symmetric matrix, we need both (i,j) and (j,i) if i != j
    // Upper triangle: 12*13/2 = 78 unique entries per element
    // But we'll generate all 144 and rely on deduplication for simplicity
    int num_mass_entries = n;
    int num_element_entries = num_elements * 144;
    int total_entries = num_mass_entries + num_element_entries;

    // Allocate temporary buffers
    auto d_all_rows = cuda::create_cuda_linear_buffer<int>(total_entries);
    auto d_all_cols = cuda::create_cuda_linear_buffer<int>(total_entries);
    auto d_write_offset = cuda::create_cuda_linear_buffer<int>(1);
    cudaMemset(d_write_offset->get_device_ptr<int>(), 0, sizeof(int));

    int* rows = d_all_rows->get_device_ptr<int>();
    int* cols = d_all_cols->get_device_ptr<int>();
    int* write_offset = d_write_offset->get_device_ptr<int>();

    // Generate all (row, col) pairs
    cuda::GPUParallelFor(
        "generate_entries_nh", total_entries, [=] __device__(int idx) {
            if (idx < num_mass_entries) {
                // Mass diagonal entries
                rows[idx] = idx;
                cols[idx] = idx;
            }
            else {
                // Element entries
                int elem_entry_idx = idx - num_mass_entries;
                int elem_idx = elem_entry_idx / 144;
                int local_idx = elem_entry_idx % 144;

                int local_row = local_idx / 12;
                int local_col = local_idx % 12;

                // Use precomputed mapping
                int v_apex = elem_to_v_ptr[elem_idx];
                int local_face_idx = elem_to_lf_ptr[elem_idx];

                unsigned offset = off_ptr[v_apex];
                unsigned face_a = adj_ptr[offset + 1 + local_face_idx * 3 + 0];
                unsigned face_b = adj_ptr[offset + 1 + local_face_idx * 3 + 1];
                unsigned face_c = adj_ptr[offset + 1 + local_face_idx * 3 + 2];
                int tet[4] = { v_apex, (int)face_a, (int)face_b, (int)face_c };

                int global_row = tet[local_row / 3] * 3 + (local_row % 3);
                int global_col = tet[local_col / 3] * 3 + (local_col % 3);

                rows[idx] = global_row;
                cols[idx] = global_col;
            }
        });

    // Sort and deduplicate
    thrust::device_ptr<int> rows_ptr(rows);
    thrust::device_ptr<int> cols_ptr(cols);

    auto zip_begin =
        thrust::make_zip_iterator(thrust::make_tuple(rows_ptr, cols_ptr));
    auto zip_end = zip_begin + total_entries;

    thrust::sort(
        zip_begin, zip_end, [] __device__(const auto& a, const auto& b) {
            int r1 = thrust::get<0>(a);
            int c1 = thrust::get<1>(a);
            int r2 = thrust::get<0>(b);
            int c2 = thrust::get<1>(b);
            return (r1 < r2) || (r1 == r2 && c1 < c2);
        });

    cudaDeviceSynchronize();

    auto new_end = thrust::unique(
        zip_begin, zip_end, [] __device__(const auto& a, const auto& b) {
            return thrust::get<0>(a) == thrust::get<0>(b) &&
                   thrust::get<1>(a) == thrust::get<1>(b);
        });

    int nnz = new_end - zip_begin;
    cudaDeviceSynchronize();

    // Allocate CSR arrays
    structure.col_indices = cuda::create_cuda_linear_buffer<int>(nnz);
    structure.row_offsets = cuda::create_cuda_linear_buffer<int>(n + 1);
    structure.mass_value_positions = cuda::create_cuda_linear_buffer<int>(n);
    structure.element_value_positions =
        cuda::create_cuda_linear_buffer<int>(num_elements * 144);

    // Copy deduplicated rows and cols to permanent storage
    auto d_unique_rows = cuda::create_cuda_linear_buffer<int>(nnz);
    cudaMemcpy(
        d_unique_rows->get_device_ptr<int>(),
        rows,
        nnz * sizeof(int),
        cudaMemcpyDeviceToDevice);

    cudaMemcpy(
        structure.col_indices->get_device_ptr<int>(),
        cols,
        nnz * sizeof(int),
        cudaMemcpyDeviceToDevice);

    // Build row_offsets using the deduplicated rows
    thrust::device_ptr<int> row_offsets_ptr(
        structure.row_offsets->get_device_ptr<int>());
    thrust::fill(thrust::device, row_offsets_ptr, row_offsets_ptr + n + 1, 0);
    cudaDeviceSynchronize();

    int* row_offsets_raw = structure.row_offsets->get_device_ptr<int>();
    int* unique_rows_ptr = d_unique_rows->get_device_ptr<int>();

    thrust::device_ptr<int> unique_rows_tptr(unique_rows_ptr);

    thrust::for_each(
        thrust::device,
        unique_rows_tptr,
        unique_rows_tptr + nnz,
        [=] __device__(int row) { atomicAdd(&row_offsets_raw[row], 1); });

    cudaDeviceSynchronize();

    thrust::exclusive_scan(
        thrust::device,
        row_offsets_ptr,
        row_offsets_ptr + n + 1,
        row_offsets_ptr);
    cudaDeviceSynchronize();

    // Build mass diagonal positions
    const int* unique_rows = d_unique_rows->get_device_ptr<int>();
    const int* unique_cols = structure.col_indices->get_device_ptr<int>();
    int* mass_positions = structure.mass_value_positions->get_device_ptr<int>();

    cuda::GPUParallelFor("build_mass_positions_nh", n, [=] __device__(int dof) {
        mass_positions[dof] =
            find_entry_position_nh(unique_rows, unique_cols, nnz, dof, dof);
    });

    // Build element positions
    int* elem_positions =
        structure.element_value_positions->get_device_ptr<int>();

    cuda::GPUParallelFor(
        "build_element_positions_nh",
        num_elements * 144,
        [=] __device__(int idx) {
            int elem_idx = idx / 144;
            int local_idx = idx % 144;

            int local_row = local_idx / 12;
            int local_col = local_idx % 12;

            // Use precomputed mapping
            int v_apex = elem_to_v_ptr[elem_idx];
            int local_face_idx = elem_to_lf_ptr[elem_idx];

            unsigned offset = off_ptr[v_apex];
            unsigned face_a = adj_ptr[offset + 1 + local_face_idx * 3 + 0];
            unsigned face_b = adj_ptr[offset + 1 + local_face_idx * 3 + 1];
            unsigned face_c = adj_ptr[offset + 1 + local_face_idx * 3 + 2];
            int tet[4] = { v_apex, (int)face_a, (int)face_b, (int)face_c };

            int global_row = tet[local_row / 3] * 3 + (local_row % 3);
            int global_col = tet[local_col / 3] * 3 + (local_col % 3);

            elem_positions[idx] = find_entry_position_nh(
                unique_rows, unique_cols, nnz, global_row, global_col);
        });

    structure.num_rows = n;
    structure.num_cols = n;
    structure.nnz = nnz;

    return structure;
}

// Fill Hessian values kernel

void update_hessian_values_nh_gpu(
    const NeoHookeanCSRStructure& csr_structure,
    cuda::CUDALinearBufferHandle x_curr,
    cuda::CUDALinearBufferHandle M_diag,
    const VolumeAdjacencyMap& volume_adjacency,
    cuda::CUDALinearBufferHandle Dm_inv,
    cuda::CUDALinearBufferHandle volumes,
    float mu,
    float lambda,
    float dt,
    int num_particles,
    int num_elements,
    cuda::CUDALinearBufferHandle values)
{
    int num_dofs = num_particles * 3;

    // Zero out values
    cudaMemset(
        values->get_device_ptr<float>(), 0, csr_structure.nnz * sizeof(float));

    // Fill mass diagonal (like mass-spring: M + regularization)
    const float* M_diag_ptr = M_diag->get_device_ptr<float>();
    const int* mass_positions =
        csr_structure.mass_value_positions->get_device_ptr<int>();
    float* values_ptr = values->get_device_ptr<float>();

    cuda::GPUParallelFor(
        "fill_mass_diagonal_nh", num_dofs, [=] __device__(int dof) {
            int pos = mass_positions[dof];
            if (pos >= 0) {
                // Stronger regularization for numerical stability
                // Especially important for high Poisson ratios
                float regularization = 1e-4f;  // Increased from 1e-6f
                values_ptr[pos] = M_diag_ptr[dof] + regularization;
            }
        });

    // Fill element contributions
    const float* x_curr_ptr = x_curr->get_device_ptr<float>();
    const unsigned* adjacency =
        volume_adjacency.adjacency_buffer()->get_device_ptr<unsigned>();
    const unsigned* offsets =
        volume_adjacency.offsets_buffer()->get_device_ptr<unsigned>();
    const int* element_to_vertex =
        volume_adjacency.element_to_vertex_buffer()->get_device_ptr<int>();
    const int* element_to_local_face =
        volume_adjacency.element_to_local_face_buffer()->get_device_ptr<int>();
    const float* Dm_inv_ptr = Dm_inv->get_device_ptr<float>();
    const float* volumes_ptr = volumes->get_device_ptr<float>();
    const int* value_positions =
        csr_structure.element_value_positions->get_device_ptr<int>();

    cuda::GPUParallelFor(
        "fill_hessian_values_nh", num_elements, [=] __device__(int elem_idx) {
            // Use precomputed mapping
            int v_apex = element_to_vertex[elem_idx];
            int local_face_idx = element_to_local_face[elem_idx];

            unsigned offset = offsets[v_apex];
            unsigned face_a = adjacency[offset + 1 + local_face_idx * 3 + 0];
            unsigned face_b = adjacency[offset + 1 + local_face_idx * 3 + 1];
            unsigned face_c = adjacency[offset + 1 + local_face_idx * 3 + 2];
            int tet[4] = { v_apex, (int)face_a, (int)face_b, (int)face_c };
            const float* Dm_inv_local = &Dm_inv_ptr[elem_idx * 9];
            float volume = volumes_ptr[elem_idx];

            Eigen::Matrix<float, 12, 12> K_elem;
            compute_element_hessian(
                x_curr_ptr, tet, Dm_inv_local, volume, mu, lambda, K_elem);

            // Scale by dt² (same as mass-spring)
            K_elem *= (dt * dt);

            // Write to CSR values array
            // CRITICAL: Eigen uses column-major order, but we need row-major
            // for CSR K_elem.data()[i] gives column-major, but positions[]
            // expects row-major For symmetric matrices: K_elem(row, col) should
            // equal K_elem(col, row)
            const int* positions = &value_positions[elem_idx * 144];
            for (int row = 0; row < 12; row++) {
                for (int col = 0; col < 12; col++) {
                    int idx = row * 12 + col;  // Row-major index
                    int pos = positions[idx];
                    if (pos >= 0) {
                        // Use (row, col) to access Eigen matrix correctly
                        // For symmetric matrix, K(row,col) should already equal
                        // K(col,row) due to the symmetric construction in
                        // compute_element_hessian
                        atomicAdd(&values_ptr[pos], K_elem(row, col));
                    }
                }
            }
        });
}

// ============================================================================
// Energy Computation
// ============================================================================

// Compute element energy kernel

float compute_energy_nh_gpu(
    cuda::CUDALinearBufferHandle x_curr,
    cuda::CUDALinearBufferHandle x_tilde,
    cuda::CUDALinearBufferHandle M_diag,
    const VolumeAdjacencyMap& volume_adjacency,
    cuda::CUDALinearBufferHandle Dm_inv,
    cuda::CUDALinearBufferHandle volumes,
    float mu,
    float lambda,
    float density,
    float gravity,
    float dt,
    int num_particles,
    int num_elements,
    cuda::CUDALinearBufferHandle d_inertial_terms,
    cuda::CUDALinearBufferHandle d_element_energies)
{
    const int n = num_particles * 3;
    const float dt2 = dt * dt;

    const float* x_ptr = x_curr->get_device_ptr<float>();
    const float* x_tilde_ptr = x_tilde->get_device_ptr<float>();
    const float* M_ptr = M_diag->get_device_ptr<float>();

    float* inertial_ptr = d_inertial_terms->get_device_ptr<float>();
    float* element_energy_ptr = d_element_energies->get_device_ptr<float>();

    // Inertial energy: 1/2 * (x - x_tilde)^T * M * (x - x_tilde)
    cuda::GPUParallelFor(
        "compute_inertial_energy_nh", n, [=] __device__(int i) {
            const float diff = x_ptr[i] - x_tilde_ptr[i];
            inertial_ptr[i] = 0.5f * M_ptr[i] * diff * diff;
        });

    const float E_inertial = thrust::reduce(
        thrust::device,
        thrust::device_ptr<float>(inertial_ptr),
        thrust::device_ptr<float>(inertial_ptr) + n);

    // Elastic + gravitational energy: sum over elements
    cudaMemset(element_energy_ptr, 0, num_elements * sizeof(float));

    const unsigned* adjacency =
        volume_adjacency.adjacency_buffer()->get_device_ptr<unsigned>();
    const unsigned* offsets =
        volume_adjacency.offsets_buffer()->get_device_ptr<unsigned>();
    const int* element_to_vertex =
        volume_adjacency.element_to_vertex_buffer()->get_device_ptr<int>();
    const int* element_to_local_face =
        volume_adjacency.element_to_local_face_buffer()->get_device_ptr<int>();
    const float* Dm_inv_ptr = Dm_inv->get_device_ptr<float>();
    const float* volumes_ptr = volumes->get_device_ptr<float>();

    cuda::GPUParallelFor(
        "compute_element_energy", num_elements, [=] __device__(int elem_idx) {
            // Use precomputed mapping
            int v_apex = element_to_vertex[elem_idx];
            int local_face_idx = element_to_local_face[elem_idx];

            unsigned offset = offsets[v_apex];
            int tet[4];
            tet[0] = v_apex;
            tet[1] = adjacency[offset + 1 + local_face_idx * 3 + 0];
            tet[2] = adjacency[offset + 1 + local_face_idx * 3 + 1];
            tet[3] = adjacency[offset + 1 + local_face_idx * 3 + 2];

            const float* Dm_inv_local = &Dm_inv_ptr[elem_idx * 9];
            float volume = volumes_ptr[elem_idx];

            Eigen::Matrix3f F;
            compute_deformation_gradient(x_ptr, tet, Dm_inv_local, F);

            // Elastic energy density
            float psi = neo_hookean_energy_density(F, mu, lambda);

            // Gravitational potential energy: U_g = -density * volume * gravity
            // * z_center
            float z_center =
                0.25f * (x_ptr[tet[0] * 3 + 2] + x_ptr[tet[1] * 3 + 2] +
                         x_ptr[tet[2] * 3 + 2] + x_ptr[tet[3] * 3 + 2]);
            float gravitational_potential =
                -density * volume * gravity * z_center;

            // Total energy: elastic + gravitational
            element_energy_ptr[elem_idx] =
                volume * psi + gravitational_potential;
        });

    const float E_combined = thrust::reduce(
        thrust::device,
        thrust::device_ptr<float>(element_energy_ptr),
        thrust::device_ptr<float>(element_energy_ptr) + num_elements);

    // Total implicit Euler energy: E = 1/2*(x-x̃)^T*M*(x-x̃) + dt²*(Ψ_elastic +
    // U_gravity)
    return E_inertial + dt2 * E_combined;
}

// ============================================================================
// Reference Data Computation
// ============================================================================

// Compute Dm_inv kernel

std::tuple<
    cuda::CUDALinearBufferHandle,
    cuda::CUDALinearBufferHandle,
    cuda::CUDALinearBufferHandle,
    cuda::CUDALinearBufferHandle>
compute_reference_data_gpu(
    cuda::CUDALinearBufferHandle positions,
    cuda::CUDALinearBufferHandle adjacency,
    cuda::CUDALinearBufferHandle offsets,
    int num_elements)
{
    // num_elements is the unique number of tetrahedra (total_face_counts / 4)
    // We need to select ONE representative element per tetrahedron
    // Strategy: For each tetrahedron, use the vertex with MINIMUM index as apex

    auto Dm_inv = cuda::create_cuda_linear_buffer<float>(num_elements * 9);
    auto volumes = cuda::create_cuda_linear_buffer<float>(num_elements);
    auto element_to_vertex = cuda::create_cuda_linear_buffer<int>(num_elements);
    auto element_to_local_face =
        cuda::create_cuda_linear_buffer<int>(num_elements);

    int block_size = 256;
    int num_vertices = offsets->getDesc().element_count;

    const unsigned* adj_ptr = adjacency->get_device_ptr<unsigned>();
    const unsigned* off_ptr = offsets->get_device_ptr<unsigned>();
    int* elem_to_v_ptr = element_to_vertex->get_device_ptr<int>();
    int* elem_to_lf_ptr = element_to_local_face->get_device_ptr<int>();

    // Atomic counter for unique elements
    auto element_counter = cuda::create_cuda_linear_buffer<int>(1);
    cudaMemset(
        reinterpret_cast<void*>(element_counter->get_device_ptr()),
        0,
        sizeof(int));
    int* counter_ptr = element_counter->get_device_ptr<int>();

    // For each vertex, check all its opposite faces
    // Only create element if this vertex has the MINIMUM index among the 4
    // vertices
    cuda::GPUParallelFor(
        "select_unique_elements", num_vertices, [=] __device__(int v) {
            unsigned offset = off_ptr[v];
            unsigned count = adj_ptr[offset];

            for (unsigned local_idx = 0; local_idx < count; local_idx++) {
                unsigned v0 = adj_ptr[offset + 1 + local_idx * 3 + 0];
                unsigned v1 = adj_ptr[offset + 1 + local_idx * 3 + 1];
                unsigned v2 = adj_ptr[offset + 1 + local_idx * 3 + 2];

                // Check if v is the minimum among {v, v0, v1, v2}
                bool is_min = (v < v0) && (v < v1) && (v < v2);

                if (is_min) {
                    // This is the unique representative for this tetrahedron
                    int elem_idx = atomicAdd(counter_ptr, 1);
                    elem_to_v_ptr[elem_idx] = v;
                    elem_to_lf_ptr[elem_idx] = local_idx;
                }
            }
        });
    cudaDeviceSynchronize();

    // Verify we got exactly num_elements
    int actual_count = 0;
    cudaMemcpy(&actual_count, counter_ptr, sizeof(int), cudaMemcpyDeviceToHost);
    if (actual_count != num_elements) {
        printf(
            "[NeoHookean ERROR] Expected %d elements but got %d unique "
            "elements!\n",
            num_elements,
            actual_count);
    }

    // Then compute Dm_inv using the selected elements
    const float* positions_ptr = positions->get_device_ptr<float>();
    const unsigned* adjacency_ptr = adjacency->get_device_ptr<unsigned>();
    const unsigned* offsets_ptr = offsets->get_device_ptr<unsigned>();
    const int* element_to_vertex_ptr = element_to_vertex->get_device_ptr<int>();
    const int* element_to_local_face_ptr =
        element_to_local_face->get_device_ptr<int>();
    float* Dm_inv_ptr = Dm_inv->get_device_ptr<float>();
    float* volumes_ptr = volumes->get_device_ptr<float>();

    cuda::GPUParallelFor(
        "compute_Dm_inv", num_elements, [=] __device__(int elem_idx) {
            // Use precomputed mapping
            int v_apex = element_to_vertex_ptr[elem_idx];
            int local_face_idx = element_to_local_face_ptr[elem_idx];

            // Extract face vertices
            unsigned offset = offsets_ptr[v_apex];
            unsigned face_a =
                adjacency_ptr[offset + 1 + local_face_idx * 3 + 0];
            unsigned face_b =
                adjacency_ptr[offset + 1 + local_face_idx * 3 + 1];
            unsigned face_c =
                adjacency_ptr[offset + 1 + local_face_idx * 3 + 2];

            // Tetrahedron vertices: apex + face (v_apex, face_a, face_b,
            // face_c)
            int tet[4] = { v_apex, (int)face_a, (int)face_b, (int)face_c };

            // Get rest positions
            Eigen::Vector3f x0(
                positions_ptr[tet[0] * 3 + 0],
                positions_ptr[tet[0] * 3 + 1],
                positions_ptr[tet[0] * 3 + 2]);
            Eigen::Vector3f x1(
                positions_ptr[tet[1] * 3 + 0],
                positions_ptr[tet[1] * 3 + 1],
                positions_ptr[tet[1] * 3 + 2]);
            Eigen::Vector3f x2(
                positions_ptr[tet[2] * 3 + 0],
                positions_ptr[tet[2] * 3 + 1],
                positions_ptr[tet[2] * 3 + 2]);
            Eigen::Vector3f x3(
                positions_ptr[tet[3] * 3 + 0],
                positions_ptr[tet[3] * 3 + 1],
                positions_ptr[tet[3] * 3 + 2]);

            // Compute Dm = [x1-x0, x2-x0, x3-x0]
            Eigen::Matrix3f Dm;
            Dm.col(0) = x1 - x0;
            Dm.col(1) = x2 - x0;
            Dm.col(2) = x3 - x0;

            // Check orientation: det(Dm) should be positive
            float det_Dm = Dm.determinant();

            // If negative orientation, swap vertices 2 and 3 to flip
            // orientation
            if (det_Dm < 0.0f) {
                // Swap x2 and x3
                Eigen::Vector3f temp = x2;
                x2 = x3;
                x3 = temp;

                // Swap in tet array for consistency
                int temp_idx = tet[2];
                tet[2] = tet[3];
                tet[3] = temp_idx;

                // Recompute Dm with corrected orientation
                Dm.col(0) = x1 - x0;
                Dm.col(1) = x2 - x0;
                Dm.col(2) = x3 - x0;

                det_Dm = Dm.determinant();
            }

            // Compute volume: V = det(Dm) / 6 (now guaranteed positive)
            float volume = det_Dm / 6.0f;

            // Sanity check: volume should be positive
            if (volume <= 1e-12f) {
                // Degenerate tetrahedron
                volume = 1e-10f;
            }

            volumes_ptr[elem_idx] = volume;

            // Compute inverse (Dm is now properly oriented)
            Eigen::Matrix3f Dm_inv_mat = Dm.inverse();

            // Store in column-major order (Eigen's default, matches loading
            // format)
            float* Dm_inv_local = &Dm_inv_ptr[elem_idx * 9];
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) {
                    Dm_inv_local[j * 3 + i] = Dm_inv_mat(i, j);  // Column-major
                }
            }
        });

    return std::make_tuple(
        Dm_inv, volumes, element_to_vertex, element_to_local_face);
}

// Compute only Dm_inv and volumes (Neo-Hookean specific reference data)
// Assumes element_to_vertex and element_to_local_face are already computed
std::tuple<cuda::CUDALinearBufferHandle, cuda::CUDALinearBufferHandle>
compute_nh_reference_data_gpu(
    cuda::CUDALinearBufferHandle positions,
    const VolumeAdjacencyMap& volume_adjacency,
    int num_elements)
{
    auto Dm_inv = cuda::create_cuda_linear_buffer<float>(num_elements * 9);
    auto volumes = cuda::create_cuda_linear_buffer<float>(num_elements);

    const float* positions_ptr = positions->get_device_ptr<float>();
    const unsigned* adjacency =
        volume_adjacency.adjacency_buffer()->get_device_ptr<unsigned>();
    const unsigned* offsets =
        volume_adjacency.offsets_buffer()->get_device_ptr<unsigned>();
    const int* element_to_vertex =
        volume_adjacency.element_to_vertex_buffer()->get_device_ptr<int>();
    const int* element_to_local_face =
        volume_adjacency.element_to_local_face_buffer()->get_device_ptr<int>();
    float* Dm_inv_ptr = Dm_inv->get_device_ptr<float>();
    float* volumes_ptr = volumes->get_device_ptr<float>();

    cuda::GPUParallelFor(
        "compute_Dm_inv", num_elements, [=] __device__(int elem_idx) {
            // Use precomputed mapping
            int v_apex = element_to_vertex[elem_idx];
            int local_face_idx = element_to_local_face[elem_idx];

            // Extract face vertices
            unsigned offset = offsets[v_apex];
            unsigned face_a = adjacency[offset + 1 + local_face_idx * 3 + 0];
            unsigned face_b = adjacency[offset + 1 + local_face_idx * 3 + 1];
            unsigned face_c = adjacency[offset + 1 + local_face_idx * 3 + 2];

            // Tetrahedron vertices: apex + face (v_apex, face_a, face_b,
            // face_c)
            int tet[4] = { v_apex, (int)face_a, (int)face_b, (int)face_c };

            // Get rest positions
            Eigen::Vector3f x0(
                positions_ptr[tet[0] * 3 + 0],
                positions_ptr[tet[0] * 3 + 1],
                positions_ptr[tet[0] * 3 + 2]);
            Eigen::Vector3f x1(
                positions_ptr[tet[1] * 3 + 0],
                positions_ptr[tet[1] * 3 + 1],
                positions_ptr[tet[1] * 3 + 2]);
            Eigen::Vector3f x2(
                positions_ptr[tet[2] * 3 + 0],
                positions_ptr[tet[2] * 3 + 1],
                positions_ptr[tet[2] * 3 + 2]);
            Eigen::Vector3f x3(
                positions_ptr[tet[3] * 3 + 0],
                positions_ptr[tet[3] * 3 + 1],
                positions_ptr[tet[3] * 3 + 2]);

            // Compute Dm = [x1-x0, x2-x0, x3-x0]
            Eigen::Matrix3f Dm;
            Dm.col(0) = x1 - x0;
            Dm.col(1) = x2 - x0;
            Dm.col(2) = x3 - x0;

            // Check orientation: det(Dm) should be positive
            float det_Dm = Dm.determinant();

            // If negative orientation, swap vertices 2 and 3 to flip
            // orientation
            if (det_Dm < 0.0f) {
                // Swap x2 and x3
                Eigen::Vector3f temp = x2;
                x2 = x3;
                x3 = temp;

                // Swap in tet array for consistency
                int temp_idx = tet[2];
                tet[2] = tet[3];
                tet[3] = temp_idx;

                // Recompute Dm with corrected orientation
                Dm.col(0) = x1 - x0;
                Dm.col(1) = x2 - x0;
                Dm.col(2) = x3 - x0;

                det_Dm = Dm.determinant();
            }

            // Compute volume: V = det(Dm) / 6 (now guaranteed positive)
            float volume = det_Dm / 6.0f;

            // Sanity check: volume should be positive
            if (volume <= 1e-12f) {
                // Degenerate tetrahedron
                volume = 1e-10f;
            }

            volumes_ptr[elem_idx] = volume;

            // Compute inverse (Dm is now properly oriented)
            Eigen::Matrix3f Dm_inv_mat = Dm.inverse();

            // Store in column-major order (Eigen's default, matches loading
            // format)
            float* Dm_inv_local = &Dm_inv_ptr[elem_idx * 9];
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) {
                    Dm_inv_local[j * 3 + i] = Dm_inv_mat(i, j);  // Column-major
                }
            }
        });

    return std::make_tuple(Dm_inv, volumes);
}

// ============================================================================
// Vector Operations
// ============================================================================

void negate_nh_gpu(
    cuda::CUDALinearBufferHandle input,
    cuda::CUDALinearBufferHandle output,
    int size)
{
    const float* in_ptr = input->get_device_ptr<float>();
    float* out_ptr = output->get_device_ptr<float>();

    cuda::GPUParallelFor(
        "negate_nh", size, [=] __device__(int i) { out_ptr[i] = -in_ptr[i]; });
}

void axpy_nh_gpu(
    float alpha,
    cuda::CUDALinearBufferHandle x,
    cuda::CUDALinearBufferHandle y,
    cuda::CUDALinearBufferHandle result,
    int size)
{
    const float* x_ptr = x->get_device_ptr<float>();
    const float* y_ptr = y->get_device_ptr<float>();
    float* result_ptr = result->get_device_ptr<float>();

    cuda::GPUParallelFor("axpy_nh", size, [=] __device__(int i) {
        result_ptr[i] = alpha * x_ptr[i] + y_ptr[i];
    });
}

void scale_vector_gpu(cuda::CUDALinearBufferHandle vec, float scale, int size)
{
    float* vec_ptr = vec->get_device_ptr<float>();

    cuda::GPUParallelFor(
        "scale_vector", size, [=] __device__(int i) { vec_ptr[i] *= scale; });
}

struct square_op_nh {
    __device__ float operator()(float x) const
    {
        return x * x;
    }
};

float compute_vector_norm_nh_gpu(cuda::CUDALinearBufferHandle vec, int size)
{
    float* vec_ptr = vec->get_device_ptr<float>();
    thrust::device_ptr<float> d_vec(vec_ptr);

    float sum_sq = thrust::transform_reduce(
        thrust::device,
        d_vec,
        d_vec + size,
        square_op_nh(),
        0.0f,
        ::cuda::std::plus<float>());

    return sqrtf(sum_sq);
}

// Update velocities: v = (x_new - x_old) / dt * damping
void update_velocities_nh_gpu(
    cuda::CUDALinearBufferHandle x_new,
    cuda::CUDALinearBufferHandle x_old,
    float dt,
    float damping,
    int num_particles,
    cuda::CUDALinearBufferHandle velocities)
{
    const float* x_new_ptr = x_new->get_device_ptr<float>();
    const float* x_old_ptr = x_old->get_device_ptr<float>();
    float* v_ptr = velocities->get_device_ptr<float>();

    float inv_dt_damped = damping / dt;

    cuda::GPUParallelFor(
        "update_velocities_nh", num_particles * 3, [=] __device__(int i) {
            v_ptr[i] = (x_new_ptr[i] - x_old_ptr[i]) * inv_dt_damped;
        });
}

// Handle ground collision (z = 0) with restitution
void handle_ground_collision_nh_gpu(
    cuda::CUDALinearBufferHandle positions,
    cuda::CUDALinearBufferHandle velocities,
    float restitution,
    int num_particles)
{
    float* pos_ptr = positions->get_device_ptr<float>();
    float* vel_ptr = velocities->get_device_ptr<float>();

    cuda::GPUParallelFor(
        "handle_ground_collision_nh", num_particles, [=] __device__(int i) {
            int z_idx = i * 3 + 2;
            if (pos_ptr[z_idx] < 0.0f) {
                pos_ptr[z_idx] = 0.0f;

                // Reflect velocity with restitution
                if (vel_ptr[z_idx] < 0.0f) {
                    vel_ptr[z_idx] = -vel_ptr[z_idx] * restitution;
                }
            }
        });
}

// ============================================================================
// Dirichlet Boundary Conditions
// ============================================================================

// Apply Dirichlet boundary conditions to Hessian matrix (CSR format)
// For each BC DOF i:
// - Set row i: H[i,i] = 1, all other entries in row i = 0
// - Set column i: H[j,i] = 0 for all j != i
void apply_dirichlet_bc_to_hessian_gpu(
    const NeoHookeanCSRStructure& csr_structure,
    cuda::CUDALinearBufferHandle bc_dofs,
    int num_bc_dofs,
    cuda::CUDALinearBufferHandle values)
{
    if (num_bc_dofs == 0)
        return;

    const int* bc_dofs_ptr = bc_dofs->get_device_ptr<int>();
    const int* row_offsets_ptr =
        csr_structure.row_offsets->get_device_ptr<int>();
    const int* col_indices_ptr =
        csr_structure.col_indices->get_device_ptr<int>();
    float* values_ptr = values->get_device_ptr<float>();

    int num_rows = csr_structure.num_rows;

    // Step 1: Mark BC DOFs in a boolean array for fast lookup
    thrust::device_vector<bool> is_bc_dof(num_rows, false);
    bool* is_bc_ptr = thrust::raw_pointer_cast(is_bc_dof.data());

    cuda::GPUParallelFor("mark_bc_dofs", num_bc_dofs, [=] __device__(int i) {
        int dof = bc_dofs_ptr[i];
        if (dof >= 0 && dof < num_rows) {
            is_bc_ptr[dof] = true;
        }
    });

    // Step 2: Process each row
    cuda::GPUParallelFor("apply_bc_to_rows", num_rows, [=] __device__(int row) {
        int row_start = row_offsets_ptr[row];
        int row_end = row_offsets_ptr[row + 1];

        if (is_bc_ptr[row]) {
            // This row corresponds to a BC DOF
            // Set diagonal to 1, all other entries to 0
            for (int idx = row_start; idx < row_end; ++idx) {
                int col = col_indices_ptr[idx];
                if (col == row) {
                    values_ptr[idx] = 1.0f;  // Diagonal entry
                }
                else {
                    values_ptr[idx] = 0.0f;  // Off-diagonal entries
                }
            }
        }
        else {
            // This row corresponds to a free DOF
            // Zero out entries in columns that are BC DOFs
            for (int idx = row_start; idx < row_end; ++idx) {
                int col = col_indices_ptr[idx];
                if (is_bc_ptr[col]) {
                    values_ptr[idx] = 0.0f;
                }
            }
        }
    });
}

// Apply Dirichlet boundary conditions to gradient vector
// Set gradient to zero for BC DOFs
void apply_dirichlet_bc_to_gradient_gpu(
    cuda::CUDALinearBufferHandle bc_dofs,
    int num_bc_dofs,
    cuda::CUDALinearBufferHandle gradient)
{
    if (num_bc_dofs == 0)
        return;

    const int* bc_dofs_ptr = bc_dofs->get_device_ptr<int>();
    float* grad_ptr = gradient->get_device_ptr<float>();

    cuda::GPUParallelFor(
        "apply_bc_to_gradient", num_bc_dofs, [=] __device__(int i) {
            int dof = bc_dofs_ptr[i];
            grad_ptr[dof] = 0.0f;
        });
}

// Zero out Newton direction for BC DOFs
void apply_dirichlet_bc_to_direction_gpu(
    cuda::CUDALinearBufferHandle bc_dofs,
    int num_bc_dofs,
    cuda::CUDALinearBufferHandle direction)
{
    if (num_bc_dofs == 0)
        return;

    const int* bc_dofs_ptr = bc_dofs->get_device_ptr<int>();
    float* dir_ptr = direction->get_device_ptr<float>();

    cuda::GPUParallelFor(
        "apply_bc_to_direction", num_bc_dofs, [=] __device__(int i) {
            int dof = bc_dofs_ptr[i];
            dir_ptr[dof] = 0.0f;
        });
}

// Add values to Hessian diagonal elements (CSR format)
void add_to_hessian_diagonal_gpu(
    const NeoHookeanCSRStructure& csr_structure,
    cuda::CUDALinearBufferHandle diagonal_additions,
    int num_dof,
    cuda::CUDALinearBufferHandle values)
{
    const float* diag_add_ptr = diagonal_additions->get_device_ptr<float>();
    const int* row_offsets_ptr =
        csr_structure.row_offsets->get_device_ptr<int>();
    const int* col_indices_ptr =
        csr_structure.col_indices->get_device_ptr<int>();
    float* values_ptr = values->get_device_ptr<float>();

    // For each row, find the diagonal element and add the contribution
    cuda::GPUParallelFor("add_to_diagonal", num_dof, [=] __device__(int row) {
        int row_start = row_offsets_ptr[row];
        int row_end = row_offsets_ptr[row + 1];

        // Find diagonal element (col == row)
        for (int idx = row_start; idx < row_end; ++idx) {
            int col = col_indices_ptr[idx];
            if (col == row) {
                // Found diagonal element
                values_ptr[idx] += diag_add_ptr[row];
                break;
            }
        }
    });
}

}  // namespace rzsim_cuda

RUZINO_NAMESPACE_CLOSE_SCOPE
