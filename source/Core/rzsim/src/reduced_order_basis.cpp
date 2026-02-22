#include <GCore/Components/MeshComponent.h>
#include <GCore/util_openmesh_bind.h>
#include <Spectra/MatOp/SparseSymMatProd.h>
#include <Spectra/SymEigsSolver.h>
#include <igl/cotmatrix.h>
#include <rzsim/reduced_order_basis.h>
#include <spdlog/spdlog.h>

#include <Eigen/Eigenvalues>
#include <Eigen/Sparse>
#include <glm/glm.hpp>
#include <glm/gtx/norm.hpp>
#include <iostream>
#include <set>

RUZINO_NAMESPACE_OPEN_SCOPE

using glm::dvec3;
using PolyMesh = OpenMesh::PolyMesh_ArrayKernelT<>;
using VolumeMesh = OpenVolumeMesh::GeometricTetrahedralMeshV3d;

// AffineTransform implementation
AffineTransform::AffineTransform(int num_basis)
{
    transforms.resize(num_basis);
    // Initialize each basis with identity transform
    // Identity: R = I (9 values), t = 0 (3 values)
    for (int i = 0; i < num_basis; ++i) {
        transforms[i] = {
            1.0f, 0.0f, 0.0f,  // R00, R01, R02
            0.0f, 1.0f, 0.0f,  // R10, R11, R12
            0.0f, 0.0f, 1.0f,  // R20, R21, R22
            0.0f, 0.0f, 0.0f   // tx, ty, tz
        };
    }
}

void AffineTransform::set_transform(
    int mode_index,
    const std::vector<float>& transform)
{
    if (mode_index < 0 || mode_index >= static_cast<int>(transforms.size())) {
        throw std::out_of_range("Mode index out of range");
    }
    if (transform.size() != 12) {
        throw std::invalid_argument("Transform must have 12 elements");
    }
    transforms[mode_index] = transform;
}

const std::vector<float>& AffineTransform::get_transform(int mode_index) const
{
    if (mode_index < 0 || mode_index >= static_cast<int>(transforms.size())) {
        throw std::out_of_range("Mode index out of range");
    }
    return transforms[mode_index];
}

ReducedOrderedBasis::ReducedOrderedBasis(
    const Geometry& g,
    int num_modes,
    int dimension,
    bool use_libigl,
    const std::vector<int>& bc_vertices)
    : bc_vertices_(bc_vertices)
{
    // Get mesh component
    auto mesh_comp = g.get_component<MeshComponent>();
    if (!mesh_comp) {
        throw std::runtime_error("Geometry must have MeshComponent");
    }

    if (!bc_vertices_.empty()) {
        spdlog::info(
            "ReducedOrderedBasis: {} vertices marked with Dirichlet BC",
            bc_vertices_.size());
    }

    auto geom_ptr = const_cast<Geometry*>(&g);

    if (dimension == 3) {
        // 3D case: assemble Laplace operator for tetrahedral mesh
        auto volumemesh = operand_to_openvolumemesh(geom_ptr);
        if (!volumemesh || volumemesh->n_vertices() == 0) {
            throw std::runtime_error(
                "Invalid volume mesh for 3D reduced order basis");
        }
        spdlog::info(
            "Assembling 3D Laplacian for tetrahedral mesh (vertices={}, "
            "cells={}) using {}",
            volumemesh->n_vertices(),
            volumemesh->n_cells(),
            use_libigl ? "libigl" : "custom implementation");

        if (use_libigl) {
            assemble_laplacian_3d_libigl(volumemesh.get());
        }
        else {
            assemble_laplacian_3d(volumemesh.get());
        }
    }
    else if (dimension == 2) {
        // 2D case: assemble Laplace operator for surface mesh (triangles/quads)
        auto openmesh = operand_to_openmesh(geom_ptr);
        if (!openmesh || openmesh->n_vertices() == 0 ||
            openmesh->n_faces() == 0) {
            throw std::runtime_error(
                "Invalid surface mesh for 2D reduced order basis");
        }
        spdlog::info(
            "Assembling 2D Laplacian for surface mesh (vertices={}, faces={}) "
            "using {}",
            openmesh->n_vertices(),
            openmesh->n_faces(),
            use_libigl ? "libigl" : "custom implementation");

        if (use_libigl) {
            assemble_laplacian_2d_libigl(openmesh.get());
        }
        else {
            assemble_laplacian_2d(openmesh.get());
        }
    }
    else {
        throw std::runtime_error("Dimension must be 2 (surface) or 3 (volume)");
    }

    // Apply boundary conditions to Laplacian matrix before computing eigenmodes
    if (!bc_vertices_.empty()) {
        apply_bc_to_laplacian();
    }

    // Compute eigenmodes
    compute_eigenmodes(num_modes);
}

void ReducedOrderedBasis::assemble_laplacian_3d(void* mesh_ptr)
{
    auto mesh = static_cast<VolumeMesh*>(mesh_ptr);
    int n_vertices = mesh->n_vertices();

    // Triplets for sparse matrix assembly
    std::vector<Eigen::Triplet<double>> triplets;

    // Debug: check first tetrahedron
    bool first_tet = true;

    // Iterate over all cells (tetrahedra) and compute cotangent Laplacian
    for (auto c_it = mesh->cells_begin(); c_it != mesh->cells_end(); ++c_it) {
        // Get the four vertices of the tetrahedron
        std::vector<int> vertex_ids;
        std::vector<dvec3> positions;

        for (auto cv_it = mesh->cv_iter(*c_it); cv_it.valid(); ++cv_it) {
            vertex_ids.push_back((*cv_it).idx());
            auto pt = mesh->vertex(*cv_it);
            positions.push_back(dvec3(pt[0], pt[1], pt[2]));
        }

        if (vertex_ids.size() != 4)
            continue;  // Skip non-tetrahedral cells

        auto p0 = positions[0];
        auto p1 = positions[1];
        auto p2 = positions[2];
        auto p3 = positions[3];

        // Compute edge lengths following libigl's numbering:
        // L[0] = edge 3-0, L[1] = edge 3-1, L[2] = edge 3-2
        // L[3] = edge 1-2, L[4] = edge 2-0, L[5] = edge 0-1
        double l[6];
        l[0] = glm::length(p3 - p0);
        l[1] = glm::length(p3 - p1);
        l[2] = glm::length(p3 - p2);
        l[3] = glm::length(p1 - p2);
        l[4] = glm::length(p2 - p0);
        l[5] = glm::length(p0 - p1);

        // Compute face areas following libigl's numbering:
        // Face 0 (opposite vertex 0): triangle 1-2-3, uses edges l[1], l[2],
        // l[3] Face 1 (opposite vertex 1): triangle 0-2-3, uses edges l[0],
        // l[2], l[4] Face 2 (opposite vertex 2): triangle 0-1-3, uses edges
        // l[0], l[1], l[5] Face 3 (opposite vertex 3): triangle 0-1-2, uses
        // edges l[3], l[4], l[5]

        auto heron = [](double a, double b, double c) {
            double s = (a + b + c) / 2.0;
            double area_sq = s * (s - a) * (s - b) * (s - c);
            return std::sqrt(std::max(0.0, area_sq));
        };

        double s[4];  // face areas
        s[0] = heron(l[1], l[2], l[3]);
        s[1] = heron(l[0], l[2], l[4]);
        s[2] = heron(l[0], l[1], l[5]);
        s[3] = heron(l[3], l[4], l[5]);

        // Compute volume
        double vol =
            std::abs(glm::dot(p1 - p0, glm::cross(p2 - p0, p3 - p0))) / 6.0;

        if (vol < 1e-12)
            continue;

        // Compute H_sqr for dihedral angles using law of cosines
        // Following libigl's dihedral_angles_intrinsic.cpp EXACTLY
        double H_sqr[6];
        H_sqr[0] =
            (1.0 / 16.0) *
            (4.0 * l[3] * l[3] * l[0] * l[0] -
             ((l[1] * l[1] + l[4] * l[4]) - (l[2] * l[2] + l[5] * l[5])) *
                 ((l[1] * l[1] + l[4] * l[4]) - (l[2] * l[2] + l[5] * l[5])));
        H_sqr[1] =
            (1.0 / 16.0) *
            (4.0 * l[4] * l[4] * l[1] * l[1] -
             ((l[2] * l[2] + l[5] * l[5]) - (l[3] * l[3] + l[0] * l[0])) *
                 ((l[2] * l[2] + l[5] * l[5]) - (l[3] * l[3] + l[0] * l[0])));
        H_sqr[2] =
            (1.0 / 16.0) *
            (4.0 * l[5] * l[5] * l[2] * l[2] -
             ((l[3] * l[3] + l[0] * l[0]) - (l[4] * l[4] + l[1] * l[1])) *
                 ((l[3] * l[3] + l[0] * l[0]) - (l[4] * l[4] + l[1] * l[1])));
        H_sqr[3] =
            (1.0 / 16.0) *
            (4.0 * l[0] * l[0] * l[3] * l[3] -
             ((l[4] * l[4] + l[1] * l[1]) - (l[5] * l[5] + l[2] * l[2])) *
                 ((l[4] * l[4] + l[1] * l[1]) - (l[5] * l[5] + l[2] * l[2])));
        H_sqr[4] =
            (1.0 / 16.0) *
            (4.0 * l[1] * l[1] * l[4] * l[4] -
             ((l[5] * l[5] + l[2] * l[2]) - (l[0] * l[0] + l[3] * l[3])) *
                 ((l[5] * l[5] + l[2] * l[2]) - (l[0] * l[0] + l[3] * l[3])));
        H_sqr[5] =
            (1.0 / 16.0) *
            (4.0 * l[2] * l[2] * l[5] * l[5] -
             ((l[0] * l[0] + l[3] * l[3]) - (l[1] * l[1] + l[4] * l[4])) *
                 ((l[0] * l[0] + l[3] * l[3]) - (l[1] * l[1] + l[4] * l[4])));

        // Compute cos of dihedral angles
        double cos_theta[6];
        cos_theta[0] =
            (H_sqr[0] - s[1] * s[1] - s[2] * s[2]) / (-2.0 * s[1] * s[2]);
        cos_theta[1] =
            (H_sqr[1] - s[2] * s[2] - s[0] * s[0]) / (-2.0 * s[2] * s[0]);
        cos_theta[2] =
            (H_sqr[2] - s[0] * s[0] - s[1] * s[1]) / (-2.0 * s[0] * s[1]);
        cos_theta[3] =
            (H_sqr[3] - s[3] * s[3] - s[0] * s[0]) / (-2.0 * s[3] * s[0]);
        cos_theta[4] =
            (H_sqr[4] - s[3] * s[3] - s[1] * s[1]) / (-2.0 * s[3] * s[1]);
        cos_theta[5] =
            (H_sqr[5] - s[3] * s[3] - s[2] * s[2]) / (-2.0 * s[3] * s[2]);

        // Compute sin of dihedral angles using volume formula
        double sin_theta[6];
        sin_theta[0] = vol / ((2.0 / (3.0 * l[0])) * s[1] * s[2]);
        sin_theta[1] = vol / ((2.0 / (3.0 * l[1])) * s[2] * s[0]);
        sin_theta[2] = vol / ((2.0 / (3.0 * l[2])) * s[0] * s[1]);
        sin_theta[3] = vol / ((2.0 / (3.0 * l[3])) * s[3] * s[0]);
        sin_theta[4] = vol / ((2.0 / (3.0 * l[4])) * s[3] * s[1]);
        sin_theta[5] = vol / ((2.0 / (3.0 * l[5])) * s[3] * s[2]);

        // Compute cotangent weights: C = (1/6) * edge_length *
        // cot(dihedral_angle) Following libigl's cotmatrix_entries.cpp formula
        double C[6];
        for (int i = 0; i < 6; i++) {
            if (std::abs(sin_theta[i]) > 1e-12) {
                C[i] = (1.0 / 6.0) * l[i] * cos_theta[i] / sin_theta[i];
            }
            else {
                C[i] = 0.0;
            }
        }

        if (first_tet) {
            std::cout << "First tet vertex IDs: " << vertex_ids[0] << " "
                      << vertex_ids[1] << " " << vertex_ids[2] << " "
                      << vertex_ids[3] << std::endl;
            std::cout << "First tet vertex positions:" << std::endl;
            for (int i = 0; i < 4; i++) {
                std::cout << "  v" << i << ": (" << positions[i][0] << ", "
                          << positions[i][1] << ", " << positions[i][2] << ")"
                          << std::endl;
            }
            std::cout << "Edge lengths (custom): l[0]=" << l[0]
                      << " l[1]=" << l[1] << " l[2]=" << l[2]
                      << " l[3]=" << l[3] << " l[4]=" << l[4]
                      << " l[5]=" << l[5] << std::endl;
            std::cout << "Face areas (custom): s[0]=" << s[0]
                      << " s[1]=" << s[1] << " s[2]=" << s[2]
                      << " s[3]=" << s[3] << std::endl;
            std::cout << "Volume (custom): " << vol << std::endl;
            std::cout << "H_sqr: " << H_sqr[0] << " " << H_sqr[1] << " "
                      << H_sqr[2] << " " << H_sqr[3] << " " << H_sqr[4] << " "
                      << H_sqr[5] << std::endl;
            std::cout << "cos_theta: " << cos_theta[0] << " " << cos_theta[1]
                      << " " << cos_theta[2] << " " << cos_theta[3] << " "
                      << cos_theta[4] << " " << cos_theta[5] << std::endl;
            std::cout << "sin_theta: " << sin_theta[0] << " " << sin_theta[1]
                      << " " << sin_theta[2] << " " << sin_theta[3] << " "
                      << sin_theta[4] << " " << sin_theta[5] << std::endl;
            std::cout << "C (cotangent weights): " << C[0] << " " << C[1] << " "
                      << C[2] << " " << C[3] << " " << C[4] << " " << C[5]
                      << std::endl;
            first_tet = false;
        }

        // Add to Laplacian matrix following libigl's cotmatrix.cpp convention
        // IMPORTANT: edges are defined as [1-2, 2-0, 0-1, 3-0, 3-1, 3-2]
        // C values are computed in order [3-0, 3-1, 3-2, 1-2, 2-0, 0-1]
        // These are opposite edge pairs! C[i] for edge i is applied to its
        // opposite edge. This is correct for tetrahedral cotangent Laplacian:
        // use opposite edge cotangents.
        int edges[6][2] = {
            { 1, 2 },  // edge 0: vertices 1-2 uses C[0] (computed for opposite
                       // edge 3-0)
            { 2, 0 },  // edge 1: vertices 2-0 uses C[1] (computed for opposite
                       // edge 3-1)
            { 0, 1 },  // edge 2: vertices 0-1 uses C[2] (computed for opposite
                       // edge 3-2)
            { 3, 0 },  // edge 3: vertices 3-0 uses C[3] (computed for opposite
                       // edge 1-2)
            { 3, 1 },  // edge 4: vertices 3-1 uses C[4] (computed for opposite
                       // edge 2-0)
            { 3, 2 }
            // edge 5: vertices 3-2 uses C[5] (computed for opposite edge 0-1)
        };

        // Assembly: use C values directly (they correspond to opposite edges by
        // design)
        for (int e = 0; e < 6; e++) {
            int i = vertex_ids[edges[e][0]];
            int j = vertex_ids[edges[e][1]];
            double weight = C[e];  // Use C directly, NOT C_reordered!

            triplets.emplace_back(i, j, weight);
            triplets.emplace_back(j, i, weight);
            triplets.emplace_back(i, i, -weight);
            triplets.emplace_back(j, j, -weight);
        }
    }

    // Assemble global Laplacian matrix
    Eigen::SparseMatrix<double> laplacian_double(n_vertices, n_vertices);
    laplacian_double.setFromTriplets(triplets.begin(), triplets.end());
    laplacian_double.makeCompressed();

    // Negate to get positive semi-definite matrix
    laplacian_matrix_ = (-laplacian_double).cast<float>();

    std::cout << "Assembled 3D cotangent Laplacian matrix: " << n_vertices
              << " x " << n_vertices << " with " << laplacian_matrix_.nonZeros()
              << " non-zeros" << std::endl;
}

void ReducedOrderedBasis::compute_eigenmodes(int num_modes)
{
    int n = laplacian_matrix_.rows();

    if (num_modes > n) {
        std::cout << "Warning: Requested " << num_modes
                  << " modes but matrix only has " << n << " dimensions. Using "
                  << n << " modes instead." << std::endl;
        num_modes = n;
    }

    std::cout << "Computing " << num_modes
              << " eigenmodes using sparse solver (Spectra)..." << std::endl;

    // Convert to double precision sparse matrix for Spectra
    Eigen::SparseMatrix<double> laplacian_double =
        laplacian_matrix_.cast<double>();

    // Use Spectra to compute the smallest eigenvalues
    // We want the smallest eigenvalues in magnitude (closest to zero)
    Spectra::SparseSymMatProd<double> op(laplacian_double);

    // Request more eigenvalues than needed for better convergence
    int ncv = std::min(std::max(2 * num_modes + 1, 20), n);

    Spectra::SymEigsSolver<Spectra::SparseSymMatProd<double>> eigs(
        op, num_modes, ncv);

    // Initialize and compute
    // Use SmallestAlge to get smallest eigenvalues in algebraic order
    // (ascending)
    eigs.init();
    int nconv = eigs.compute(Spectra::SortRule::SmallestAlge, 4000, 1e-12);

    if (eigs.info() != Spectra::CompInfo::Successful) {
        std::cerr << "Spectra eigenvalue computation failed, falling back to "
                     "dense solver..."
                  << std::endl;

        // Fallback to dense solver
        Eigen::MatrixXd dense_laplacian = laplacian_double;
        Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> eigensolver(
            dense_laplacian);

        if (eigensolver.info() != Eigen::Success) {
            throw std::runtime_error(
                "Both sparse and dense eigenvalue decomposition failed");
        }

        Eigen::VectorXd all_eigenvalues = eigensolver.eigenvalues();
        Eigen::MatrixXd all_eigenvectors = eigensolver.eigenvectors();

        basis.clear();
        eigenvalues.clear();

        int actual_modes =
            std::min(num_modes, static_cast<int>(all_eigenvalues.size()));
        for (int i = 0; i < actual_modes; i++) {
            eigenvalues.push_back(static_cast<float>(all_eigenvalues(i)));
            Eigen::VectorXf eigenvector = all_eigenvectors.col(i).cast<float>();
            // Normalize the eigenvector
            float norm = eigenvector.norm();
            if (norm > 1e-12f) {
                eigenvector /= norm;
            }
            basis.push_back(eigenvector);
            std::cout << "  Mode " << i
                      << ": eigenvalue = " << all_eigenvalues(i) << std::endl;
        }
    }
    else {
        // Get eigenvalues and eigenvectors from Spectra
        Eigen::VectorXd eigenvalues_d = eigs.eigenvalues();
        Eigen::MatrixXd eigenvectors_d = eigs.eigenvectors();

        // Sort eigenvalues and eigenvectors in ascending order
        std::vector<std::pair<double, int>> sorted_indices;
        int actual_modes = std::min(nconv, num_modes);
        for (int i = 0; i < actual_modes; i++) {
            sorted_indices.push_back({ eigenvalues_d(i), i });
        }
        std::sort(sorted_indices.begin(), sorted_indices.end());

        basis.clear();
        eigenvalues.clear();

        for (int i = 0; i < actual_modes; i++) {
            int original_index = sorted_indices[i].second;
            double eigenvalue = sorted_indices[i].first;
            eigenvalues.push_back(static_cast<float>(eigenvalue));
            Eigen::VectorXf eigenvector =
                eigenvectors_d.col(original_index).cast<float>();
            // Normalize the eigenvector
            float norm = eigenvector.norm();
            if (norm > 1e-12f) {
                eigenvector /= norm;
            }
            basis.push_back(eigenvector);
            std::cout << "  Mode " << i << ": eigenvalue =" << eigenvalue
                      << std::endl;
        }
    }

    std::cout << "Eigenmode computation complete. Stored " << basis.size()
              << " basis vectors." << std::endl;
}

void ReducedOrderedBasis::assemble_laplacian_2d(void* mesh_ptr)
{
    auto mesh = static_cast<PolyMesh*>(mesh_ptr);
    int n_vertices = mesh->n_vertices();

    // Triplets for sparse matrix assembly
    std::vector<Eigen::Triplet<double>> triplets;

    // Iterate over all faces and compute cotangent Laplacian
    for (auto f_it : mesh->faces()) {
        // Get vertices of the face
        std::vector<int> vertex_ids;
        std::vector<dvec3> positions;

        for (auto fv_it : mesh->fv_range(f_it)) {
            vertex_ids.push_back(fv_it.idx());
            auto pt = mesh->point(fv_it);
            positions.push_back(dvec3(pt[0], pt[1], pt[2]));
        }

        int num_verts = vertex_ids.size();

        // Process triangles and quads (triangulate quads)
        if (num_verts == 3) {
            // Triangle - compute cotangent weights
            for (int i = 0; i < 3; i++) {
                int v0 = vertex_ids[i];
                int v1 = vertex_ids[(i + 1) % 3];
                int v2 = vertex_ids[(i + 2) % 3];

                auto p0 = positions[i];
                auto p1 = positions[(i + 1) % 3];
                auto p2 = positions[(i + 2) % 3];

                // Edge vectors
                auto e1 = p1 - p0;
                auto e2 = p2 - p0;

                // Compute cotangent of angle at v0
                double dot_prod = e1[0] * e2[0] + e1[1] * e2[1] + e1[2] * e2[2];
                double cross_x = e1[1] * e2[2] - e1[2] * e2[1];
                double cross_y = e1[2] * e2[0] - e1[0] * e2[2];
                double cross_z = e1[0] * e2[1] - e1[1] * e2[0];
                double cross_norm = std::sqrt(
                    cross_x * cross_x + cross_y * cross_y + cross_z * cross_z);

                if (cross_norm > 1e-12) {
                    double cot = dot_prod / cross_norm;

                    // Add cotangent weight to edge (v1, v2)
                    triplets.emplace_back(v1, v2, cot);
                    triplets.emplace_back(v2, v1, cot);
                    triplets.emplace_back(v1, v1, -cot);
                    triplets.emplace_back(v2, v2, -cot);
                }
            }
        }
        else if (num_verts == 4) {
            // Quad - split into two triangles and process each
            // Triangle 1: v0, v1, v2
            for (int tri = 0; tri < 2; tri++) {
                std::vector<int> tri_verts;
                std::vector<dvec3> tri_pos;

                if (tri == 0) {
                    tri_verts = { vertex_ids[0], vertex_ids[1], vertex_ids[2] };
                    tri_pos = { positions[0], positions[1], positions[2] };
                }
                else {
                    tri_verts = { vertex_ids[0], vertex_ids[2], vertex_ids[3] };
                    tri_pos = { positions[0], positions[2], positions[3] };
                }

                for (int i = 0; i < 3; i++) {
                    int v0 = tri_verts[i];
                    int v1 = tri_verts[(i + 1) % 3];
                    int v2 = tri_verts[(i + 2) % 3];

                    auto p0 = tri_pos[i];
                    auto p1 = tri_pos[(i + 1) % 3];
                    auto p2 = tri_pos[(i + 2) % 3];

                    auto e1 = p1 - p0;
                    auto e2 = p2 - p0;

                    double dot_prod =
                        e1[0] * e2[0] + e1[1] * e2[1] + e1[2] * e2[2];
                    double cross_x = e1[1] * e2[2] - e1[2] * e2[1];
                    double cross_y = e1[2] * e2[0] - e1[0] * e2[2];
                    double cross_z = e1[0] * e2[1] - e1[1] * e2[0];
                    double cross_norm = std::sqrt(
                        cross_x * cross_x + cross_y * cross_y +
                        cross_z * cross_z);

                    if (cross_norm > 1e-12) {
                        double cot = dot_prod / cross_norm;

                        triplets.emplace_back(v1, v2, cot);
                        triplets.emplace_back(v2, v1, cot);
                        triplets.emplace_back(v1, v1, -cot);
                        triplets.emplace_back(v2, v2, -cot);
                    }
                }
            }
        }
    }

    // Assemble global Laplacian matrix (use double precision)
    Eigen::SparseMatrix<double> laplacian_double(n_vertices, n_vertices);
    laplacian_double.setFromTriplets(triplets.begin(), triplets.end());
    laplacian_double.makeCompressed();

    // Convert to float, multiply by 0.5, and negate to get positive
    // semi-definite matrix Standard cotangent Laplacian is negative
    // semi-definite, we want eigenvalues >= 0
    laplacian_matrix_ = (-0.5 * laplacian_double).cast<float>();

    std::cout << "Assembled 2D cotangent Laplacian matrix: " << n_vertices
              << " x " << n_vertices << " with " << laplacian_matrix_.nonZeros()
              << " non-zeros" << std::endl;
}

void ReducedOrderedBasis::assemble_laplacian_2d_libigl(void* mesh_ptr)
{
    auto mesh = static_cast<PolyMesh*>(mesh_ptr);
    int n_vertices = mesh->n_vertices();

    // Convert OpenMesh to Eigen matrices for libigl
    Eigen::MatrixXd V(n_vertices, 3);
    std::vector<std::vector<int>> faces;

    // Extract vertices
    int v_idx = 0;
    for (auto v_it : mesh->vertices()) {
        auto pt = mesh->point(v_it);
        V(v_idx, 0) = pt[0];
        V(v_idx, 1) = pt[1];
        V(v_idx, 2) = pt[2];
        v_idx++;
    }

    // Extract faces (triangulate quads)
    for (auto f_it : mesh->faces()) {
        std::vector<int> face_verts;
        for (auto fv_it : mesh->fv_range(f_it)) {
            face_verts.push_back(fv_it.idx());
        }

        if (face_verts.size() == 3) {
            faces.push_back(face_verts);
        }
        else if (face_verts.size() == 4) {
            // Split quad into two triangles
            faces.push_back({ face_verts[0], face_verts[1], face_verts[2] });
            faces.push_back({ face_verts[0], face_verts[2], face_verts[3] });
        }
    }

    // Convert faces to Eigen matrix
    Eigen::MatrixXi F(faces.size(), 3);
    for (size_t i = 0; i < faces.size(); i++) {
        F(i, 0) = faces[i][0];
        F(i, 1) = faces[i][1];
        F(i, 2) = faces[i][2];
    }

    // Use libigl to compute cotangent Laplacian
    Eigen::SparseMatrix<double> L;
    igl::cotmatrix(V, F, L);

    // Negate to get positive semi-definite matrix (libigl's cotmatrix is
    // negative)
    laplacian_matrix_ = (-L).cast<float>();

    std::cout << "Assembled 2D cotangent Laplacian matrix using libigl: "
              << n_vertices << " x " << n_vertices << " with "
              << laplacian_matrix_.nonZeros() << " non-zeros" << std::endl;
}

void ReducedOrderedBasis::assemble_laplacian_3d_libigl(void* mesh_ptr)
{
    auto mesh = static_cast<VolumeMesh*>(mesh_ptr);
    int n_vertices = mesh->n_vertices();

    // Convert OpenVolumeMesh to Eigen matrices for libigl
    Eigen::MatrixXd V(n_vertices, 3);
    std::vector<std::vector<int>> cells;

    // Extract vertices
    int v_idx = 0;
    for (auto v_it = mesh->vertices_begin(); v_it != mesh->vertices_end();
         ++v_it) {
        auto pt = mesh->vertex(*v_it);
        V(v_idx, 0) = pt[0];
        V(v_idx, 1) = pt[1];
        V(v_idx, 2) = pt[2];
        v_idx++;
    }

    // Extract tetrahedral cells
    bool first_tet = true;
    for (auto c_it = mesh->cells_begin(); c_it != mesh->cells_end(); ++c_it) {
        std::vector<int> cell_verts;
        for (auto cv_it = mesh->cv_iter(*c_it); cv_it.valid(); ++cv_it) {
            cell_verts.push_back((*cv_it).idx());
        }
        if (cell_verts.size() == 4) {
            if (first_tet) {
                std::cout << "libigl first tet vertex IDs: " << cell_verts[0]
                          << " " << cell_verts[1] << " " << cell_verts[2] << " "
                          << cell_verts[3] << std::endl;
                std::cout << "libigl first tet vertex positions:" << std::endl;
                for (int i = 0; i < 4; i++) {
                    std::cout << "  v" << i << ": (" << V(cell_verts[i], 0)
                              << ", " << V(cell_verts[i], 1) << ", "
                              << V(cell_verts[i], 2) << ")" << std::endl;
                }
                first_tet = false;
            }
            cells.push_back(cell_verts);
        }
    }

    // Convert cells to Eigen matrix
    Eigen::MatrixXi T(cells.size(), 4);
    for (size_t i = 0; i < cells.size(); i++) {
        T(i, 0) = cells[i][0];
        T(i, 1) = cells[i][1];
        T(i, 2) = cells[i][2];
        T(i, 3) = cells[i][3];
    }

    // Use libigl to compute cotangent Laplacian for tetrahedral mesh
    Eigen::SparseMatrix<double> L;

    // DEBUG: Call cotmatrix_entries directly to see C values
    Eigen::MatrixXd C_libigl;
    igl::cotmatrix_entries(V, T, C_libigl);
    std::cout << "libigl cotmatrix_entries first row C values: ";
    for (int c = 0; c < 6; c++) {
        std::cout << C_libigl(0, c) << " ";
    }
    std::cout << std::endl;

    igl::cotmatrix(V, T, L);

    // Negate to get positive semi-definite matrix
    laplacian_matrix_ = (-L).cast<float>();
    // DEBUG: Print first few matrix elements
    std::cout << "First diagonal elements (libigl): ";
    for (int i = 0; i < std::min(5, static_cast<int>(laplacian_matrix_.rows()));
         i++) {
        std::cout << laplacian_matrix_.coeff(i, i) << " ";
    }
    std::cout << std::endl;
    std::cout << "Assembled 3D cotangent Laplacian matrix using libigl: "
              << n_vertices << " x " << n_vertices << " with "
              << laplacian_matrix_.nonZeros() << " non-zeros" << std::endl;
}

void ReducedOrderedBasis::apply_bc_to_laplacian()
{
    // Apply Dirichlet boundary conditions to Laplacian matrix
    // Similar to neo_hookean: BC rows get diagonal=1, off-diagonal=0
    // Free rows get BC columns=0

    spdlog::info(
        "Applying Dirichlet BC to Laplacian matrix for {} vertices",
        bc_vertices_.size());

    // Create a set for fast lookup
    std::set<int> bc_set(bc_vertices_.begin(), bc_vertices_.end());

    // Convert to triplet format for modification
    std::vector<Eigen::Triplet<float>> triplets;

    for (int k = 0; k < laplacian_matrix_.outerSize(); ++k) {
        for (Eigen::SparseMatrix<float>::InnerIterator it(laplacian_matrix_, k);
             it;
             ++it) {
            int row = it.row();
            int col = it.col();
            float value = it.value();

            bool row_is_bc = bc_set.count(row) > 0;
            bool col_is_bc = bc_set.count(col) > 0;

            if (row_is_bc) {
                // BC row: diagonal = 1, off-diagonal = 0
                if (row == col) {
                    triplets.push_back(Eigen::Triplet<float>(row, col, 1.0f));
                }
                // else: skip (set to 0)
            }
            else if (col_is_bc) {
                // Free row, BC column: set to 0
                // Skip this entry
            }
            else {
                // Free row, free column: keep original value
                triplets.push_back(Eigen::Triplet<float>(row, col, value));
            }
        }
    }

    // Rebuild the matrix
    int n = laplacian_matrix_.rows();
    laplacian_matrix_.resize(n, n);
    laplacian_matrix_.setFromTriplets(triplets.begin(), triplets.end());

    spdlog::info(
        "BC applied to Laplacian: {} non-zeros after modification",
        laplacian_matrix_.nonZeros());
}

RUZINO_NAMESPACE_CLOSE_SCOPE
