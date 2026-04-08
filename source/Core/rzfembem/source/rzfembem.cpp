#include <Eigen/Sparse>
#include <iostream>
#include <map>
#include <set>

#include "GCore/Components/MeshComponent.h"
#include "GCore/util_openmesh_bind.h"
#include "RZSolver/Solver.hpp"
#include "fem_bem/ElementBasis.hpp"
#include "fem_bem/api.h"
#include "fem_bem/fem_bem.hpp"

RUZINO_NAMESPACE_OPEN_SCOPE

class FEMSolver2D : public ElementSolver {
   public:
    FEMSolver2D(const ElementSolverDesc& desc) : desc_(desc)
    {
        // Create 2D finite element basis with P- family
        basis_ = fem_bem::make_fem_2d();

        // For P_minus with k=0 (H1 space), we need linear shape functions on
        // triangle vertices P-1Λ0 space has linear basis functions: u1, u2, u3
        // where u3 = 1 - u1 - u2
        basis_->add_vertex_expression(
            "1 - u1 - u2");                   // shape function at vertex 2
        basis_->add_vertex_expression("u1");  // shape function at vertex 0
        basis_->add_vertex_expression("u2");  // shape function at vertex 1
    }

    void set_geometry(const Geometry& geom) override
    {
        geometry_ = geom;

        // Get mesh component
        mesh_comp_ = geometry_.get_component<MeshComponent>();
        if (!mesh_comp_) {
            throw std::runtime_error("Geometry must have MeshComponent");
        }

        // Convert to OpenMesh for half-edge operations
        auto geom_ptr = const_cast<Geometry*>(&geometry_);
        openmesh_ = operand_to_openmesh(geom_ptr);

        // Extract mesh connectivity
        extract_mesh_data();
    }

    unsigned get_boundary_count() const override
    {
        return 1;  // Single boundary for now
    }

    void set_boundary_condition(
        const std::string& expr,
        BoundaryCondition type,
        unsigned boundary_id) override
    {
        if (type != BoundaryCondition::Dirichlet) {
            throw std::runtime_error(
                "Only Dirichlet boundary conditions supported");
        }
        boundary_expr_ = expr;
    }

    std::vector<float> solve() override
    {
        // Assemble system matrix and RHS
        auto [A, b] = assemble_system();

        // Solve linear system
        auto solution = solve_linear_system(A, b);

        return solution;
    }

   private:
    ElementSolverDesc desc_;
    Geometry geometry_;
    std::shared_ptr<MeshComponent> mesh_comp_;
    std::shared_ptr<PolyMesh> openmesh_;
    fem_bem::ElementBasisHandle basis_;
    std::string boundary_expr_;

    void extract_mesh_data()
    {
        if (!mesh_comp_)
            return;
        openmesh_ = operand_to_openmesh(&geometry_);
    }

    std::pair<Eigen::SparseMatrix<float>, Eigen::VectorXf> assemble_system()
    {
        int n_vertices = openmesh_->n_vertices();

        // Initialize sparse matrix and RHS vector
        Eigen::SparseMatrix<float> A(n_vertices, n_vertices);
        Eigen::VectorXf b = Eigen::VectorXf::Zero(n_vertices);

        // Triplets for sparse matrix assembly
        std::vector<Eigen::Triplet<float>> triplets;

        // First deal with the vertex based element.
        auto expressions = basis_->get_vertex_expressions();

        auto first = expressions[0];
        auto gradient_first = first.gradient({ "u1", "u2" });

        auto coor1 = expressions[1];
        auto gradient_coor1 = coor1.gradient({ "u1", "u2" });
        auto coor2 = expressions[2];
        auto gradient_coor2 = coor2.gradient({ "u1", "u2" });

        fem_bem::Expression final_expressions[3];

        final_expressions[0] =
            gradient_first[0] * fem_bem::Expression("j00") * gradient_first[0] +
            gradient_first[1] * fem_bem::Expression("j01") * gradient_first[0] +
            gradient_first[0] * fem_bem::Expression("j10") * gradient_first[1] +
            gradient_first[1] * fem_bem::Expression("j11") * gradient_first[1];

        final_expressions[1] =
            gradient_first[0] * fem_bem::Expression("j00") * gradient_coor1[0] +
            gradient_first[1] * fem_bem::Expression("j01") * gradient_coor1[0] +
            gradient_first[0] * fem_bem::Expression("j10") * gradient_coor1[1] +
            gradient_first[1] * fem_bem::Expression("j11") * gradient_coor1[1];

        final_expressions[2] =
            gradient_first[0] * fem_bem::Expression("j00") * gradient_coor2[0] +
            gradient_first[1] * fem_bem::Expression("j01") * gradient_coor2[0] +
            gradient_first[0] * fem_bem::Expression("j10") * gradient_coor2[1] +
            gradient_first[1] * fem_bem::Expression("j11") * gradient_coor2[1];

        for (auto v_it : openmesh_->vertices()) {
            // This id
            int vertex_id = v_it.idx();  // matrix id i
            // if this vertex on the boundary, then delta_ij in the matrix, and
            // RHS = boundary_value
            if (openmesh_->is_boundary(v_it)) {
                float boundary_value = get_boundary_value(vertex_id);
                triplets.emplace_back(vertex_id, vertex_id, 1.0f);
                b[vertex_id] = boundary_value;
                continue;
            }

            // Otherwise, first find all connected faces
            for (auto f_it : openmesh_->vf_range(v_it)) {
                // Then, get the other two counter-clockwise vertex ids within
                // the face.
                auto face_vertices = openmesh_->fv_range(f_it);
                std::vector<pxr::GfVec2d> tri_verts;
                std::vector<int> face_vertex_ids;

                std::vector<int> ordered_vertices;
                for (auto fv_it : face_vertices) {
                    ordered_vertices.push_back(fv_it.idx());
                    tri_verts.push_back(
                        pxr::GfVec2d(
                            openmesh_->point(fv_it)[0],
                            openmesh_->point(fv_it)[1]));
                }

                // Find the position of current vertex in the face
                int vertex_pos = -1;
                for (int i = 0; i < 3; i++) {
                    if (ordered_vertices[i] == vertex_id) {
                        vertex_pos = i;
                        break;
                    }
                }

                // Get the other two vertices in counter-clockwise order
                int next1 = (vertex_pos + 1) % 3;
                int next2 = (vertex_pos + 2) % 3;
                face_vertex_ids.push_back(ordered_vertices[next1]);
                face_vertex_ids.push_back(ordered_vertices[next2]);

                // Now we assemble the stiffness matrix and load vector for the
                // triangle

                assert(expressions.size() == 3);
                auto triangle_area = compute_triangle_area(tri_verts);

                // Calc Square of Inverse Jacobian Matrix
                auto v0 = openmesh_->point(openmesh_->vertex_handle(vertex_id));
                auto v1 = openmesh_->point(
                    openmesh_->vertex_handle(face_vertex_ids[0]));
                auto v2 = openmesh_->point(
                    openmesh_->vertex_handle(face_vertex_ids[1]));

                auto d1 = v1 - v0;
                auto d2 = v2 - v0;

                // Jacobian matrix elements
                auto jac_det = d1[0] * d2[1] - d1[1] * d2[0];
                auto det_sq = jac_det * jac_det;

                // Inverse Jacobian squared elements
                auto j00 = (d2[0] * d2[0] + d2[1] * d2[1]) / det_sq;
                auto j01 = -(d2[0] * d1[0] + d2[1] * d1[1]) / det_sq;
                auto j10 = j01;
                auto j11 = (d1[0] * d1[0] + d1[1] * d1[1]) / det_sq;

                auto calc_inner_product =
                    [j00, j01, j10, j11, &final_expressions](int id) {
                        if (id < 3)
                            final_expressions[id].bind_variables(
                                {
                                    { "j00", j00 },
                                    { "j01", j01 },
                                    { "j10", j10 },
                                    { "j11", j11 },
                                });
                        auto integrated = fem_bem::integrate_over_simplex(
                            final_expressions[id], { "u1", "u2" }, nullptr, 2);
                        return integrated;
                    };

                int face_id;
                if (vertex_id == 67 && face_vertex_ids[0] == 72)
                    face_id = f_it.idx();
                if (vertex_id == 72 && face_vertex_ids[1] == 67)
                    face_id = f_it.idx();
                triplets.emplace_back(
                    vertex_id,
                    vertex_id,
                    calc_inner_product(0) * triangle_area);
                triplets.emplace_back(
                    vertex_id,
                    face_vertex_ids[0],
                    calc_inner_product(1) * triangle_area);
                triplets.emplace_back(
                    vertex_id,
                    face_vertex_ids[1],
                    calc_inner_product(2) * triangle_area);
            }
        }

        A.setFromTriplets(triplets.begin(), triplets.end());
        A.makeCompressed();
        return { A, b };
    }

    float get_boundary_value(int vertex_id)
    {
        // Evaluate boundary expression at vertex
        auto& vertex = openmesh_->point(openmesh_->vertex_handle(vertex_id));
        double x = vertex[0], y = vertex[1];

        fem_bem::Expression boundary_func(boundary_expr_);
        return static_cast<float>(
            boundary_func.evaluate_at({ { "x", x }, { "y", y } }));
    }

    float compute_triangle_area(const std::vector<pxr::GfVec2d>& tri_verts)
    {
        double x1 = tri_verts[0][0], y1 = tri_verts[0][1];
        double x2 = tri_verts[1][0], y2 = tri_verts[1][1];
        double x3 = tri_verts[2][0], y3 = tri_verts[2][1];

        return static_cast<float>(
            0.5 * std::abs((x2 - x1) * (y3 - y1) - (x3 - x1) * (y2 - y1)));
    }

    std::vector<float> solve_linear_system(
        const Eigen::SparseMatrix<float>& A,
        const Eigen::VectorXf& b)
    {
        // Use iterative solver for sparse system
        auto solver = Solver::SolverFactory::create(
            Solver::SolverType::EIGEN_ITERATIVE_BICGSTAB);

        Eigen::VectorXf x = Eigen::VectorXf::Zero(b.size());

        Solver::SolverConfig config;
        config.tolerance = 1e-6f;
        config.max_iterations = 1000;
        config.verbose = true;

        auto result = solver->solve(A, b, x, config);

        if (!result.converged) {
            std::cerr
                << "Warning: Linear solver did not converge. Final residual: "
                << result.final_residual << std::endl;
        }

        // Convert to std::vector
        std::vector<float> solution(x.size());
        for (int i = 0; i < x.size(); i++) {
            solution[i] = x[i];
        }

        return solution;
    }
};

class FEMSolver3D : public ElementSolver {
   public:
    FEMSolver3D(const ElementSolverDesc& desc) : desc_(desc)
    {
        // Create 3D finite element basis with P- family
        basis_ = fem_bem::make_fem_3d();

        // For P_minus with k=0 (H1 space), we need linear shape functions on
        // tetrahedron vertices P-1Λ0 space has linear basis functions: u1, u2,
        // u3, u4 where u4 = 1 - u1 - u2 - u3
        basis_->add_vertex_expression(
            "1 - u1 - u2 - u3");              // shape function at vertex 3
        basis_->add_vertex_expression("u1");  // shape function at vertex 0
        basis_->add_vertex_expression("u2");  // shape function at vertex 1
        basis_->add_vertex_expression("u3");  // shape function at vertex 2
    }

    void set_geometry(const Geometry& geom) override
    {
        geometry_ = geom;

        // Get mesh component
        mesh_comp_ = geometry_.get_component<MeshComponent>();
        if (!mesh_comp_) {
            throw std::runtime_error("Geometry must have MeshComponent");
        }

        // Extract mesh connectivity
        extract_mesh_data();
    }

    unsigned get_boundary_count() const override
    {
        return 1;  // Single boundary for now
    }

    void set_boundary_condition(
        const std::string& expr,
        BoundaryCondition type,
        unsigned boundary_id) override
    {
        if (type != BoundaryCondition::Dirichlet) {
            throw std::runtime_error(
                "Only Dirichlet boundary conditions supported");
        }
        boundary_expr_ = expr;
    }

    std::vector<float> solve() override
    {
        // Assemble system matrix and RHS
        auto [A, b] = assemble_system();

        // Solve linear system
        auto solution = solve_linear_system(A, b);

        return solution;
    }

   private:
    ElementSolverDesc desc_;
    Geometry geometry_;
    std::shared_ptr<MeshComponent> mesh_comp_;
    std::shared_ptr<VolumeMesh> volumemesh_;
    fem_bem::ElementBasisHandle basis_;
    std::string boundary_expr_;

    void extract_mesh_data()
    {
        if (!mesh_comp_)
            return;
        auto geom_ptr = const_cast<Geometry*>(&geometry_);
        volumemesh_ = operand_to_openvolumemesh(geom_ptr);

        // Validate the volume mesh
        //        validate_volume_mesh();
    }

    void validate_volume_mesh()
    {
        if (!volumemesh_) {
            throw std::runtime_error("Volume mesh is not initialized");
        }

        if (volumemesh_->n_vertices() == 0) {
            throw std::runtime_error("Volume mesh has no vertices");
        }

        if (volumemesh_->n_cells() == 0) {
            throw std::runtime_error("Volume mesh has no cells");
        }

        // Check that all cells are tetrahedra
        for (auto c_it = volumemesh_->cells_begin();
             c_it != volumemesh_->cells_end();
             ++c_it) {
            int vertex_count = 0;
            for (auto cv_it = volumemesh_->cv_iter(*c_it); cv_it.valid();
                 ++cv_it) {
                vertex_count++;
            }
            if (vertex_count != 4) {
                throw std::runtime_error(
                    "Non-tetrahedral cell found in volume mesh");
            }
        }

        // Check that all vertex handles are valid
        for (int i = 0; i < volumemesh_->n_vertices(); ++i) {
            auto vh = OpenVolumeMesh::VertexHandle(i);
            if (!vh.is_valid()) {
                throw std::runtime_error("Invalid vertex handle found");
            }
        }
    }

    std::pair<Eigen::SparseMatrix<float>, Eigen::VectorXf> assemble_system()
    {
        int n_vertices = volumemesh_->n_vertices();

        if (n_vertices == 0) {
            throw std::runtime_error("Volume mesh has no vertices");
        }

        // Initialize sparse matrix and RHS vector
        Eigen::SparseMatrix<float> A(n_vertices, n_vertices);
        Eigen::VectorXf b = Eigen::VectorXf::Zero(n_vertices);

        // Triplets for sparse matrix assembly
        std::vector<Eigen::Triplet<float>> triplets;

        // Get vertex expressions for 3D tetrahedra
        auto expressions = basis_->get_vertex_expressions();
        assert(expressions.size() == 4);

        auto first = expressions[0];
        auto gradient_first = first.gradient({ "u1", "u2", "u3" });

        auto coor1 = expressions[1];
        auto gradient_coor1 = coor1.gradient({ "u1", "u2", "u3" });
        auto coor2 = expressions[2];
        auto gradient_coor2 = coor2.gradient({ "u1", "u2", "u3" });
        auto coor3 = expressions[3];
        auto gradient_coor3 = coor3.gradient({ "u1", "u2", "u3" });

        fem_bem::Expression final_expressions[4];

        // Build stiffness matrix expressions for 3D
        for (int i = 0; i < 4; i++) {
            auto& grad = (i == 0)   ? gradient_first
                         : (i == 1) ? gradient_coor1
                         : (i == 2) ? gradient_coor2
                                    : gradient_coor3;

            final_expressions[i] =
                gradient_first[0] * fem_bem::Expression("j00") * grad[0] +
                gradient_first[0] * fem_bem::Expression("j01") * grad[1] +
                gradient_first[0] * fem_bem::Expression("j02") * grad[2] +
                gradient_first[1] * fem_bem::Expression("j10") * grad[0] +
                gradient_first[1] * fem_bem::Expression("j11") * grad[1] +
                gradient_first[1] * fem_bem::Expression("j12") * grad[2] +
                gradient_first[2] * fem_bem::Expression("j20") * grad[0] +
                gradient_first[2] * fem_bem::Expression("j21") * grad[1] +
                gradient_first[2] * fem_bem::Expression("j22") * grad[2];
        }

        for (int vertex_id = 0; vertex_id < n_vertices; ++vertex_id) {
            auto vh = OpenVolumeMesh::VertexHandle(vertex_id);

            // Check if this vertex is on boundary
            if (is_boundary_vertex(vh)) {
                float boundary_value = get_boundary_value(vertex_id);
                triplets.emplace_back(vertex_id, vertex_id, 1.0f);
                b[vertex_id] = boundary_value;
                continue;
            }

            // Iterate through all cells (tetrahedra) connected to this vertex
            for (auto vc_it = volumemesh_->vc_iter(vh); vc_it.valid();
                 ++vc_it) {
                std::vector<int> tet_vertex_ids;
                std::vector<pxr::GfVec3d> tet_coords;

                // Get vertices of the cell
                for (auto cv_it = volumemesh_->cv_iter(*vc_it); cv_it.valid();
                     ++cv_it) {
                    tet_vertex_ids.push_back((*cv_it).idx());
                    auto point = volumemesh_->vertex(*cv_it);
                    tet_coords.push_back(
                        pxr::GfVec3d(point[0], point[1], point[2]));
                }

                // Validate tetrahedron geometry
                if (!validate_tetrahedron(tet_coords)) {
                    // Skip degenerate tetrahedra
                    continue;
                }

                // Find position of current vertex in tetrahedron
                int vertex_pos = -1;
                for (int i = 0; i < 4; i++) {
                    if (tet_vertex_ids[i] == vertex_id) {
                        vertex_pos = i;
                        break;
                    }
                }

                if (vertex_pos == -1) {
                    // This should never happen if the mesh is constructed
                    // correctly
                    continue;
                }

                // Get the other three vertices
                std::vector<int> other_vertices;
                for (int i = 0; i < 4; i++) {
                    if (i != vertex_pos) {
                        other_vertices.push_back(tet_vertex_ids[i]);
                    }
                }

                // Calculate tetrahedron volume and Jacobian
                auto tet_volume = compute_tetrahedron_volume(tet_coords);
                auto [j00, j01, j02, j10, j11, j12, j20, j21, j22] =
                    compute_inverse_jacobian_squared(tet_coords, vertex_pos);

                auto calc_inner_product = [&](int id) {
                    final_expressions[id].bind_variables(
                        { { "j00", j00 },
                          { "j01", j01 },
                          { "j02", j02 },
                          { "j10", j10 },
                          { "j11", j11 },
                          { "j12", j12 },
                          { "j20", j20 },
                          { "j21", j21 },
                          { "j22", j22 } });
                    return fem_bem::integrate_over_simplex(
                        final_expressions[id],
                        { "u1", "u2", "u3" },
                        nullptr,
                        3);
                };

                // Add contributions to stiffness matrix
                triplets.emplace_back(
                    vertex_id, vertex_id, calc_inner_product(0) * tet_volume);
                for (int i = 0; i < 3; i++) {
                    triplets.emplace_back(
                        vertex_id,
                        other_vertices[i],
                        calc_inner_product(i + 1) * tet_volume);
                }
            }
        }

        A.setFromTriplets(triplets.begin(), triplets.end());
        A.makeCompressed();
        return { A, b };
    }

    bool is_boundary_vertex(const OpenVolumeMesh::VertexHandle& vh)
    {
        // A vertex is on boundary if it belongs to at least one boundary face
        for (auto vf_it = volumemesh_->vf_iter(vh); vf_it.valid(); ++vf_it) {
            if (volumemesh_->is_boundary(*vf_it)) {
                return true;
            }
        }
        return false;
    }

    float get_boundary_value(int vertex_id)
    {
        auto vertex =
            volumemesh_->vertex(OpenVolumeMesh::VertexHandle(vertex_id));
        double x = vertex[0], y = vertex[1], z = vertex[2];

        fem_bem::Expression boundary_func(boundary_expr_);
        return static_cast<float>(
            boundary_func.evaluate_at({ { "x", x }, { "y", y }, { "z", z } }));
    }

    float compute_tetrahedron_volume(
        const std::vector<pxr::GfVec3d>& tet_coords)
    {
        auto& v0 = tet_coords[0];
        auto& v1 = tet_coords[1];
        auto& v2 = tet_coords[2];
        auto& v3 = tet_coords[3];

        auto d1 = v1 - v0;
        auto d2 = v2 - v0;
        auto d3 = v3 - v0;

        double det = d1[0] * (d2[1] * d3[2] - d2[2] * d3[1]) -
                     d1[1] * (d2[0] * d3[2] - d2[2] * d3[0]) +
                     d1[2] * (d2[0] * d3[1] - d2[1] * d3[0]);

        return static_cast<float>(std::abs(det) / 6.0);
    }

    std::tuple<
        double,
        double,
        double,
        double,
        double,
        double,
        double,
        double,
        double>
    compute_inverse_jacobian_squared(
        const std::vector<pxr::GfVec3d>& tet_coords,
        int vertex_pos)
    {
        // Reorder vertices so that vertex_pos is at position 0
        auto v0 = tet_coords[vertex_pos];
        std::vector<pxr::GfVec3d> others;
        for (int i = 0; i < 4; i++) {
            if (i != vertex_pos) {
                others.push_back(tet_coords[i]);
            }
        }

        auto d1 = others[0] - v0;
        auto d2 = others[1] - v0;
        auto d3 = others[2] - v0;

        // Compute Jacobian determinant
        double det = d1[0] * (d2[1] * d3[2] - d2[2] * d3[1]) -
                     d1[1] * (d2[0] * d3[2] - d2[2] * d3[0]) +
                     d1[2] * (d2[0] * d3[1] - d2[1] * d3[0]);

        if (std::abs(det) < 1e-12) {
            throw std::runtime_error("Singular Jacobian matrix in tetrahedron");
        }

        double det_sq = det * det;

        // Compute the inverse of J^T * J for the shape function gradients
        // For a tetrahedron, this is more complex than the simplified version
        // below Here's a proper implementation for the metric tensor (J^T *
        // J)^-1

        // J = [d1 d2 d3] (3x3 matrix)
        // J^T * J = [[d1·d1, d1·d2, d1·d3],
        //            [d2·d1, d2·d2, d2·d3],
        //            [d3·d1, d3·d2, d3·d3]]

        double g11 = d1[0] * d1[0] + d1[1] * d1[1] + d1[2] * d1[2];
        double g12 = d1[0] * d2[0] + d1[1] * d2[1] + d1[2] * d2[2];
        double g13 = d1[0] * d3[0] + d1[1] * d3[1] + d1[2] * d3[2];
        double g22 = d2[0] * d2[0] + d2[1] * d2[1] + d2[2] * d2[2];
        double g23 = d2[0] * d3[0] + d2[1] * d3[1] + d2[2] * d3[2];
        double g33 = d3[0] * d3[0] + d3[1] * d3[1] + d3[2] * d3[2];

        // Compute determinant of metric tensor
        double g_det = g11 * (g22 * g33 - g23 * g23) -
                       g12 * (g12 * g33 - g13 * g23) +
                       g13 * (g12 * g23 - g13 * g22);

        if (std::abs(g_det) < 1e-12) {
            throw std::runtime_error("Singular metric tensor in tetrahedron");
        }

        // Compute inverse of metric tensor
        double j00 = (g22 * g33 - g23 * g23) / g_det;
        double j01 = (g13 * g23 - g12 * g33) / g_det;
        double j02 = (g12 * g23 - g13 * g22) / g_det;
        double j10 = j01;  // symmetric
        double j11 = (g11 * g33 - g13 * g13) / g_det;
        double j12 = (g12 * g13 - g11 * g23) / g_det;
        double j20 = j02;  // symmetric
        double j21 = j12;  // symmetric
        double j22 = (g11 * g22 - g12 * g12) / g_det;

        return { j00, j01, j02, j10, j11, j12, j20, j21, j22 };
    }

    bool validate_tetrahedron(const std::vector<pxr::GfVec3d>& tet_coords)
    {
        if (tet_coords.size() != 4) {
            return false;
        }

        // Check for degenerate tetrahedron by computing volume
        auto& v0 = tet_coords[0];
        auto& v1 = tet_coords[1];
        auto& v2 = tet_coords[2];
        auto& v3 = tet_coords[3];

        auto d1 = v1 - v0;
        auto d2 = v2 - v0;
        auto d3 = v3 - v0;

        double det = d1[0] * (d2[1] * d3[2] - d2[2] * d3[1]) -
                     d1[1] * (d2[0] * d3[2] - d2[2] * d3[0]) +
                     d1[2] * (d2[0] * d3[1] - d2[1] * d3[0]);

        // Volume should be positive (within tolerance)
        return std::abs(det) > 1e-12;
    }

    std::vector<float> solve_linear_system(
        const Eigen::SparseMatrix<float>& A,
        const Eigen::VectorXf& b)
    {
        auto solver = Solver::SolverFactory::create(
            Solver::SolverType::EIGEN_ITERATIVE_BICGSTAB);

        Eigen::VectorXf x = Eigen::VectorXf::Zero(b.size());

        Solver::SolverConfig config;
        config.tolerance = 1e-6f;
        config.max_iterations = 1000;
        config.verbose = true;

        auto result = solver->solve(A, b, x, config);

        if (!result.converged) {
            std::cerr
                << "Warning: Linear solver did not converge. Final residual: "
                << result.final_residual << std::endl;
        }

        std::vector<float> solution(x.size());
        for (int i = 0; i < x.size(); i++) {
            solution[i] = x[i];
        }

        return solution;
    }
};

std::shared_ptr<ElementSolver> create_element_solver(
    const ElementSolverDesc& desc)
{
    if (desc.get_problem_dim() != 2 && desc.get_problem_dim() != 3) {
        throw std::runtime_error("Only 2D and 3D problems supported");
    }

    if (desc.get_element_family() != ElementFamily::P_minus) {
        throw std::runtime_error("Only P_minus element family supported");
    }

    if (desc.get_equation_type() != EquationType::Laplacian) {
        throw std::runtime_error("Only Laplacian equation supported");
    }

    if (desc.get_problem_dim() == 2) {
        return std::make_shared<FEMSolver2D>(desc);
    }
    else {
        return std::make_shared<FEMSolver3D>(desc);
    }
}

RUZINO_NAMESPACE_CLOSE_SCOPE
