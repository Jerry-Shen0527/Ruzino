#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

#include "fem_bem/ElementBasis.hpp"
#include "fem_bem/api.h"
#include "fem_bem/fem_bem.hpp"

using namespace Ruzino;
int main()
{
    // Create a simple triangle: (0,0), (0,5), (1,0)
    std::vector<std::vector<double>> vertices = {
        { 0.0, 0.0 },  // v0
        { 0.0, 5.0 },  // v1
        { 1.0, 0.0 }   // v2
    };

    // Create 2D finite element basis
    auto basis = fem_bem::make_fem_2d();

    // Add linear shape functions
    basis->add_vertex_expression("1 - u1 - u2");  // N0 = 1 - u1 - u2
    basis->add_vertex_expression("u1");           // N1 = u1
    basis->add_vertex_expression("u2");           // N2 = u2

    auto expressions = basis->get_vertex_expressions();

    // Compute gradients in reference coordinates
    auto grad_N0 = expressions[0].gradient({ "u1", "u2" });
    auto grad_N1 = expressions[1].gradient({ "u1", "u2" });
    auto grad_N2 = expressions[2].gradient({ "u1", "u2" });

    // Compute Jacobian transformation
    // d1 = v1 - v0, d2 = v2 - v0
    auto d1_x = vertices[1][0] - vertices[0][0];
    auto d1_y = vertices[1][1] - vertices[0][1];
    auto d2_x = vertices[2][0] - vertices[0][0];
    auto d2_y = vertices[2][1] - vertices[0][1];

    // Jacobian determinant
    auto jac_det = d1_x * d2_y - d1_y * d2_x;
    auto det_sq = jac_det * jac_det;

    // Inverse Jacobian squared elements
    auto j00 = (d2_x * d2_x + d2_y * d2_y) / det_sq;
    auto j01 = -(d2_x * d2_x + d1_y * d1_y) / det_sq;
    auto j10 = j01;
    auto j11 = (d1_x * d1_x + d1_y * d1_y) / det_sq;

    // Triangle area
    auto triangle_area = std::abs(jac_det) / 2.0;

    // Create final expressions for gradient inner products
    fem_bem::Expression final_expressions[9];  // 3x3 matrix

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            // Get gradients based on index
            auto& grad_i = (i == 0) ? grad_N0 : (i == 1) ? grad_N1 : grad_N2;
            auto& grad_j = (j == 0) ? grad_N0 : (j == 1) ? grad_N1 : grad_N2;

            // ∇Ni · G · ∇Nj where G is the metric tensor
            final_expressions[i * 3 + j] =
                grad_i[0] * fem_bem::Expression("j00") * grad_j[0] +
                grad_i[0] * fem_bem::Expression("j01") * grad_j[1] +
                grad_i[1] * fem_bem::Expression("j10") * grad_j[0] +
                grad_i[1] * fem_bem::Expression("j11") * grad_j[1];
        }
    }

    // Compute stiffness matrix entries and verify they are computed correctly
    double K[3][3];

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            // Bind the metric tensor values
            final_expressions[i * 3 + j].bind_variables(
                { { "j00", j00 },
                  { "j01", j01 },
                  { "j10", j10 },
                  { "j11", j11 } });

            // Integrate over reference triangle
            auto integrated = fem_bem::integrate_over_simplex(
                final_expressions[i * 3 + j], { "u1", "u2" }, nullptr, 2);

            // Scale by triangle area
            K[i][j] = integrated * triangle_area;
        }
    }

    // Verify matrix is symmetric (key property of stiffness matrices)
    for (int i = 0; i < 3; i++) {
        for (int j = i + 1; j < 3; j++) {
            if (std::abs(K[i][j] - K[j][i]) > 1e-6) {
                std::cout << "FAILED: Stiffness matrix is not symmetric"
                          << std::endl;
                return 1;
            }
        }
    }

    // Verify rows sum to zero (constant strain condition)
    for (int i = 0; i < 3; i++) {
        double row_sum = K[i][0] + K[i][1] + K[i][2];
        if (std::abs(row_sum) > 1e-5) {
            std::cout << "FAILED: Stiffness matrix rows do not sum to zero"
                      << std::endl;
            return 1;
        }
    }

    return 0;
}
