#pragma once

#include <vector>

#include "RHI/internal/cuda_extension.hpp"
#include "api.h"

RUZINO_NAMESPACE_OPEN_SCOPE

namespace rzsim_cuda {

class RZSIM_CUDA_API AdjacencyMap {
   public:
    AdjacencyMap(
        const std::vector<int>& face_vertex_indices,
        const std::vector<int>& face_counts);

    // For each vertex, an easy way to get its adjacent faces
    std::tuple<cuda::CUDALinearBufferHandle, cuda::CUDALinearBufferHandle>
    get_adjacent_faces() const;

    cuda::CUDALinearBufferHandle element_to_vertex_buffer() const;
    cuda::CUDALinearBufferHandle element_to_local_face_buffer() const;

   private:
    cuda::CUDALinearBufferHandle adjacency_list_;
    cuda::CUDALinearBufferHandle offset_buffer_;

    //...
};

// Volume mesh adjacency for FEM simulation
class RZSIM_CUDA_API VolumeAdjacencyMap {
   public:
    VolumeAdjacencyMap(
        cuda::CUDALinearBufferHandle positions,
        const std::vector<int>& face_vertex_indices);

    // Accessors for adjacency data
    cuda::CUDALinearBufferHandle adjacency_buffer() const { return adjacency_list_; }
    cuda::CUDALinearBufferHandle offsets_buffer() const { return offset_buffer_; }
    cuda::CUDALinearBufferHandle element_to_vertex_buffer() const { return element_to_vertex_; }
    cuda::CUDALinearBufferHandle element_to_local_face_buffer() const { return element_to_local_face_; }

    unsigned num_elements() const { return num_elements_; }

   private:
    cuda::CUDALinearBufferHandle adjacency_list_;
    cuda::CUDALinearBufferHandle offset_buffer_;
    cuda::CUDALinearBufferHandle element_to_vertex_;
    cuda::CUDALinearBufferHandle element_to_local_face_;
    unsigned num_elements_;
};

// Surface mesh (triangles): For each vertex, stores pairs of opposite edge
// vertices For vertex v in triangle (v, a, b), stores pair (a, b) Format:
// [count_v0, a1,b1, a2,b2, ... | count_v1, ... ]
// - adjacency_list: flattened pairs of opposite vertices
// - offset_buffer: starting position for each vertex
RZSIM_CUDA_API
std::tuple<cuda::CUDALinearBufferHandle, cuda::CUDALinearBufferHandle>
compute_surface_adjacency_gpu(
    cuda::CUDALinearBufferHandle vertices,
    cuda::CUDALinearBufferHandle
        triangles);  // triangle indices [v0,v1,v2, ...]

// Volume mesh (tetrahedra): For each vertex, stores triplets of opposite face
// vertices For vertex v in tetrahedron (v, a, b, c), stores triplet (a, b, c)
// Input: triangle faces, CUDA will reconstruct tetrahedra topology
// Format: [count_v0, a1,b1,c1, a2,b2,c2, ... | count_v1, ... ]
// - adjacency_list: flattened triplets of opposite face vertices
// - offset_buffer: starting position for each vertex
// - num_elements: total number of tetrahedra
RZSIM_CUDA_API
std::tuple<cuda::CUDALinearBufferHandle, cuda::CUDALinearBufferHandle, unsigned>
compute_volume_adjacency_gpu(
    cuda::CUDALinearBufferHandle vertices,
    cuda::CUDALinearBufferHandle
        triangles);  // triangle indices [v0,v1,v2, ...]

// Build edge set from triangles
// Returns: buffer of unique edges in format [v0, v1, v0, v1, ...]
RZSIM_CUDA_API
cuda::CUDALinearBufferHandle build_edge_set_gpu(
    cuda::CUDALinearBufferHandle positions,
    cuda::CUDALinearBufferHandle edges);

// Compute rest lengths for springs/edges
// Returns: buffer of rest lengths, one per edge
RZSIM_CUDA_API
cuda::CUDALinearBufferHandle compute_rest_lengths_gpu(
    cuda::CUDALinearBufferHandle positions,
    cuda::CUDALinearBufferHandle springs);

// Build adjacency list from triangles for mass-spring simulation
// Returns: (adjacent_vertices, vertex_offsets, rest_lengths)
// Format: adjacent_vertices[vertex_offsets[v]..vertex_offsets[v+1]] = neighbors
// of vertex v rest_lengths has the same length as adjacent_vertices
RZSIM_CUDA_API
std::tuple<
    cuda::CUDALinearBufferHandle,
    cuda::CUDALinearBufferHandle,
    cuda::CUDALinearBufferHandle>
build_adjacency_list_gpu(
    cuda::CUDALinearBufferHandle triangles,
    cuda::CUDALinearBufferHandle positions,
    int num_particles);

}  // namespace rzsim_cuda

RUZINO_NAMESPACE_CLOSE_SCOPE
