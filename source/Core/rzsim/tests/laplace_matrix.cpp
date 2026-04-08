#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <RHI/internal/cuda_extension.hpp>
#include <glm/glm.hpp>
#include <iostream>
#include <map>
#include <rzsim_cuda/adjacency_map.cuh>
#include <rzsim_cuda/build_matrix.cuh>
#include <set>
#include <unordered_map>
#include <vector>

using namespace Ruzino;

// Triplet structure for matrix entries (must match GPU definition)
struct Triplet {
    unsigned row;
    unsigned col;
    float value;
};

// Helper to print Laplace matrix from triplets
void print_laplace_matrix(
    const std::vector<Triplet>& triplets,
    unsigned num_vertices)
{
    std::cout << "Laplace matrix (triplet format):\n";
    std::cout << "Non-zeros count: " << triplets.size() << std::endl;

    // Group by row for readability
    std::unordered_map<unsigned, std::vector<std::pair<unsigned, float>>> rows;
    for (const auto& t : triplets) {
        rows[t.row].push_back({ t.col, t.value });
    }

    for (unsigned i = 0; i < num_vertices; ++i) {
        std::cout << "Row " << i << ": ";
        if (rows.count(i)) {
            for (const auto& [col, val] : rows[i]) {
                std::cout << "(" << col << "," << val << ") ";
            }
        }
        std::cout << std::endl;
    }
}

// Test with simple triangle
TEST(LaplacianMatrix, SimpleTriangle)
{
    // Create triangle adjacency: 0-1-2-0 (undirected cycle)
    // Vertex 0: neighbors 1, 2
    // Vertex 1: neighbors 0, 2
    // Vertex 2: neighbors 0, 1

    // Build adjacency list: [degree_v0, neighbors... | degree_v1, neighbors...
    // | ...]
    std::vector<unsigned> adjacency_data = {
        2, 1, 2,  // Vertex 0: degree 2, neighbors 1, 2
        2, 0, 2,  // Vertex 1: degree 2, neighbors 0, 2
        2, 0, 1   // Vertex 2: degree 2, neighbors 0, 1
    };

    std::vector<unsigned> offset_buffer = { 0,
                                            3,
                                            6 };  // Offsets for each vertex

    // Create GPU buffers
    auto adj_gpu = Ruzino::cuda::create_cuda_linear_buffer(adjacency_data);
    auto offset_gpu = Ruzino::cuda::create_cuda_linear_buffer(offset_buffer);
    auto adj_tuple = std::make_tuple(adj_gpu, offset_gpu);

    // Create dummy vertices buffer (size = 3 for 3 vertices)
    std::vector<float> dummy_vertices(3, 0.0f);
    auto vertices_gpu = Ruzino::cuda::create_cuda_linear_buffer(dummy_vertices);

    // Build Laplace matrix
    auto laplace_gpu =
        rzsim_cuda::build_laplace_matrix(adj_tuple, vertices_gpu);

    // Download triplets using convenient interface
    auto triplets_cpu = laplace_gpu->get_host_vector<Triplet>();

    std::cout << "\n=== Triangle Laplace Matrix ===\n";
    print_laplace_matrix(triplets_cpu, 3);

    // Verify diagonal entries = degree
    for (const auto& t : triplets_cpu) {
        if (t.row == t.col) {
            EXPECT_EQ(t.value, 2.0f)
                << "Diagonal L[" << t.row << "][" << t.col << "] should be 2";
        }
    }

    // Count diagonals
    unsigned diag_count = 0;
    for (const auto& t : triplets_cpu) {
        if (t.row == t.col)
            diag_count++;
    }
    EXPECT_EQ(diag_count, 3) << "Should have 3 diagonal entries";
}

// Test with quad
TEST(LaplacianMatrix, Quad)
{
    // Quad: 0-1-2-3-0 (undirected cycle)
    // All vertices have degree 2
    std::vector<unsigned> adjacency_data = {
        2, 1, 3,  // Vertex 0: neighbors 1, 3
        2, 0, 2,  // Vertex 1: neighbors 0, 2
        2, 1, 3,  // Vertex 2: neighbors 1, 3
        2, 0, 2   // Vertex 3: neighbors 0, 2
    };

    std::vector<unsigned> offset_buffer = { 0, 3, 6, 9 };

    auto adj_gpu = Ruzino::cuda::create_cuda_linear_buffer(adjacency_data);
    auto offset_gpu = Ruzino::cuda::create_cuda_linear_buffer(offset_buffer);
    auto adj_tuple = std::make_tuple(adj_gpu, offset_gpu);

    std::vector<float> dummy_vertices(4, 0.0f);
    auto vertices_gpu = Ruzino::cuda::create_cuda_linear_buffer(dummy_vertices);

    auto laplace_gpu =
        rzsim_cuda::build_laplace_matrix(adj_tuple, vertices_gpu);

    // Download triplets using convenient interface
    auto triplets_cpu = laplace_gpu->get_host_vector<Triplet>();
    print_laplace_matrix(triplets_cpu, 4);

    // Verify
    for (const auto& t : triplets_cpu) {
        if (t.row == t.col) {
            EXPECT_EQ(t.value, 2.0f) << "Diagonal should be 2";
        }
    }

    unsigned diag_count = 0;
    for (const auto& t : triplets_cpu) {
        if (t.row == t.col)
            diag_count++;
    }
    EXPECT_EQ(diag_count, 4) << "Should have 4 diagonal entries";
}

// Test with pyramid (mixed degrees)
TEST(LaplacianMatrix, Pyramid)
{
    // Pyramid: apex(4) connected to all 4 base vertices(0,1,2,3)
    // Base forms a quad
    // Vertex 0,1,2,3: degree 3 (connected to 2 neighbors in quad + apex)
    // Vertex 4 (apex): degree 4 (connected to all base vertices)

    std::vector<unsigned> adjacency_data = {
        3, 1, 3, 4,    // Vertex 0: neighbors 1, 3, 4
        3, 0, 2, 4,    // Vertex 1: neighbors 0, 2, 4
        3, 1, 3, 4,    // Vertex 2: neighbors 1, 3, 4
        3, 0, 2, 4,    // Vertex 3: neighbors 0, 2, 4
        4, 0, 1, 2, 3  // Vertex 4 (apex): neighbors 0, 1, 2, 3
    };

    std::vector<unsigned> offset_buffer = { 0, 4, 8, 12, 16 };

    auto adj_gpu = Ruzino::cuda::create_cuda_linear_buffer(adjacency_data);
    auto offset_gpu = Ruzino::cuda::create_cuda_linear_buffer(offset_buffer);
    auto adj_tuple = std::make_tuple(adj_gpu, offset_gpu);

    std::vector<float> dummy_vertices(5, 0.0f);
    auto vertices_gpu = Ruzino::cuda::create_cuda_linear_buffer(dummy_vertices);

    auto laplace_gpu =
        rzsim_cuda::build_laplace_matrix(adj_tuple, vertices_gpu);

    // Download triplets using convenient interface
    auto triplets_cpu = laplace_gpu->get_host_vector<Triplet>();
    print_laplace_matrix(triplets_cpu, 5);

    // Verify apex has degree 4
    float apex_diag = 0.0f;
    for (const auto& t : triplets_cpu) {
        if (t.row == 4 && t.col == 4) {
            apex_diag = t.value;
            break;
        }
    }
    EXPECT_EQ(apex_diag, 4.0f) << "Apex should have degree 4";

    // Verify base vertices have degree 3
    for (unsigned i = 0; i < 4; ++i) {
        float diag = 0.0f;
        for (const auto& t : triplets_cpu) {
            if (t.row == i && t.col == i) {
                diag = t.value;
                break;
            }
        }
        EXPECT_EQ(diag, 3.0f) << "Base vertex " << i << " should have degree 3";
    }
}

// Test cotangent weights with right triangle
TEST(LaplacianMatrix, CotangentWeights_RightTriangle)
{
    // Right triangle at origin: (0,0,0), (1,0,0), (0,1,0)
    // Edge lengths: |01| = 1, |02| = 1, |12| = sqrt(2)
    // Angle at v0 (origin) = 90 degrees, cot = 0
    // Angle at v1 = 45 degrees, cot = 1
    // Angle at v2 = 45 degrees, cot = 1
    //
    // Expected weights (w_ij = (cot α + cot β) / 2):
    // For single triangle, only one opposite angle per edge:
    // w_01 = cot(angle at v2) / 2 = 1.0 / 2 = 0.5
    // w_02 = cot(angle at v1) / 2 = 1.0 / 2 = 0.5
    // w_12 = cot(angle at v0) / 2 = 0.0 / 2 = 0.0

    std::vector<float> vertices = {
        0.0f, 0.0f, 0.0f,  // v0
        1.0f, 0.0f, 0.0f,  // v1
        0.0f, 1.0f, 0.0f   // v2
    };

    std::vector<unsigned> faces = { 0, 1, 2 };  // single triangle

    // Build adjacency map
    std::vector<unsigned> adjacency_data = {
        2, 1, 2,  // v0: neighbors 1, 2
        2, 0, 2,  // v1: neighbors 0, 2
        2, 0, 1   // v2: neighbors 0, 1
    };
    std::vector<unsigned> offsets = { 0, 3, 6 };

    auto vertices_gpu = Ruzino::cuda::create_cuda_linear_buffer(vertices);
    auto faces_gpu = Ruzino::cuda::create_cuda_linear_buffer(faces);
    auto adj_gpu = Ruzino::cuda::create_cuda_linear_buffer(adjacency_data);
    auto offset_gpu = Ruzino::cuda::create_cuda_linear_buffer(offsets);
    auto adj_tuple = std::make_tuple(adj_gpu, offset_gpu);

    // Compute cotangent weights
    auto weights_gpu = rzsim_cuda::compute_cotangent_weights(
        vertices_gpu, faces_gpu, adj_tuple);
    auto weights_cpu = weights_gpu->get_host_vector<float>();

    std::cout << "\n=== Cotangent Weights (Right Triangle) ===\n";
    std::cout << "Weights in adjacency order:\n";
    for (size_t i = 0; i < weights_cpu.size(); ++i) {
        std::cout << "[" << i << "] = " << weights_cpu[i] << "\n";
    }

    // Verify weights (accounting for adjacency_list structure: [degree,
    // neighbors...]) weights[0] corresponds to adjacency_data[0] = degree
    // (skip) weights[1] corresponds to edge v0->v1 weights[2] corresponds to
    // edge v0->v2

    EXPECT_NEAR(weights_cpu[1], 0.5f, 1e-5f)
        << "Weight for edge (0,1) should be cot(45°)/2 = 0.5";
    EXPECT_NEAR(weights_cpu[2], 0.5f, 1e-5f)
        << "Weight for edge (0,2) should be cot(45°)/2 = 0.5";

    EXPECT_NEAR(weights_cpu[4], 0.5f, 1e-5f) << "Weight for edge (1,0)";
    EXPECT_NEAR(weights_cpu[5], 0.0f, 1e-5f)
        << "Weight for edge (1,2) should be cot(90°)/2 = 0";

    EXPECT_NEAR(weights_cpu[7], 0.5f, 1e-5f) << "Weight for edge (2,0)";
    EXPECT_NEAR(weights_cpu[8], 0.0f, 1e-5f)
        << "Weight for edge (2,1) should be 0";
}

// Test FEM Laplace matrix with cotangent weights
TEST(LaplacianMatrix, FEM_RightTriangle)
{
    std::vector<float> vertices = {
        0.0f, 0.0f, 0.0f,  // v0
        1.0f, 0.0f, 0.0f,  // v1
        0.0f, 1.0f, 0.0f   // v2
    };

    std::vector<unsigned> faces = { 0, 1, 2 };

    std::vector<unsigned> adjacency_data = { 2, 1, 2, 2, 0, 2, 2, 0, 1 };
    std::vector<unsigned> offsets = { 0, 3, 6 };

    auto vertices_gpu = Ruzino::cuda::create_cuda_linear_buffer(vertices);
    auto faces_gpu = Ruzino::cuda::create_cuda_linear_buffer(faces);
    auto adj_gpu = Ruzino::cuda::create_cuda_linear_buffer(adjacency_data);
    auto offset_gpu = Ruzino::cuda::create_cuda_linear_buffer(offsets);
    auto adj_tuple = std::make_tuple(adj_gpu, offset_gpu);

    // Compute cotangent weights
    auto weights_gpu = rzsim_cuda::compute_cotangent_weights(
        vertices_gpu, faces_gpu, adj_tuple);

    // Build weighted Laplace matrix
    auto laplace_gpu = rzsim_cuda::build_laplace_matrix_weighted(
        adj_tuple, vertices_gpu, weights_gpu);
    auto triplets_cpu = laplace_gpu->get_host_vector<Triplet>();

    std::cout << "\n=== FEM Laplace Matrix (Right Triangle) ===\n";
    print_laplace_matrix(triplets_cpu, 3);

    // Verify diagonal entries (sum of cotangent weights)
    // v0: w(0,1) + w(0,2) = 0.5 + 0.5 = 1.0
    // v1: w(1,0) + w(1,2) = 0.5 + 0.0 = 0.5
    // v2: w(2,0) + w(2,1) = 0.5 + 0.0 = 0.5

    std::map<unsigned, float> expected_diag = { { 0, 1.0f },
                                                { 1, 0.5f },
                                                { 2, 0.5f } };
    for (const auto& t : triplets_cpu) {
        if (t.row == t.col && expected_diag.count(t.row)) {
            EXPECT_NEAR(t.value, expected_diag[t.row], 1e-5f)
                << "FEM diagonal at vertex " << t.row;
        }
    }
}

int main(int argc, char** argv)
{
    Ruzino::cuda::cuda_init();

    ::testing::InitGoogleTest(&argc, argv);
    int result = RUN_ALL_TESTS();

    Ruzino::cuda::cuda_shutdown();

    return result;
}
