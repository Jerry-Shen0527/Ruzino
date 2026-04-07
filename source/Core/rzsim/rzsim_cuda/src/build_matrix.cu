#include <thrust/device_vector.h>
#include <thrust/execution_policy.h>
#include <thrust/for_each.h>
#include <thrust/scan.h>
#include <thrust/fill.h>

#include <RHI/cuda.hpp>
#include <RHI/rhi.hpp>

#include "rzsim_cuda/build_matrix.cuh"

RUZINO_NAMESPACE_OPEN_SCOPE

namespace rzsim_cuda {

// Triplet format: (row_index, col_index, value)
struct Triplet {
    unsigned row;
    unsigned col;
    float value;
};

// Kernel to generate Laplace matrix triplets (unweighted)
// L = D - A where D is degree matrix (diagonal), A is adjacency
__global__ void build_laplace_triplets_kernel(
    const unsigned* adjacency_list,
    const unsigned* offset_buffer,
    unsigned num_vertices,
    Triplet* triplets,
    unsigned* triplet_count)
{
    unsigned v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= num_vertices)
        return;

    unsigned offset = offset_buffer[v];
    unsigned degree = adjacency_list[offset];

    // Local write position for this thread
    unsigned write_pos = atomicAdd(triplet_count, degree + 1);

    // Write diagonal entry: L[v][v] = degree(v)
    triplets[write_pos] = {v, v, static_cast<float>(degree)};

    // Write off-diagonal entries: L[v][neighbor] = -1 (unweighted)
    for (unsigned i = 0; i < degree; ++i) {
        unsigned neighbor = adjacency_list[offset + 1 + i];
        triplets[write_pos + 1 + i] = {v, neighbor, -1.0f};
    }
}

// Kernel to generate weighted Laplace matrix triplets
// L = D - A where D is weighted degree matrix, A is weighted adjacency
__global__ void build_laplace_triplets_weighted_kernel(
    const unsigned* adjacency_list,
    const unsigned* offset_buffer,
    const float* edge_weights,
    unsigned num_vertices,
    Triplet* triplets,
    unsigned* triplet_count)
{
    unsigned v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= num_vertices)
        return;

    unsigned offset = offset_buffer[v];
    unsigned degree = adjacency_list[offset];

    // Compute weighted degree
    float weighted_degree = 0.0f;
    for (unsigned i = 0; i < degree; ++i) {
        weighted_degree += edge_weights[offset + 1 + i];
    }

    // Local write position for this thread
    unsigned write_pos = atomicAdd(triplet_count, degree + 1);

    // Write diagonal entry: L[v][v] = sum of weights
    triplets[write_pos] = {v, v, weighted_degree};

    // Write off-diagonal entries: L[v][neighbor] = -weight(v, neighbor)
    for (unsigned i = 0; i < degree; ++i) {
        unsigned neighbor = adjacency_list[offset + 1 + i];
        triplets[write_pos + 1 + i] = {v, neighbor, -edge_weights[offset + 1 + i]};
    }
}

cuda::CUDALinearBufferHandle build_laplace_matrix(
    std::tuple<cuda::CUDALinearBufferHandle, cuda::CUDALinearBufferHandle>
        adjacency_map,
    cuda::CUDALinearBufferHandle vertices)
{
    auto adjacency_list_handle = std::get<0>(adjacency_map);
    auto offset_buffer_handle = std::get<1>(adjacency_map);

    auto adjacency_list_ptr = (unsigned*)adjacency_list_handle->get_device_ptr();
    auto offset_buffer_ptr = (unsigned*)offset_buffer_handle->get_device_ptr();

    auto vertex_count = vertices->getDesc().element_count;

    // Estimate total non-zeros: diagonal + all edges (2 entries per undirected edge)
    // Conservative: allocate space for each vertex's degree + 1 diagonal
    thrust::device_vector<unsigned> edge_counts(vertex_count);
    auto edge_counts_ptr = thrust::raw_pointer_cast(edge_counts.data());

    // Count edges per vertex: adjacency_list[offset] = degree
    thrust::for_each(
        thrust::device,
        thrust::counting_iterator<unsigned>(0),
        thrust::counting_iterator<unsigned>(vertex_count),
        [adjacency_list_ptr, offset_buffer_ptr, edge_counts_ptr] __device__(unsigned v) {
            unsigned offset = offset_buffer_ptr[v];
            unsigned degree = adjacency_list_ptr[offset];
            edge_counts_ptr[v] = degree + 1;  // +1 for diagonal
        });

    // Compute total non-zeros
    unsigned total_entries =
        thrust::reduce(thrust::device, edge_counts.begin(), edge_counts.end(), 0u);

    // Allocate triplet buffer
    cuda::CUDALinearBufferDesc triplet_desc;
    triplet_desc.element_count = total_entries;
    triplet_desc.element_size = sizeof(Triplet);

    auto triplet_buffer = cuda::create_cuda_linear_buffer(triplet_desc);
    auto triplets_ptr = (Triplet*)triplet_buffer->get_device_ptr();

    // Counter for actual triplet written
    thrust::device_vector<unsigned> triplet_count(1, 0);

    int threads_per_block = 256;
    int blocks = (vertex_count + threads_per_block - 1) / threads_per_block;

    build_laplace_triplets_kernel<<<blocks, threads_per_block>>>(
        adjacency_list_ptr,
        offset_buffer_ptr,
        vertex_count,
        triplets_ptr,
        thrust::raw_pointer_cast(triplet_count.data()));
    cudaDeviceSynchronize();

    return triplet_buffer;
}

cuda::CUDALinearBufferHandle build_laplace_matrix_weighted(
    std::tuple<cuda::CUDALinearBufferHandle, cuda::CUDALinearBufferHandle>
        adjacency_map,
    cuda::CUDALinearBufferHandle vertices,
    cuda::CUDALinearBufferHandle edge_weights)
{
    auto adjacency_list_handle = std::get<0>(adjacency_map);
    auto offset_buffer_handle = std::get<1>(adjacency_map);

    auto adjacency_list_ptr = (unsigned*)adjacency_list_handle->get_device_ptr();
    auto offset_buffer_ptr = (unsigned*)offset_buffer_handle->get_device_ptr();
    auto edge_weights_ptr = (float*)edge_weights->get_device_ptr();

    auto vertex_count = vertices->getDesc().element_count;

    // Count edges per vertex
    thrust::device_vector<unsigned> edge_counts(vertex_count);
    auto edge_counts_ptr = thrust::raw_pointer_cast(edge_counts.data());

    thrust::for_each(
        thrust::device,
        thrust::counting_iterator<unsigned>(0),
        thrust::counting_iterator<unsigned>(vertex_count),
        [adjacency_list_ptr, offset_buffer_ptr, edge_counts_ptr] __device__(unsigned v) {
            unsigned offset = offset_buffer_ptr[v];
            unsigned degree = adjacency_list_ptr[offset];
            edge_counts_ptr[v] = degree + 1;  // +1 for diagonal
        });

    unsigned total_entries =
        thrust::reduce(thrust::device, edge_counts.begin(), edge_counts.end(), 0u);

    // Allocate triplet buffer
    cuda::CUDALinearBufferDesc triplet_desc;
    triplet_desc.element_count = total_entries;
    triplet_desc.element_size = sizeof(Triplet);

    auto triplet_buffer = cuda::create_cuda_linear_buffer(triplet_desc);
    auto triplets_ptr = (Triplet*)triplet_buffer->get_device_ptr();

    // Counter for actual triplet written
    thrust::device_vector<unsigned> triplet_count(1, 0);

    int threads_per_block = 256;
    int blocks = (vertex_count + threads_per_block - 1) / threads_per_block;

    build_laplace_triplets_weighted_kernel<<<blocks, threads_per_block>>>(
        adjacency_list_ptr,
        offset_buffer_ptr,
        edge_weights_ptr,
        vertex_count,
        triplets_ptr,
        thrust::raw_pointer_cast(triplet_count.data()));
    cudaDeviceSynchronize();

    return triplet_buffer;
}

// Helper device functions for cotangent computation
__device__ inline float dot(float ax, float ay, float az,
                           float bx, float by, float bz)
{
    return ax * bx + ay * by + az * bz;
}

__device__ inline float length(float x, float y, float z)
{
    return sqrtf(x * x + y * y + z * z);
}

__device__ inline float compute_cotangent_angle(
    const float* v0,  // opposite vertex
    const float* v1,  // edge start
    const float* v2)  // edge end
{
    // Vectors from opposite vertex to edge endpoints
    float e1x = v1[0] - v0[0];
    float e1y = v1[1] - v0[1];
    float e1z = v1[2] - v0[2];

    float e2x = v2[0] - v0[0];
    float e2y = v2[1] - v0[1];
    float e2z = v2[2] - v0[2];

    // cos(angle) = dot(e1, e2) / (|e1| * |e2|)
    // sin(angle) = |cross(e1, e2)| / (|e1| * |e2|)
    // cot(angle) = cos / sin

    float dot_prod = dot(e1x, e1y, e1z, e2x, e2y, e2z);

    // Cross product
    float crossx = e1y * e2z - e1z * e2y;
    float crossy = e1z * e2x - e1x * e2z;
    float crossz = e1x * e2y - e1y * e2x;
    float cross_len = length(crossx, crossy, crossz);

    // Avoid division by zero
    if (cross_len < 1e-8f)
        return 0.0f;

    return dot_prod / cross_len;
}

// Kernel to compute cotangent weights for triangle mesh
// For each edge, find adjacent triangles and compute cotangent of opposite angles
__global__ void compute_cotangent_weights_kernel(
    const float* vertices,         // [x0,y0,z0, x1,y1,z1, ...]
    const unsigned* face_indices,  // [v0,v1,v2, v0,v1,v2, ...]
    unsigned num_faces,
    const unsigned* adjacency_list,
    const unsigned* offset_buffer,
    unsigned num_vertices,
    float* edge_weights)           // output: weights in adjacency_list order
{
    unsigned face_id = blockIdx.x * blockDim.x + threadIdx.x;
    if (face_id >= num_faces)
        return;

    // Get triangle vertex indices
    unsigned v0 = face_indices[face_id * 3 + 0];
    unsigned v1 = face_indices[face_id * 3 + 1];
    unsigned v2 = face_indices[face_id * 3 + 2];

    // Process each edge of the triangle
    // Edge (v0, v1): opposite vertex is v2
    // Edge (v1, v2): opposite vertex is v0
    // Edge (v2, v0): opposite vertex is v1

    unsigned edges[3][2] = {{v0, v1}, {v1, v2}, {v2, v0}};
    unsigned opposite[3] = {v2, v0, v1};

    for (int e = 0; e < 3; ++e) {
        unsigned vi = edges[e][0];
        unsigned vj = edges[e][1];
        unsigned vo = opposite[e];

        // Compute cotangent of angle at vo
        const float* p_vo = &vertices[vo * 3];
        const float* p_vi = &vertices[vi * 3];
        const float* p_vj = &vertices[vj * 3];

        float cot = compute_cotangent_angle(p_vo, p_vi, p_vj);

        // For undirected edges, add weight in BOTH directions: (vi, vj) and (vj, vi)

        // Direction 1: Find edge (vi, vj) in vi's neighbor list
        unsigned offset_vi = offset_buffer[vi];
        unsigned degree_vi = adjacency_list[offset_vi];

        for (unsigned k = 0; k < degree_vi; ++k) {
            if (adjacency_list[offset_vi + 1 + k] == vj) {
                atomicAdd(&edge_weights[offset_vi + 1 + k], cot * 0.5f);
                break;
            }
        }

        // Direction 2: Find edge (vj, vi) in vj's neighbor list
        unsigned offset_vj = offset_buffer[vj];
        unsigned degree_vj = adjacency_list[offset_vj];

        for (unsigned k = 0; k < degree_vj; ++k) {
            if (adjacency_list[offset_vj + 1 + k] == vi) {
                atomicAdd(&edge_weights[offset_vj + 1 + k], cot * 0.5f);
                break;
            }
        }
    }
}

cuda::CUDALinearBufferHandle compute_cotangent_weights(
    cuda::CUDALinearBufferHandle vertices,
    cuda::CUDALinearBufferHandle face_indices,
    std::tuple<cuda::CUDALinearBufferHandle, cuda::CUDALinearBufferHandle>
        adjacency_map)
{
    auto adjacency_list_handle = std::get<0>(adjacency_map);
    auto offset_buffer_handle = std::get<1>(adjacency_map);

    auto vertices_ptr = (float*)vertices->get_device_ptr();
    auto face_indices_ptr = (unsigned*)face_indices->get_device_ptr();
    auto adjacency_list_ptr = (unsigned*)adjacency_list_handle->get_device_ptr();
    auto offset_buffer_ptr = (unsigned*)offset_buffer_handle->get_device_ptr();

    unsigned num_vertices = vertices->getDesc().element_count / 3;  // 3 floats per vertex
    unsigned num_faces = face_indices->getDesc().element_count / 3;  // 3 indices per triangle

    // Allocate edge weights buffer (same size as adjacency_list)
    unsigned adjacency_list_size = adjacency_list_handle->getDesc().element_count;
    cuda::CUDALinearBufferDesc weight_desc;
    weight_desc.element_count = adjacency_list_size;
    weight_desc.element_size = sizeof(float);

    auto weight_buffer = cuda::create_cuda_linear_buffer(weight_desc);
    auto weights_ptr = (float*)weight_buffer->get_device_ptr();

    // Initialize weights to zero
    thrust::device_ptr<float> weights_dev_ptr(weights_ptr);
    thrust::fill(thrust::device, weights_dev_ptr, weights_dev_ptr + adjacency_list_size, 0.0f);

    // Launch kernel: one thread per triangle
    int threads_per_block = 256;
    int blocks = (num_faces + threads_per_block - 1) / threads_per_block;

    compute_cotangent_weights_kernel<<<blocks, threads_per_block>>>(
        vertices_ptr,
        face_indices_ptr,
        num_faces,
        adjacency_list_ptr,
        offset_buffer_ptr,
        num_vertices,
        weights_ptr);
    cudaDeviceSynchronize();

    return weight_buffer;
}

}  // namespace rzsim_cuda

RUZINO_NAMESPACE_CLOSE_SCOPE
