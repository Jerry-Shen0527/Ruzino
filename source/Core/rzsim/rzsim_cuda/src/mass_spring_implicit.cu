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
#include <cstddef>

#include "rzsim_cuda/adjacency_map.cuh"
#include "rzsim_cuda/mass_spring_implicit.cuh"

RUZINO_NAMESPACE_OPEN_SCOPE

namespace rzsim_cuda {

void explicit_step_gpu(
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
        "explicit_step", num_particles, GPU_LAMBDA_Ex(int i) {
            x_tilde_ptr[i * 3 + 0] = x_ptr[i * 3 + 0] + dt * v_ptr[i * 3 + 0];
            x_tilde_ptr[i * 3 + 1] = x_ptr[i * 3 + 1] + dt * v_ptr[i * 3 + 1];
            x_tilde_ptr[i * 3 + 2] = x_ptr[i * 3 + 2] + dt * v_ptr[i * 3 + 2];
        });
}

void setup_external_forces_gpu(
    float mass,
    float gravity,
    int num_particles,
    cuda::CUDALinearBufferHandle f_ext)
{
    float* f_ext_ptr = f_ext->get_device_ptr<float>();

    cuda::GPUParallelFor(
        "setup_forces", num_particles, GPU_LAMBDA_Ex(int i) {
            f_ext_ptr[i * 3 + 0] = 0.0f;
            f_ext_ptr[i * 3 + 1] = 0.0f;
            f_ext_ptr[i * 3 + 2] = mass * gravity;
        });
}

// Gradient kernel using adjacency list
__global__ void compute_gradient_kernel_adjacency(
    const float* x_curr,
    const float* x_tilde,
    const float* M_diag,
    const float* f_ext,
    const int* adjacent_vertices,
    const int* vertex_offsets,
    const float* rest_lengths,
    float stiffness,
    float dt,
    int num_particles,
    float* grad)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= num_particles)
        return;

    // Initialize with inertial term: M * (x - x_tilde)
    grad[i * 3 + 0] =
        M_diag[i * 3 + 0] * (x_curr[i * 3 + 0] - x_tilde[i * 3 + 0]);
    grad[i * 3 + 1] =
        M_diag[i * 3 + 1] * (x_curr[i * 3 + 1] - x_tilde[i * 3 + 1]);
    grad[i * 3 + 2] =
        M_diag[i * 3 + 2] * (x_curr[i * 3 + 2] - x_tilde[i * 3 + 2]);

    // Add spring forces - iterate over adjacent vertices
    int start = vertex_offsets[i];
    int end = vertex_offsets[i + 1];

    for (int idx = start; idx < end; ++idx) {
        int j = adjacent_vertices[idx];
        float l0 = rest_lengths[idx];
        float l0_sq = l0 * l0;

        float dx = x_curr[i * 3 + 0] - x_curr[j * 3 + 0];
        float dy = x_curr[i * 3 + 1] - x_curr[j * 3 + 1];
        float dz = x_curr[i * 3 + 2] - x_curr[j * 3 + 2];
        float diff_sq = dx * dx + dy * dy + dz * dz;

        float factor = 2.0f * stiffness * (diff_sq / l0_sq - 1.0f) * dt * dt;

        // Spring force on vertex i
        grad[i * 3 + 0] += factor * dx;
        grad[i * 3 + 1] += factor * dy;
        grad[i * 3 + 2] += factor * dz;
    }

    // Subtract external forces
    grad[i * 3 + 0] -= dt * dt * f_ext[i * 3 + 0];
    grad[i * 3 + 1] -= dt * dt * f_ext[i * 3 + 1];
    grad[i * 3 + 2] -= dt * dt * f_ext[i * 3 + 2];
}

void compute_gradient_gpu(
    cuda::CUDALinearBufferHandle x_curr,
    cuda::CUDALinearBufferHandle x_tilde,
    cuda::CUDALinearBufferHandle M_diag,
    cuda::CUDALinearBufferHandle f_ext,
    cuda::CUDALinearBufferHandle adjacent_vertices,
    cuda::CUDALinearBufferHandle vertex_offsets,
    cuda::CUDALinearBufferHandle rest_lengths,
    float stiffness,
    float dt,
    int num_particles,
    cuda::CUDALinearBufferHandle grad)
{
    int block_size = 256;
    int num_blocks = (num_particles + block_size - 1) / block_size;

    compute_gradient_kernel_adjacency<<<num_blocks, block_size>>>(
        x_curr->get_device_ptr<float>(),
        x_tilde->get_device_ptr<float>(),
        M_diag->get_device_ptr<float>(),
        f_ext->get_device_ptr<float>(),
        adjacent_vertices->get_device_ptr<int>(),
        vertex_offsets->get_device_ptr<int>(),
        rest_lengths->get_device_ptr<float>(),
        stiffness,
        dt,
        num_particles,
        grad->get_device_ptr<float>());

    cudaDeviceSynchronize();
}

// Custom 3x3 symmetric eigenvalue decomposition using Jacobi rotation
// This is needed because Eigen's SelfAdjointEigenSolver doesn't work on CUDA
// device
__device__ void eigen_decomposition_3x3(
    const Eigen::Matrix3f& A,
    Eigen::Vector3f& eigenvalues,
    Eigen::Matrix3f& eigenvectors)
{
    const int MAX_ITER = 50;
    const float EPSILON = 1e-10f;

    // Initialize eigenvectors as identity
    eigenvectors.setIdentity();

    // Copy A to working matrix
    Eigen::Matrix3f a = A;

    // Jacobi rotation
    for (int iter = 0; iter < MAX_ITER; iter++) {
        // Find largest off-diagonal element
        float max_offdiag = 0.0f;
        int p = 0, q = 1;

        for (int i = 0; i < 3; i++) {
            for (int j = i + 1; j < 3; j++) {
                float abs_aij = fabsf(a(i, j));
                if (abs_aij > max_offdiag) {
                    max_offdiag = abs_aij;
                    p = i;
                    q = j;
                }
            }
        }

        // Check convergence
        if (max_offdiag < EPSILON) {
            break;
        }

        // Compute rotation angle
        float diff = a(q, q) - a(p, p);
        float theta = 0.5f * atan2f(2.0f * a(p, q), diff);
        float c = cosf(theta);
        float s = sinf(theta);

        // Apply rotation to a: a = J^T * a * J
        Eigen::Matrix3f J;
        J.setIdentity();
        J(p, p) = c;
        J(q, q) = c;
        J(p, q) = s;
        J(q, p) = -s;

        a = J.transpose() * a * J;

        // Accumulate eigenvectors
        eigenvectors = eigenvectors * J;
    }

    // Extract eigenvalues from diagonal
    eigenvalues(0) = a(0, 0);
    eigenvalues(1) = a(1, 1);
    eigenvalues(2) = a(2, 2);
}

// Custom PSD projection for 3x3 symmetric matrix
__device__ Eigen::Matrix3f project_psd_custom(const Eigen::Matrix3f& H)
{
    Eigen::Vector3f eigenvalues;
    Eigen::Matrix3f eigenvectors;

    // Compute eigendecomposition
    eigen_decomposition_3x3(H, eigenvalues, eigenvectors);

    // Clamp negative eigenvalues to zero
    for (int i = 0; i < 3; i++) {
        if (eigenvalues(i) < 0.0f) {
            eigenvalues(i) = 0.0f;
        }
    }

    // Reconstruct: H_psd = V * Lambda * V^T
    Eigen::Matrix3f result =
        eigenvectors * eigenvalues.asDiagonal() * eigenvectors.transpose();

    return result;
}

// ============================================================================
// NEW: Zero-sort direct-fill implementation
// ============================================================================

// Binary search helper for finding (row, col) in sorted unique arrays
__device__ int find_entry_position(
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
        int mid_row = unique_rows[mid];
        int mid_col = unique_cols[mid];

        if (mid_row == target_row && mid_col == target_col) {
            return mid;
        }

        // Compare as (row, col) pairs
        if (mid_row < target_row ||
            (mid_row == target_row && mid_col < target_col)) {
            left = mid + 1;
        }
        else {
            right = mid - 1;
        }
    }

    return -1;  // Not found (should never happen for valid structure)
}

// Build CSR sparsity pattern once during initialization (GPU version)
CSRStructure build_hessian_structure_gpu(
    cuda::CUDALinearBufferHandle adjacent_vertices,
    cuda::CUDALinearBufferHandle vertex_offsets,
    int num_particles)
{
    CSRStructure structure;
    int n = num_particles * 3;

    const int* adj_ptr = adjacent_vertices->get_device_ptr<int>();
    const int* offsets_ptr = vertex_offsets->get_device_ptr<int>();

    // Count total edges (j > i) using thrust::transform_reduce
    auto counting_iter = thrust::make_counting_iterator(0);
    int num_edges = thrust::transform_reduce(
        thrust::device,
        counting_iter,
        counting_iter + num_particles,
        [adj_ptr, offsets_ptr] __device__(int i) -> int {
            int start = offsets_ptr[i];
            int end = offsets_ptr[i + 1];
            int count = 0;
            for (int idx = start; idx < end; idx++) {
                if (adj_ptr[idx] > i)
                    count++;
            }
            return count;
        },
        0,
        ::cuda::std::plus<int>());

    int num_mass_entries = n;
    int num_spring_entries = num_edges * 36;
    int total_entries = num_mass_entries + num_spring_entries;

    // Allocate temporary buffers for all entries
    auto d_all_rows = cuda::create_cuda_linear_buffer<int>(total_entries);
    auto d_all_cols = cuda::create_cuda_linear_buffer<int>(total_entries);
    auto d_write_offset = cuda::create_cuda_linear_buffer<int>(1);
    cudaMemset(d_write_offset->get_device_ptr<int>(), 0, sizeof(int));

    int* rows = d_all_rows->get_device_ptr<int>();
    int* cols = d_all_cols->get_device_ptr<int>();
    int* write_offset = d_write_offset->get_device_ptr<int>();

    // Generate all (row, col) pairs on GPU using GPUParallelFor
    cuda::GPUParallelFor(
        "generate_matrix_entries", num_particles, GPU_LAMBDA_Ex(int i) {
            // First, write mass diagonal entries
            for (int d = 0; d < 3; d++) {
                int dof = i * 3 + d;
                int pos = atomicAdd(write_offset, 1);
                rows[pos] = dof;
                cols[pos] = dof;
            }

            // Then, write spring entries for neighbors with j > i
            int start = offsets_ptr[i];
            int end = offsets_ptr[i + 1];

            for (int idx = start; idx < end; idx++) {
                int j = adj_ptr[idx];
                if (j <= i)
                    continue;

                // Generate 36 entries for this edge (vi, vj)
                for (int block_r = 0; block_r < 2; block_r++) {
                    for (int block_c = 0; block_c < 2; block_c++) {
                        int base_r = (block_r == 0 ? i : j) * 3;
                        int base_c = (block_c == 0 ? i : j) * 3;

                        for (int local_r = 0; local_r < 3; local_r++) {
                            for (int local_c = 0; local_c < 3; local_c++) {
                                int pos = atomicAdd(write_offset, 1);
                                rows[pos] = base_r + local_r;
                                cols[pos] = base_c + local_c;
                            }
                        }
                    }
                }
            }
        });

    // Sort by (row, col) using thrust
    thrust::device_ptr<int> rows_ptr(d_all_rows->get_device_ptr<int>());
    thrust::device_ptr<int> cols_ptr(d_all_cols->get_device_ptr<int>());

    auto zip_begin =
        thrust::make_zip_iterator(thrust::make_tuple(rows_ptr, cols_ptr));

    thrust::sort(
        thrust::device,
        zip_begin,
        zip_begin + total_entries,
        [] __device__(
            const thrust::tuple<int, int>& a,
            const thrust::tuple<int, int>& b) {
            if (thrust::get<0>(a) != thrust::get<0>(b))
                return thrust::get<0>(a) < thrust::get<0>(b);
            return thrust::get<1>(a) < thrust::get<1>(b);
        });

    cudaDeviceSynchronize();

    // Remove duplicates
    auto new_end = thrust::unique(
        thrust::device,
        zip_begin,
        zip_begin + total_entries,
        [] __device__(
            const thrust::tuple<int, int>& a,
            const thrust::tuple<int, int>& b) {
            return thrust::get<0>(a) == thrust::get<0>(b) &&
                   thrust::get<1>(a) == thrust::get<1>(b);
        });

    int nnz = new_end - zip_begin;

    cudaDeviceSynchronize();

    // Get total adjacencies count
    int total_adjacencies;
    cudaMemcpy(
        &total_adjacencies,
        vertex_offsets->get_device_ptr<int>() + num_particles,
        sizeof(int),
        cudaMemcpyDeviceToHost);

    // Allocate CSR arrays
    structure.col_indices = cuda::create_cuda_linear_buffer<int>(nnz);
    structure.row_offsets = cuda::create_cuda_linear_buffer<int>(n + 1);
    structure.mass_value_positions = cuda::create_cuda_linear_buffer<int>(n);
    structure.spring_value_positions =
        cuda::create_cuda_linear_buffer<int>(total_adjacencies * 36);

    // Copy unique columns
    cudaMemcpy(
        structure.col_indices->get_device_ptr<int>(),
        d_all_cols->get_device_ptr<int>(),
        nnz * sizeof(int),
        cudaMemcpyDeviceToDevice);

    // Build row_offsets using histogram
    thrust::device_ptr<int> row_offsets_ptr(
        structure.row_offsets->get_device_ptr<int>());
    thrust::fill(thrust::device, row_offsets_ptr, row_offsets_ptr + n + 1, 0);

    int* row_offsets_raw = structure.row_offsets->get_device_ptr<int>();
    int* unique_rows_raw = d_all_rows->get_device_ptr<int>();

    thrust::for_each(
        thrust::device,
        thrust::make_counting_iterator(0),
        thrust::make_counting_iterator(nnz),
        [row_offsets_raw, unique_rows_raw] __device__(int idx) {
            int row = unique_rows_raw[idx];
            atomicAdd(&row_offsets_raw[row], 1);
        });

    cudaDeviceSynchronize();

    // Compute prefix sum for row offsets
    thrust::exclusive_scan(
        thrust::device,
        row_offsets_ptr,
        row_offsets_ptr + n + 1,
        row_offsets_ptr);

    cudaDeviceSynchronize();

    // Build mass diagonal positions using GPUParallelFor
    const int* unique_rows = d_all_rows->get_device_ptr<int>();
    const int* unique_cols = d_all_cols->get_device_ptr<int>();
    int* mass_positions = structure.mass_value_positions->get_device_ptr<int>();

    cuda::GPUParallelFor(
        "build_mass_positions", n, GPU_LAMBDA_Ex(int i) {
            mass_positions[i] =
                find_entry_position(unique_rows, unique_cols, nnz, i, i);
        });

    // Build spring positions using GPUParallelFor
    int* spring_positions =
        structure.spring_value_positions->get_device_ptr<int>();

    cuda::GPUParallelFor(
        "build_spring_positions", num_particles, GPU_LAMBDA_Ex(int i) {
            int start = offsets_ptr[i];
            int end = offsets_ptr[i + 1];

            for (int idx = start; idx < end; idx++) {
                int j = adj_ptr[idx];
                int base_out = idx * 36;

                if (j <= i) {
                    // Mark as unused
                    for (int k = 0; k < 36; k++) {
                        spring_positions[base_out + k] = -1;
                    }
                    continue;
                }

                // Store 36 positions for this edge
                int count = 0;
                for (int block_r = 0; block_r < 2; block_r++) {
                    for (int block_c = 0; block_c < 2; block_c++) {
                        int vi = (block_r == 0 ? i : j);
                        int vj = (block_c == 0 ? i : j);

                        for (int local_r = 0; local_r < 3; local_r++) {
                            for (int local_c = 0; local_c < 3; local_c++) {
                                int row = vi * 3 + local_r;
                                int col = vj * 3 + local_c;
                                int pos = find_entry_position(
                                    unique_rows, unique_cols, nnz, row, col);
                                spring_positions[base_out + count] = pos;
                                count++;
                            }
                        }
                    }
                }
            }
        });

    structure.num_rows = n;
    structure.num_cols = n;
    structure.nnz = nnz;

    return structure;
}

// Kernel: Directly fill spring Hessian values into CSR using vertex iteration
__global__ void fill_spring_hessian_values_kernel(
    const float* x_curr,
    const int* adjacent_vertices,
    const int* vertex_offsets,
    const float* rest_lengths,
    const int*
        value_positions,  // Pre-computed positions: [total_adjacencies * 36]
    float stiffness,
    float dt,
    int num_particles,
    float* values)  // Output CSR values array
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= num_particles)
        return;

    int start = vertex_offsets[i];
    int end = vertex_offsets[i + 1];

    for (int idx = start; idx < end; idx++) {
        int j = adjacent_vertices[idx];
        if (j <= i)
            continue;  // Only process j > i

        float l0 = rest_lengths[idx];
        if (l0 < 1e-10f)
            continue;

        float k = stiffness;
        float l0_sq = l0 * l0;

        // Get positions
        Eigen::Vector3f xi(x_curr[i * 3], x_curr[i * 3 + 1], x_curr[i * 3 + 2]);
        Eigen::Vector3f xj(x_curr[j * 3], x_curr[j * 3 + 1], x_curr[j * 3 + 2]);
        Eigen::Vector3f diff = xi - xj;
        float diff_sq = diff.squaredNorm();

        // H_diff = 2*k/l0^2 * (2*outer(diff,diff) + (diff_sq - l0^2)*I)
        Eigen::Matrix3f outer = diff * diff.transpose();
        Eigen::Matrix3f H_diff =
            2.0f * k / l0_sq *
            (2.0f * outer + (diff_sq - l0_sq) * Eigen::Matrix3f::Identity());

        // PSD projection
        H_diff = project_psd_custom(H_diff);

        // Scale by dt^2
        float scale = dt * dt;
        H_diff *= scale;

        // Use adjacency index directly for position lookup
        int base_pos = idx * 36;
        int count = 0;

        // 4 blocks: (i,i), (i,j), (j,i), (j,j)
        for (int block_r = 0; block_r < 2; block_r++) {
            for (int block_c = 0; block_c < 2; block_c++) {
                float sign_row = (block_r == 0) ? 1.0f : -1.0f;
                float sign_col = (block_c == 0) ? 1.0f : -1.0f;

                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        float val = H_diff(r, c) * sign_row * sign_col;
                        int pos = value_positions[base_pos + count++];
                        atomicAdd(&values[pos], val);
                    }
                }
            }
        }
    }
}

// Fast update: directly fill values (NO SORTING!)
void update_hessian_values_gpu(
    const CSRStructure& csr_structure,
    cuda::CUDALinearBufferHandle x_curr,
    cuda::CUDALinearBufferHandle M_diag,
    cuda::CUDALinearBufferHandle adjacent_vertices,
    cuda::CUDALinearBufferHandle vertex_offsets,
    cuda::CUDALinearBufferHandle rest_lengths,
    float stiffness,
    float dt,
    int num_particles,
    cuda::CUDALinearBufferHandle values)
{
    int num_dofs = num_particles * 3;

    // Zero out values array
    cudaMemset(
        values->get_device_ptr<float>(), 0, csr_structure.nnz * sizeof(float));

    // Fill mass diagonal
    const float* M_diag_ptr = M_diag->get_device_ptr<float>();
    const int* mass_positions =
        csr_structure.mass_value_positions->get_device_ptr<int>();
    float* values_ptr = values->get_device_ptr<float>();

    cuda::GPUParallelFor(
        "fill_mass_diagonal", num_dofs, GPU_LAMBDA_Ex(int i) {
            float regularization = 1e-6f;
            int pos = mass_positions[i];
            values_ptr[pos] = M_diag_ptr[i] + regularization;
        });

    // Fill spring contributions - manual kernel launch for complex Eigen
    // operations
    int block_size = 256;
    int vertex_blocks = (num_particles + block_size - 1) / block_size;
    fill_spring_hessian_values_kernel<<<vertex_blocks, block_size>>>(
        x_curr->get_device_ptr<float>(),
        adjacent_vertices->get_device_ptr<int>(),
        vertex_offsets->get_device_ptr<int>(),
        rest_lengths->get_device_ptr<float>(),
        csr_structure.spring_value_positions->get_device_ptr<int>(),
        stiffness,
        dt,
        num_particles,
        values->get_device_ptr<float>());

    cudaDeviceSynchronize();
}

// ============================================================================
// End of zero-sort implementation
// ============================================================================

// Kernel to compute spring energy on GPU using adjacency list
__global__ void compute_spring_energy_kernel(
    const float* x_curr,
    const int* adjacent_vertices,
    const int* vertex_offsets,
    const float* rest_lengths,
    float stiffness,
    int num_particles,
    float* spring_energies_per_vertex)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= num_particles)
        return;

    int start = vertex_offsets[i];
    int end = vertex_offsets[i + 1];

    float xi[3] = { x_curr[i * 3], x_curr[i * 3 + 1], x_curr[i * 3 + 2] };

    float total_energy = 0.0f;

    for (int idx = start; idx < end; idx++) {
        int j = adjacent_vertices[idx];

        // Only compute energy for j > i to avoid double counting
        if (j <= i)
            continue;

        float l0 = rest_lengths[idx];
        float l0_sq = l0 * l0;

        // Get neighbor position
        float xj[3] = { x_curr[j * 3], x_curr[j * 3 + 1], x_curr[j * 3 + 2] };

        // Compute squared distance
        float diff[3] = { xi[0] - xj[0], xi[1] - xj[1], xi[2] - xj[2] };
        float diff_sq =
            diff[0] * diff[0] + diff[1] * diff[1] + diff[2] * diff[2];

        // Spring energy matching gradient: 0.5 * k * l0^2 * ((diff_sq / l0^2) -
        // 1)^2
        float ratio = diff_sq / l0_sq - 1.0f;
        float energy = 0.5f * stiffness * l0_sq * ratio * ratio;

        total_energy += energy;
    }

    // Store per-vertex total energy
    spring_energies_per_vertex[i] = total_energy;
}

// Compute total energy: E = 0.5 * M * ||x - x_tilde||^2 + spring_energy -
// f_ext^T * x
float compute_energy_gpu(
    cuda::CUDALinearBufferHandle x_curr,
    cuda::CUDALinearBufferHandle x_tilde,
    cuda::CUDALinearBufferHandle M_diag,
    cuda::CUDALinearBufferHandle f_ext,
    cuda::CUDALinearBufferHandle d_adjacent_vertices,
    cuda::CUDALinearBufferHandle d_vertex_offsets,
    cuda::CUDALinearBufferHandle rest_lengths,
    float stiffness,
    float dt,
    int num_particles,
    cuda::CUDALinearBufferHandle d_inertial_terms,
    cuda::CUDALinearBufferHandle d_spring_energies,
    cuda::CUDALinearBufferHandle d_potential_terms)
{
    int n = num_particles * 3;

    float* x_ptr = reinterpret_cast<float*>(x_curr->get_device_ptr());
    float* x_tilde_ptr = reinterpret_cast<float*>(x_tilde->get_device_ptr());
    float* M_ptr = reinterpret_cast<float*>(M_diag->get_device_ptr());
    float* f_ptr = reinterpret_cast<float*>(f_ext->get_device_ptr());

    // Use pre-allocated buffers instead of creating new ones
    float* inertial_ptr =
        reinterpret_cast<float*>(d_inertial_terms->get_device_ptr());

    cuda::GPUParallelFor(
        "compute_inertial_energy", n, GPU_LAMBDA_Ex(int i) {
            float diff = x_ptr[i] - x_tilde_ptr[i];
            inertial_ptr[i] = 0.5f * M_ptr[i] * diff * diff;
        });

    // Sum inertial energy
    thrust::device_ptr<float> d_inertial_thrust(inertial_ptr);
    float E_inertial =
        thrust::reduce(d_inertial_thrust, d_inertial_thrust + n, 0.0f);

    // Use pre-allocated buffer for spring energies (per-vertex)
    float* spring_energy_ptr =
        reinterpret_cast<float*>(d_spring_energies->get_device_ptr());

    // Zero out spring energies buffer (size = num_particles)
    cudaMemset(spring_energy_ptr, 0, num_particles * sizeof(float));

    int block_size = 256;
    int num_blocks = (num_particles + block_size - 1) / block_size;
    compute_spring_energy_kernel<<<num_blocks, block_size>>>(
        x_ptr,
        reinterpret_cast<const int*>(d_adjacent_vertices->get_device_ptr()),
        reinterpret_cast<const int*>(d_vertex_offsets->get_device_ptr()),
        reinterpret_cast<const float*>(rest_lengths->get_device_ptr()),
        stiffness,
        num_particles,
        spring_energy_ptr);

    cudaDeviceSynchronize();

    // Sum spring energy (over num_particles, not total_adjacencies)
    thrust::device_ptr<float> d_spring_thrust(spring_energy_ptr);
    float E_spring =
        thrust::reduce(d_spring_thrust, d_spring_thrust + num_particles, 0.0f);

    // Use pre-allocated buffer for potential energy
    float* potential_ptr =
        reinterpret_cast<float*>(d_potential_terms->get_device_ptr());

    cuda::GPUParallelFor(
        "compute_potential_energy", n, GPU_LAMBDA_Ex(int i) {
            potential_ptr[i] = -f_ptr[i] * x_ptr[i] * dt * dt;
        });

    thrust::device_ptr<float> d_potential_thrust(potential_ptr);
    float E_potential =
        thrust::reduce(d_potential_thrust, d_potential_thrust + n, 0.0f);

    float total_energy = E_inertial + dt * dt * E_spring + E_potential;

    return total_energy;
}

// Functors for thrust operations (must be defined outside functions for CUDA
// compatibility)
struct square_op {
    __device__ float operator()(float x) const
    {
        return x * x;
    }
};

// GPU vector operations to avoid CPU-GPU transfers
float compute_vector_norm_gpu(cuda::CUDALinearBufferHandle vec, int size)
{
    float* vec_ptr = reinterpret_cast<float*>(vec->get_device_ptr());
    thrust::device_ptr<float> d_vec(vec_ptr);

    // Compute sum of squares using functor
    float sum_sq = thrust::transform_reduce(
        thrust::device,
        d_vec,
        d_vec + size,
        square_op(),
        0.0f,
        ::cuda::std::plus<float>());

    return sqrtf(sum_sq);
}

float compute_dot_product_gpu(
    cuda::CUDALinearBufferHandle vec1,
    cuda::CUDALinearBufferHandle vec2,
    int size)
{
    float* vec1_ptr = reinterpret_cast<float*>(vec1->get_device_ptr());
    float* vec2_ptr = reinterpret_cast<float*>(vec2->get_device_ptr());

    thrust::device_ptr<float> d_vec1(vec1_ptr);
    thrust::device_ptr<float> d_vec2(vec2_ptr);

    return thrust::inner_product(
        thrust::device, d_vec1, d_vec1 + size, d_vec2, 0.0f);
}

void axpy_gpu(
    float alpha,
    cuda::CUDALinearBufferHandle x,
    cuda::CUDALinearBufferHandle y,
    cuda::CUDALinearBufferHandle result,
    int size)
{
    const float* x_ptr = x->get_device_ptr<float>();
    const float* y_ptr = y->get_device_ptr<float>();
    float* result_ptr = result->get_device_ptr<float>();

    cuda::GPUParallelFor(
        "axpy", size, GPU_LAMBDA_Ex(int i) {
            result_ptr[i] = y_ptr[i] + alpha * x_ptr[i];
        });
}

void negate_gpu(
    cuda::CUDALinearBufferHandle in,
    cuda::CUDALinearBufferHandle out,
    int size)
{
    const float* in_ptr = in->get_device_ptr<float>();
    float* out_ptr = out->get_device_ptr<float>();

    cuda::GPUParallelFor(
        "negate", size, GPU_LAMBDA_Ex(int i) { out_ptr[i] = -in_ptr[i]; });
}

void project_to_ground_gpu(
    cuda::CUDALinearBufferHandle positions,
    int num_particles,
    float ground_height)
{
    float* pos_ptr = positions->get_device_ptr<float>();

    cuda::GPUParallelFor(
        "project_to_ground", num_particles, GPU_LAMBDA_Ex(int i) {
            if (pos_ptr[i * 3 + 2] < ground_height) {
                pos_ptr[i * 3 + 2] = ground_height;
            }
        });
}

// Kernel to compute face normals with precomputed face_offsets
__global__ void compute_normals_kernel(
    const float* positions,
    const int* face_vertex_indices,
    const int* face_counts,
    const int* face_offsets,  // Precomputed prefix sum
    int num_faces,
    bool flip_normal,
    float* normals)
{
    int face_id = blockIdx.x * blockDim.x + threadIdx.x;
    if (face_id >= num_faces)
        return;

    int face_start = face_offsets[face_id];
    int face_count = face_counts[face_id];
    if (face_count < 3)
        return;

    // Get first 3 vertices of the face
    int i0 = face_vertex_indices[face_start];
    int i1 = face_vertex_indices[face_start + 1];
    int i2 = face_vertex_indices[face_start + 2];

    // Compute edges
    float e1x = positions[i1 * 3] - positions[i0 * 3];
    float e1y = positions[i1 * 3 + 1] - positions[i0 * 3 + 1];
    float e1z = positions[i1 * 3 + 2] - positions[i0 * 3 + 2];

    float e2x = positions[i2 * 3] - positions[i0 * 3];
    float e2y = positions[i2 * 3 + 1] - positions[i0 * 3 + 1];
    float e2z = positions[i2 * 3 + 2] - positions[i0 * 3 + 2];

    // Compute cross product
    float nx, ny, nz;
    if (flip_normal) {
        nx = e1y * e2z - e1z * e2y;
        ny = e1z * e2x - e1x * e2z;
        nz = e1x * e2y - e1y * e2x;
    }
    else {
        nx = e2y * e1z - e2z * e1y;
        ny = e2z * e1x - e2x * e1z;
        nz = e2x * e1y - e2y * e1x;
    }

    // Normalize
    float length = sqrtf(nx * nx + ny * ny + nz * nz);
    if (length > 1e-8f) {
        nx /= length;
        ny /= length;
        nz /= length;
    }
    else {
        nx = 0.0f;
        ny = 0.0f;
        nz = 1.0f;
    }

    // Write normal for all vertices in this face
    for (int v = 0; v < face_count; v++) {
        int out_idx = (face_start + v) * 3;
        normals[out_idx] = nx;
        normals[out_idx + 1] = ny;
        normals[out_idx + 2] = nz;
    }
}

void compute_normals_gpu(
    cuda::CUDALinearBufferHandle positions,
    cuda::CUDALinearBufferHandle face_vertex_indices,
    cuda::CUDALinearBufferHandle face_counts,
    bool flip_normal,
    cuda::CUDALinearBufferHandle normals)
{
    int num_faces = face_counts->getDesc().element_count;

    // Precompute face offsets using prefix sum (exclusive scan)
    thrust::device_vector<int> face_offsets(num_faces);
    const int* face_counts_ptr = face_counts->get_device_ptr<int>();
    thrust::device_ptr<const int> face_counts_thrust(face_counts_ptr);
    thrust::exclusive_scan(
        thrust::device,
        face_counts_thrust,
        face_counts_thrust + num_faces,
        face_offsets.begin());

    int block_size = 256;
    int num_blocks = (num_faces + block_size - 1) / block_size;

    compute_normals_kernel<<<num_blocks, block_size>>>(
        positions->get_device_ptr<float>(),
        face_vertex_indices->get_device_ptr<int>(),
        face_counts->get_device_ptr<int>(),
        thrust::raw_pointer_cast(face_offsets.data()),
        num_faces,
        flip_normal,
        normals->get_device_ptr<float>());

    cudaDeviceSynchronize();
}

}  // namespace rzsim_cuda

RUZINO_NAMESPACE_CLOSE_SCOPE
