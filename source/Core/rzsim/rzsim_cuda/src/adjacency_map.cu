#include <thrust/device_vector.h>
#include <thrust/distance.h>
#include <thrust/execution_policy.h>
#include <thrust/for_each.h>
#include <thrust/scan.h>
#include <thrust/sort.h>
#include <thrust/tuple.h>
#include <thrust/unique.h>

#include <RHI/cuda.hpp>
#include <RHI/rhi.hpp>

#include "rzsim_cuda/adjacency_map.cuh"
#include "rzsim_cuda/neo_hookean.cuh"

RUZINO_NAMESPACE_OPEN_SCOPE

namespace rzsim_cuda {

// ============================================================================
// Surface Mesh (Triangles): Store opposite edge pairs for each vertex
// ============================================================================

// Count how many triangles each vertex belongs to
__global__ void count_vertex_triangles_kernel(
    const unsigned* triangles,
    unsigned num_triangles,
    unsigned* triangle_counts)
{
    unsigned tri_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (tri_idx >= num_triangles)
        return;

    unsigned v0 = triangles[tri_idx * 3 + 0];
    unsigned v1 = triangles[tri_idx * 3 + 1];
    unsigned v2 = triangles[tri_idx * 3 + 2];

    atomicAdd(&triangle_counts[v0], 1);
    atomicAdd(&triangle_counts[v1], 1);
    atomicAdd(&triangle_counts[v2], 1);
}

// Fill opposite edge pairs for each vertex
// For vertex v in triangle (v, a, b), store pair (a, b) with consistent
// orientation
__global__ void fill_surface_adjacency_kernel(
    const unsigned* triangles,
    unsigned num_triangles,
    const unsigned* offsets,
    unsigned* write_positions,
    unsigned* adjacency_list)
{
    unsigned tri_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (tri_idx >= num_triangles)
        return;

    unsigned v0 = triangles[tri_idx * 3 + 0];
    unsigned v1 = triangles[tri_idx * 3 + 1];
    unsigned v2 = triangles[tri_idx * 3 + 2];

    // For v0: opposite edge is (v1, v2)
    unsigned pos0 = atomicAdd(&write_positions[v0], 2);
    adjacency_list[offsets[v0] + 1 + pos0 + 0] = v1;
    adjacency_list[offsets[v0] + 1 + pos0 + 1] = v2;

    // For v1: opposite edge is (v2, v0) - maintaining CCW order
    unsigned pos1 = atomicAdd(&write_positions[v1], 2);
    adjacency_list[offsets[v1] + 1 + pos1 + 0] = v2;
    adjacency_list[offsets[v1] + 1 + pos1 + 1] = v0;

    // For v2: opposite edge is (v0, v1)
    unsigned pos2 = atomicAdd(&write_positions[v2], 2);
    adjacency_list[offsets[v2] + 1 + pos2 + 0] = v0;
    adjacency_list[offsets[v2] + 1 + pos2 + 1] = v1;
}

std::tuple<cuda::CUDALinearBufferHandle, cuda::CUDALinearBufferHandle>
compute_surface_adjacency_gpu(
    cuda::CUDALinearBufferHandle vertices,
    cuda::CUDALinearBufferHandle triangles)
{
    unsigned num_vertices =
        vertices->getDesc().element_count;  // glm::vec3 count
    unsigned num_triangles =
        triangles->getDesc().element_count / 3;  // 3 indices per triangle

    auto triangle_ptr = (unsigned*)triangles->get_device_ptr();

    // Step 1: Count triangles per vertex
    thrust::device_vector<unsigned> triangle_counts(num_vertices, 0);
    auto counts_ptr = thrust::raw_pointer_cast(triangle_counts.data());

    int threads = 256;
    int blocks = (num_triangles + threads - 1) / threads;

    count_vertex_triangles_kernel<<<blocks, threads>>>(
        triangle_ptr, num_triangles, counts_ptr);
    cudaDeviceSynchronize();

    // Step 2: Build offset buffer
    // Each vertex stores: [count, pair1_a, pair1_b, pair2_a, pair2_b, ...]
    thrust::device_vector<unsigned> offsets(num_vertices);
    thrust::device_vector<unsigned> adjacency_sizes(num_vertices);

    // Each triangle contributes 2 values (one edge pair) per vertex
    thrust::transform(
        thrust::device,
        triangle_counts.begin(),
        triangle_counts.end(),
        adjacency_sizes.begin(),
        [] __device__(unsigned count) {
            return 1 + count * 2;
        });  // +1 for count field

    thrust::exclusive_scan(
        thrust::device,
        adjacency_sizes.begin(),
        adjacency_sizes.end(),
        offsets.begin());

    unsigned total_size = thrust::reduce(
        thrust::device, adjacency_sizes.begin(), adjacency_sizes.end());

    // Step 3: Allocate and fill adjacency list
    cuda::CUDALinearBufferDesc adj_desc;
    adj_desc.element_count = total_size;
    adj_desc.element_size = sizeof(unsigned);
    auto adjacency_buffer = cuda::create_cuda_linear_buffer(adj_desc);
    auto adj_ptr = (unsigned*)adjacency_buffer->get_device_ptr();

    // Initialize count fields
    auto offsets_ptr = thrust::raw_pointer_cast(offsets.data());
    thrust::for_each(
        thrust::device,
        thrust::counting_iterator<unsigned>(0),
        thrust::counting_iterator<unsigned>(num_vertices),
        [adj_ptr, offsets_ptr, counts_ptr] __device__(unsigned v) {
            adj_ptr[offsets_ptr[v]] = counts_ptr[v];
        });

    // Track write positions for each vertex
    thrust::device_vector<unsigned> write_positions(num_vertices, 0);
    auto write_pos_ptr = thrust::raw_pointer_cast(write_positions.data());

    fill_surface_adjacency_kernel<<<blocks, threads>>>(
        triangle_ptr, num_triangles, offsets_ptr, write_pos_ptr, adj_ptr);
    cudaDeviceSynchronize();

    // Step 4: Create offset buffer for output
    cuda::CUDALinearBufferDesc offset_desc;
    offset_desc.element_count = num_vertices;
    offset_desc.element_size = sizeof(unsigned);
    auto offset_buffer = cuda::create_cuda_linear_buffer(offset_desc);

    cudaMemcpy(
        (void*)offset_buffer->get_device_ptr(),
        offsets_ptr,
        num_vertices * sizeof(unsigned),
        cudaMemcpyDeviceToDevice);

    return std::make_tuple(adjacency_buffer, offset_buffer);
}

// ============================================================================
// Volume Mesh (Tetrahedra): Store opposite face triplets for each vertex
// Input: triangular faces, reconstruct tetrahedral topology
// ============================================================================

// Edge represented as sorted pair (min, max)
struct Edge {
    unsigned v0, v1;

    __host__ __device__ Edge() : v0(0), v1(0)
    {
    }

    __host__ __device__ Edge(unsigned a, unsigned b)
    {
        v0 = min(a, b);
        v1 = max(a, b);
    }

    __host__ __device__ bool operator<(const Edge& other) const
    {
        if (v0 != other.v0)
            return v0 < other.v0;
        return v1 < other.v1;
    }

    __host__ __device__ bool operator==(const Edge& other) const
    {
        return v0 == other.v0 && v1 == other.v1;
    }
};

// Device function to check if a vertex is in a triangle
__device__ bool
contains_vertex(unsigned v0, unsigned v1, unsigned v2, unsigned v)
{
    return (v == v0) || (v == v1) || (v == v2);
}

// Binary search for edge in sorted edge list
__device__ bool
has_edge_fast(const Edge* edges, unsigned num_edges, unsigned v1, unsigned v2)
{
    Edge target(v1, v2);

    unsigned left = 0;
    unsigned right = num_edges;

    while (left < right) {
        unsigned mid = (left + right) / 2;
        if (edges[mid] < target) {
            left = mid + 1;
        }
        else if (target < edges[mid]) {
            right = mid;
        }
        else {
            return true;
        }
    }
    return false;
}

// Triangle structure for deduplication (normalized form)
struct Triangle {
    unsigned v0, v1, v2;
    unsigned original_index;  // Index in original triangle buffer

    __host__ __device__ Triangle() : v0(0), v1(0), v2(0), original_index(0)
    {
    }

    __host__ __device__
    Triangle(unsigned a, unsigned b, unsigned c, unsigned orig_idx)
        : original_index(orig_idx)
    {
        // Normalize: sort vertices in ascending order (orientation-independent)
        // This treats (a,b,c) and (a,c,b) as the same triangle
        unsigned vmin = a < b ? (a < c ? a : c) : (b < c ? b : c);
        unsigned vmax = a > b ? (a > c ? a : c) : (b > c ? b : c);
        unsigned vmid =
            (a != vmin && a != vmax) ? a : ((b != vmin && b != vmax) ? b : c);

        v0 = vmin;
        v1 = vmid;
        v2 = vmax;
    }

    __host__ __device__ bool operator<(const Triangle& other) const
    {
        if (v0 != other.v0)
            return v0 < other.v0;
        if (v1 != other.v1)
            return v1 < other.v1;
        return v2 < other.v2;
    }

    __host__ __device__ bool operator==(const Triangle& other) const
    {
        return v0 == other.v0 && v1 == other.v1 && v2 == other.v2;
    }
};

// Kernel to normalize triangles for deduplication
__global__ void normalize_triangles_kernel(
    const unsigned* triangles,
    unsigned num_triangles,
    Triangle* normalized)
{
    unsigned tri_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (tri_idx >= num_triangles)
        return;

    unsigned v0 = triangles[tri_idx * 3 + 0];
    unsigned v1 = triangles[tri_idx * 3 + 1];
    unsigned v2 = triangles[tri_idx * 3 + 2];

    normalized[tri_idx] = Triangle(v0, v1, v2, tri_idx);
}

// Kernel to extract all edges from triangles
__global__ void extract_edges_kernel(
    const unsigned* triangles,
    unsigned num_triangles,
    Edge* edges)
{
    unsigned tri_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (tri_idx >= num_triangles)
        return;

    unsigned v0 = triangles[tri_idx * 3 + 0];
    unsigned v1 = triangles[tri_idx * 3 + 1];
    unsigned v2 = triangles[tri_idx * 3 + 2];

    // Each triangle contributes 3 edges
    edges[tri_idx * 3 + 0] = Edge(v0, v1);
    edges[tri_idx * 3 + 1] = Edge(v1, v2);
    edges[tri_idx * 3 + 2] = Edge(v2, v0);
}

// Binary search for a triangle in sorted triangle list
__device__ bool has_triangle_fast(
    const Triangle* triangles,
    unsigned num_triangles,
    unsigned a,
    unsigned b,
    unsigned c)
{
    Triangle target(a, b, c, 0);  // Normalized triangle for search

    // Binary search in sorted triangle array
    int left = 0;
    int right = num_triangles - 1;

    while (left <= right) {
        int mid = left + (right - left) / 2;
        const Triangle& mid_tri = triangles[mid];

        if (mid_tri == target) {
            return true;
        }
        else if (mid_tri < target) {
            left = mid + 1;
        }
        else {
            right = mid - 1;
        }
    }

    return false;
}

// Check if vertex v and face (a,b,c) form a valid tetrahedron
// by verifying all 4 faces exist in the input (matching OVM's approach)
__device__ bool forms_tetrahedron_fast(
    const Triangle* triangles,
    unsigned num_triangles,
    unsigned v,
    unsigned a,
    unsigned b,
    unsigned c)
{
    // Face (a,b,c) already exists (we're iterating over it)
    // We need to check if the other 3 faces of tetrahedron (v,a,b,c) exist:
    // - Face (v,a,b)
    // - Face (v,b,c)
    // - Face (v,a,c)

    if (!has_triangle_fast(triangles, num_triangles, v, a, b))
        return false;
    if (!has_triangle_fast(triangles, num_triangles, v, b, c))
        return false;
    if (!has_triangle_fast(triangles, num_triangles, v, a, c))
        return false;

    return true;
}

// Compute signed volume of tetrahedron (v, a, b, c)
// Positive if vertices are in right-hand orientation
__device__ float compute_tet_signed_volume(
    const float* positions,
    unsigned v,
    unsigned a,
    unsigned b,
    unsigned c)
{
    // Get vertex positions
    float x0[3] = { positions[v * 3 + 0],
                    positions[v * 3 + 1],
                    positions[v * 3 + 2] };
    float x1[3] = { positions[a * 3 + 0],
                    positions[a * 3 + 1],
                    positions[a * 3 + 2] };
    float x2[3] = { positions[b * 3 + 0],
                    positions[b * 3 + 1],
                    positions[b * 3 + 2] };
    float x3[3] = { positions[c * 3 + 0],
                    positions[c * 3 + 1],
                    positions[c * 3 + 2] };

    // Compute edge vectors from v
    float e1[3] = { x1[0] - x0[0], x1[1] - x0[1], x1[2] - x0[2] };
    float e2[3] = { x2[0] - x0[0], x2[1] - x0[1], x2[2] - x0[2] };
    float e3[3] = { x3[0] - x0[0], x3[1] - x0[1], x3[2] - x0[2] };

    // Compute determinant: det([e1, e2, e3])
    float det = e1[0] * (e2[1] * e3[2] - e2[2] * e3[1]) -
                e1[1] * (e2[0] * e3[2] - e2[2] * e3[0]) +
                e1[2] * (e2[0] * e3[1] - e2[1] * e3[0]);

    return det / 6.0f;  // Signed volume
}

// Count valid opposite faces using 2D parallelization
__global__ void count_vertex_opposite_faces_kernel(
    const unsigned* triangles_original,
    unsigned num_triangles_original,
    const Triangle* triangles_sorted,
    unsigned num_triangles_sorted,
    unsigned num_vertices,
    unsigned* face_counts)
{
    unsigned long long idx =
        (unsigned long long)blockIdx.x * blockDim.x + threadIdx.x;
    unsigned long long total =
        (unsigned long long)num_triangles_original * num_vertices;

    if (idx >= total)
        return;

    unsigned tri_idx = (unsigned)(idx / num_vertices);
    unsigned v = (unsigned)(idx % num_vertices);

    // Read triangle vertices from ORIGINAL array (preserve vertex order)
    unsigned v0 = triangles_original[tri_idx * 3 + 0];
    unsigned v1 = triangles_original[tri_idx * 3 + 1];
    unsigned v2 = triangles_original[tri_idx * 3 + 2];

    // Check if vertex v is not in this triangle and forms a tetrahedron
    if (!contains_vertex(v0, v1, v2, v) &&
        forms_tetrahedron_fast(
            triangles_sorted, num_triangles_sorted, v, v0, v1, v2)) {
        atomicAdd(&face_counts[v], 1);
    }
}

// Fill opposite face triplets using 2D parallelization
__global__ void fill_volume_adjacency_kernel(
    const float* positions,
    const unsigned* triangles_original,
    unsigned num_triangles_original,
    const Triangle* triangles_sorted,
    unsigned num_triangles_sorted,
    unsigned num_vertices,
    const unsigned* offsets,
    unsigned* write_positions,
    unsigned* adjacency_list)
{
    unsigned long long idx =
        (unsigned long long)blockIdx.x * blockDim.x + threadIdx.x;
    unsigned long long total =
        (unsigned long long)num_triangles_original * num_vertices;

    if (idx >= total)
        return;

    unsigned tri_idx = (unsigned)(idx / num_vertices);
    unsigned v = (unsigned)(idx % num_vertices);

    // Read triangle vertices from ORIGINAL array (preserve vertex order)
    unsigned v0 = triangles_original[tri_idx * 3 + 0];
    unsigned v1 = triangles_original[tri_idx * 3 + 1];
    unsigned v2 = triangles_original[tri_idx * 3 + 2];

    // Check if vertex v is not in this triangle and forms a tetrahedron
    if (!contains_vertex(v0, v1, v2, v) &&
        forms_tetrahedron_fast(
            triangles_sorted, num_triangles_sorted, v, v0, v1, v2)) {
        // Check orientation: compute signed volume of tet (v, v0, v1, v2)
        float signed_vol = compute_tet_signed_volume(positions, v, v0, v1, v2);

        unsigned pos = atomicAdd(&write_positions[v], 3);

        // Store vertices in order that gives positive volume
        if (signed_vol > 0.0f) {
            // Positive orientation: store as is
            adjacency_list[offsets[v] + 1 + pos + 0] = v0;
            adjacency_list[offsets[v] + 1 + pos + 1] = v1;
            adjacency_list[offsets[v] + 1 + pos + 2] = v2;
        }
        else {
            // Negative orientation: swap v1 and v2 to flip
            adjacency_list[offsets[v] + 1 + pos + 0] = v0;
            adjacency_list[offsets[v] + 1 + pos + 1] = v2;
            adjacency_list[offsets[v] + 1 + pos + 2] = v1;
        }
    }
}

// Kernel to build element mappings (apex vertex and local face index)
__global__ void build_element_mappings_kernel(
    const unsigned* adjacency_ptr,
    const unsigned* offsets_ptr,
    unsigned num_vertices,
    int* elem_to_v_ptr,
    int* elem_to_lf_ptr,
    int* counter_ptr)
{
    unsigned v = blockIdx.x * blockDim.x + threadIdx.x;
    if (v >= num_vertices)
        return;

    unsigned offset = offsets_ptr[v];
    unsigned count = adjacency_ptr[offset];

    for (unsigned local_idx = 0; local_idx < count; local_idx++) {
        unsigned v0 = adjacency_ptr[offset + 1 + local_idx * 3 + 0];
        unsigned v1 = adjacency_ptr[offset + 1 + local_idx * 3 + 1];
        unsigned v2 = adjacency_ptr[offset + 1 + local_idx * 3 + 2];

        // Check if v is the minimum among {v, v0, v1, v2}
        bool is_min = (v < v0) && (v < v1) && (v < v2);

        if (is_min) {
            // This is the unique representative for this tetrahedron
            int elem_idx = atomicAdd(counter_ptr, 1);
            elem_to_v_ptr[elem_idx] = (int)v;
            elem_to_lf_ptr[elem_idx] = (int)local_idx;
        }
    }
}

std::tuple<cuda::CUDALinearBufferHandle, cuda::CUDALinearBufferHandle, unsigned>
compute_volume_adjacency_gpu(
    cuda::CUDALinearBufferHandle vertices,
    cuda::CUDALinearBufferHandle triangles)
{
    unsigned num_vertices =
        vertices->getDesc().element_count;  // glm::vec3 count
    unsigned num_triangles =
        triangles->getDesc().element_count / 3;  // 3 indices per triangle

    auto tri_ptr = (unsigned*)triangles->get_device_ptr();

    int threads = 256;
    int blocks = (num_triangles + threads - 1) / threads;

    // Step 0: Deduplicate triangles for adjacency computation
    // (but build edges from ALL triangles including duplicates)
    thrust::device_vector<Triangle> normalized_triangles(num_triangles);
    auto norm_tri_ptr = thrust::raw_pointer_cast(normalized_triangles.data());

    normalize_triangles_kernel<<<blocks, threads>>>(
        tri_ptr, num_triangles, norm_tri_ptr);
    cudaDeviceSynchronize();

    // Sort and remove duplicates
    thrust::sort(
        thrust::device,
        normalized_triangles.begin(),
        normalized_triangles.end());
    auto new_tri_end = thrust::unique(
        thrust::device,
        normalized_triangles.begin(),
        normalized_triangles.end());
    unsigned num_unique_triangles =
        thrust::distance(normalized_triangles.begin(), new_tri_end);
    normalized_triangles.resize(num_unique_triangles);

    // Debug: Check how many duplicates were removed
    unsigned num_duplicates = num_triangles - num_unique_triangles;
    if (num_duplicates > 0) {
        printf(
            "[GPU Adjacency] Triangle deduplication: %u -> %u (removed %u "
            "duplicates, %.1f%%)\n",
            num_triangles,
            num_unique_triangles,
            num_duplicates,
            100.0f * num_duplicates / num_triangles);
    }

    auto norm_tri_ptr_const =
        thrust::raw_pointer_cast(normalized_triangles.data());

    // Step 2: Count opposite faces per vertex using 2D parallelization
    thrust::device_vector<unsigned> face_counts(num_vertices, 0);
    auto counts_ptr = thrust::raw_pointer_cast(face_counts.data());

    unsigned long long total_pairs =
        (unsigned long long)num_triangles *
        num_vertices;  // Use ORIGINAL triangle count
    blocks = (unsigned)((total_pairs + threads - 1) / threads);

    count_vertex_opposite_faces_kernel<<<blocks, threads>>>(
        tri_ptr,               // Original triangles for iteration
        num_triangles,         // Original count for iteration
        norm_tri_ptr_const,    // Normalized triangles for binary search
        num_unique_triangles,  // Unique count for binary search
        num_vertices,
        counts_ptr);
    cudaDeviceSynchronize();

    // Step 3: Build offset buffer
    // Each vertex stores: [count, triplet1_a, triplet1_b, triplet1_c, ...]
    thrust::device_vector<unsigned> offsets(num_vertices);
    thrust::device_vector<unsigned> adjacency_sizes(num_vertices);

    // Each opposite face contributes 3 values (one face triplet) per vertex
    thrust::transform(
        thrust::device,
        face_counts.begin(),
        face_counts.end(),
        adjacency_sizes.begin(),
        [] __device__(unsigned count) {
            return 1 + count * 3;
        });  // +1 for count field

    thrust::exclusive_scan(
        thrust::device,
        adjacency_sizes.begin(),
        adjacency_sizes.end(),
        offsets.begin());

    unsigned total_size = thrust::reduce(
        thrust::device, adjacency_sizes.begin(), adjacency_sizes.end());

    // Step 3: Allocate and fill adjacency list
    cuda::CUDALinearBufferDesc adj_desc;
    adj_desc.element_count = total_size;
    adj_desc.element_size = sizeof(unsigned);
    auto adjacency_buffer = cuda::create_cuda_linear_buffer(adj_desc);
    auto adj_ptr = (unsigned*)adjacency_buffer->get_device_ptr();

    // Initialize count fields
    auto offsets_ptr = thrust::raw_pointer_cast(offsets.data());
    thrust::for_each(
        thrust::device,
        thrust::counting_iterator<unsigned>(0),
        thrust::counting_iterator<unsigned>(num_vertices),
        [adj_ptr, offsets_ptr, counts_ptr] __device__(unsigned v) {
            adj_ptr[offsets_ptr[v]] = counts_ptr[v];
        });

    // Track write positions for each vertex
    thrust::device_vector<unsigned> write_positions(num_vertices, 0);
    auto write_pos_ptr = thrust::raw_pointer_cast(write_positions.data());

    // Get positions pointer for orientation check
    const float* positions_ptr = vertices->get_device_ptr<float>();

    fill_volume_adjacency_kernel<<<blocks, threads>>>(
        positions_ptr,
        tri_ptr,               // Original triangles for iteration
        num_triangles,         // Original count for iteration
        norm_tri_ptr_const,    // Normalized triangles for binary search
        num_unique_triangles,  // Unique count for binary search
        num_vertices,
        offsets_ptr,
        write_pos_ptr,
        adj_ptr);
    cudaDeviceSynchronize();

    // Step 4: Create offset buffer for output
    cuda::CUDALinearBufferDesc offset_desc;
    offset_desc.element_count = num_vertices;
    offset_desc.element_size = sizeof(unsigned);
    auto offset_buffer = cuda::create_cuda_linear_buffer(offset_desc);

    cudaMemcpy(
        (void*)offset_buffer->get_device_ptr(),
        offsets_ptr,
        num_vertices * sizeof(unsigned),
        cudaMemcpyDeviceToDevice);

    // Calculate total number of tetrahedra
    // Each tetrahedron has 4 vertices, and each vertex records one opposite
    // face So total_face_counts = 4 * num_tetrahedra Therefore: num_tetrahedra
    // = total_face_counts / 4
    unsigned total_face_counts = thrust::reduce(
        thrust::device,
        face_counts.begin(),
        face_counts.end(),
        0u,
        thrust::plus<unsigned>());

    unsigned total_elements = total_face_counts / 4;

    printf(
        "[GPU Adjacency] Total opposite face records: %u, computed tets: %u\n",
        total_face_counts,
        total_elements);
    printf(
        "[GPU Adjacency] Input: %u vertices, %u triangles (%u unique)\n",
        num_vertices,
        num_triangles,
        num_unique_triangles);

    return std::make_tuple(adjacency_buffer, offset_buffer, total_elements);
}

// Functor for comparing edge pairs
struct EdgePairEqual {
    __host__ __device__ bool operator()(
        const thrust::tuple<int, int>& a,
        const thrust::tuple<int, int>& b) const
    {
        return thrust::get<0>(a) == thrust::get<0>(b) &&
               thrust::get<1>(a) == thrust::get<1>(b);
    }
};

cuda::CUDALinearBufferHandle build_edge_set_gpu(
    cuda::CUDALinearBufferHandle positions,
    cuda::CUDALinearBufferHandle edges)
{
    // Get triangle count
    size_t num_triangles = edges->getDesc().element_count / 3;

    // Allocate temporary buffer for all edges (3 edges per triangle)
    thrust::device_vector<int> all_edges(num_triangles * 6);
    const int* triangles = edges->get_device_ptr<int>();
    int* edge_pairs = thrust::raw_pointer_cast(all_edges.data());

    // Extract edges from triangles
    cuda::GPUParallelFor(
        "extract_edges", num_triangles, GPU_LAMBDA_Ex(int tid) {
            int base_idx = tid * 3;
            int v0 = triangles[base_idx];
            int v1 = triangles[base_idx + 1];
            int v2 = triangles[base_idx + 2];

            int output_base = tid * 6;
            // Edge 0-1
            edge_pairs[output_base + 0] = min(v0, v1);
            edge_pairs[output_base + 1] = max(v0, v1);
            // Edge 1-2
            edge_pairs[output_base + 2] = min(v1, v2);
            edge_pairs[output_base + 3] = max(v1, v2);
            // Edge 2-0
            edge_pairs[output_base + 4] = min(v2, v0);
            edge_pairs[output_base + 5] = max(v2, v0);
        });

    // Create vectors for edge pairs
    thrust::device_vector<int> edge_first(num_triangles * 3);
    thrust::device_vector<int> edge_second(num_triangles * 3);
    const int* interleaved = thrust::raw_pointer_cast(all_edges.data());
    int* first = thrust::raw_pointer_cast(edge_first.data());
    int* second = thrust::raw_pointer_cast(edge_second.data());

    // Separate the interleaved edge data
    cuda::GPUParallelFor(
        "separate_edges", num_triangles * 3, GPU_LAMBDA_Ex(int tid) {
            first[tid] = interleaved[tid * 2];
            second[tid] = interleaved[tid * 2 + 1];
        });

    // Create zip iterator
    auto edge_begin = thrust::make_zip_iterator(
        thrust::make_tuple(edge_first.begin(), edge_second.begin()));
    auto edge_end = thrust::make_zip_iterator(
        thrust::make_tuple(edge_first.end(), edge_second.end()));

    // Sort edges
    thrust::sort(
        edge_begin,
        edge_end,
        [] __device__(
            const thrust::tuple<int, int>& a,
            const thrust::tuple<int, int>& b) {
            if (thrust::get<0>(a) != thrust::get<0>(b))
                return thrust::get<0>(a) < thrust::get<0>(b);
            return thrust::get<1>(a) < thrust::get<1>(b);
        });

    // Remove duplicates
    auto new_end = thrust::unique(edge_begin, edge_end, EdgePairEqual());

    // Calculate unique edge count
    size_t num_unique_edges = new_end - edge_begin;

    // Copy unique edges to output buffer (interleaved format)
    auto output_buffer =
        cuda::create_cuda_linear_buffer<int>(size_t(num_unique_edges * 2));
    int* output_ptr = output_buffer->get_device_ptr<int>();
    const int* edge_first_ptr = thrust::raw_pointer_cast(edge_first.data());
    const int* edge_second_ptr = thrust::raw_pointer_cast(edge_second.data());

    // Interleave the data
    cuda::GPUParallelFor(
        "interleave_edges", num_unique_edges, GPU_LAMBDA_Ex(int tid) {
            output_ptr[tid * 2] = edge_first_ptr[tid];
            output_ptr[tid * 2 + 1] = edge_second_ptr[tid];
        });

    return output_buffer;
}

cuda::CUDALinearBufferHandle compute_rest_lengths_gpu(
    cuda::CUDALinearBufferHandle positions,
    cuda::CUDALinearBufferHandle springs)
{
    size_t num_springs = springs->getDesc().element_count / 2;
    auto rest_lengths_buffer =
        cuda::create_cuda_linear_buffer<float>(num_springs);

    const float* pos_ptr = positions->get_device_ptr<float>();
    const int* springs_ptr = springs->get_device_ptr<int>();
    float* rest_ptr = rest_lengths_buffer->get_device_ptr<float>();

    cuda::GPUParallelFor(
        "compute_rest_lengths", num_springs, GPU_LAMBDA_Ex(int s) {
            int i = springs_ptr[s * 2];
            int j = springs_ptr[s * 2 + 1];

            float dx = pos_ptr[i * 3] - pos_ptr[j * 3];
            float dy = pos_ptr[i * 3 + 1] - pos_ptr[j * 3 + 1];
            float dz = pos_ptr[i * 3 + 2] - pos_ptr[j * 3 + 2];

            rest_ptr[s] = sqrtf(dx * dx + dy * dy + dz * dz);
        });

    return rest_lengths_buffer;
}

// Build adjacency list: for each vertex, store list of adjacent vertices
// Returns: (adjacent_vertices, vertex_offsets, rest_lengths)
// Format: adjacent_vertices[vertex_offsets[v]..vertex_offsets[v+1]] = adjacent
// vertex indices for vertex v
std::tuple<
    cuda::CUDALinearBufferHandle,
    cuda::CUDALinearBufferHandle,
    cuda::CUDALinearBufferHandle>
build_adjacency_list_gpu(
    cuda::CUDALinearBufferHandle triangles,
    cuda::CUDALinearBufferHandle positions,
    int num_particles)
{
    // Step 1: Extract and deduplicate edges
    auto springs =
        build_edge_set_gpu(cuda::CUDALinearBufferHandle(), triangles);
    int num_springs = springs->getDesc().element_count / 2;
    const int* springs_ptr = springs->get_device_ptr<int>();

    // Step 2: Compute rest lengths for each edge
    auto rest_lengths_per_edge = compute_rest_lengths_gpu(positions, springs);

    // Step 3: Count adjacent vertices per vertex
    auto d_adj_count = cuda::create_cuda_linear_buffer<int>(num_particles);
    int* count_ptr = d_adj_count->get_device_ptr<int>();
    cudaMemset(count_ptr, 0, num_particles * sizeof(int));

    // Each edge contributes 2 adjacencies
    cuda::GPUParallelFor(
        "count_adjacencies", num_springs, GPU_LAMBDA_Ex(int s) {
            int i = springs_ptr[s * 2];
            int j = springs_ptr[s * 2 + 1];
            atomicAdd(&count_ptr[i], 1);
            atomicAdd(&count_ptr[j], 1);
        });

    // Step 4: Build offset buffer (prefix sum)
    auto d_offsets = cuda::create_cuda_linear_buffer<int>(num_particles + 1);
    int* offsets_ptr = d_offsets->get_device_ptr<int>();

    thrust::device_ptr<int> count_thrust(count_ptr);
    thrust::device_ptr<int> offsets_thrust(offsets_ptr);
    thrust::exclusive_scan(
        thrust::device,
        count_thrust,
        count_thrust + num_particles,
        offsets_thrust);

    // Get total count
    int total_entries;
    cudaMemcpy(
        &total_entries,
        count_ptr + num_particles - 1,
        sizeof(int),
        cudaMemcpyDeviceToHost);
    int last_offset;
    cudaMemcpy(
        &last_offset,
        offsets_ptr + num_particles - 1,
        sizeof(int),
        cudaMemcpyDeviceToHost);
    total_entries += last_offset;
    cudaMemcpy(
        offsets_ptr + num_particles,
        &total_entries,
        sizeof(int),
        cudaMemcpyHostToDevice);

    // Step 5: Allocate adjacency and rest length buffers
    auto d_adjacent_vertices =
        cuda::create_cuda_linear_buffer<int>(total_entries);
    auto d_rest_lengths = cuda::create_cuda_linear_buffer<float>(total_entries);
    int* adj_ptr = d_adjacent_vertices->get_device_ptr<int>();
    float* rest_ptr = d_rest_lengths->get_device_ptr<float>();
    const float* edge_rest_ptr = rest_lengths_per_edge->get_device_ptr<float>();

    // Reset counts for filling
    cudaMemset(count_ptr, 0, num_particles * sizeof(int));

    // Step 6: Fill adjacency lists
    cuda::GPUParallelFor(
        "fill_adjacency", num_springs, GPU_LAMBDA_Ex(int s) {
            int i = springs_ptr[s * 2];
            int j = springs_ptr[s * 2 + 1];
            float rest_len = edge_rest_ptr[s];

            // Add j to i's adjacency list
            int pos_i = offsets_ptr[i] + atomicAdd(&count_ptr[i], 1);
            adj_ptr[pos_i] = j;
            rest_ptr[pos_i] = rest_len;

            // Add i to j's adjacency list
            int pos_j = offsets_ptr[j] + atomicAdd(&count_ptr[j], 1);
            adj_ptr[pos_j] = i;
            rest_ptr[pos_j] = rest_len;
        });

    cudaDeviceSynchronize();

    return { d_adjacent_vertices, d_offsets, d_rest_lengths };
}

// ============================================================================
// VolumeAdjacencyMap Implementation
// ============================================================================

VolumeAdjacencyMap::VolumeAdjacencyMap(
    cuda::CUDALinearBufferHandle positions,
    const std::vector<int>& face_vertex_indices)
{
    // Convert face indices to CUDA buffer
    auto face_indices_buffer =
        cuda::create_cuda_linear_buffer(face_vertex_indices);

    // Compute volume adjacency
    std::tie(adjacency_list_, offset_buffer_, num_elements_) =
        compute_volume_adjacency_gpu(positions, face_indices_buffer);

    if (num_elements_ == 0) {
        return;  // No tetrahedral elements found
    }

    // Build element topology mappings
    element_to_vertex_ = cuda::create_cuda_linear_buffer<int>(num_elements_);
    element_to_local_face_ =
        cuda::create_cuda_linear_buffer<int>(num_elements_);

    int* elem_to_v_ptr = element_to_vertex_->get_device_ptr<int>();
    int* elem_to_lf_ptr = element_to_local_face_->get_device_ptr<int>();

    const unsigned* adjacency_ptr = adjacency_list_->get_device_ptr<unsigned>();
    const unsigned* offsets_ptr = offset_buffer_->get_device_ptr<unsigned>();

    unsigned num_vertices = offset_buffer_->getDesc().element_count;

    // Use atomic counter to assign global element indices
    auto element_counter = cuda::create_cuda_linear_buffer<int>(1);
    cudaMemset(
        reinterpret_cast<void*>(element_counter->get_device_ptr()),
        0,
        sizeof(int));
    int* counter_ptr = element_counter->get_device_ptr<int>();

    // For each vertex, process its opposite faces and assign unique element IDs
    // Only vertices where v < all face vertices create elements (deduplication)
    int threads = 256;
    int blocks = (num_vertices + threads - 1) / threads;

    build_element_mappings_kernel<<<blocks, threads>>>(
        adjacency_ptr,
        offsets_ptr,
        num_vertices,
        elem_to_v_ptr,
        elem_to_lf_ptr,
        counter_ptr);

    cudaDeviceSynchronize();
}

}  // namespace rzsim_cuda

RUZINO_NAMESPACE_CLOSE_SCOPE
