// Bristle density constraint — Wetbrush Section 4.1
// Position-Based Fluids (Macklin & Müller 2013) applied to bristle vertices.
// Prevents bristles from clustering by enforcing a maximal density constraint.
//
// Two-pass dispatch:
//   mode=0: compute density ρ and Lagrange multiplier λ per vertex
//   mode=1: apply position correction using λ values

#include "common.slangh"

struct BristleV2 { float2 pos, vel; };
struct ConstraintModeCB { int mode; float3 _pad; };

ConstantBuffer<BristleConstants> cb : register(b0);
RWStructuredBuffer<BristleV2> bristle_data : register(u0);  // Nb * M
RWStructuredBuffer<float> lambda_buf : register(u1);        // Nb * M (temp)
ConstantBuffer<ConstraintModeCB> mode_cb : register(b1);

// Poly6 kernel (2D): W(r, h) = (4 / π h^8) * (h² - r²)³
float W_poly6(float r2, float h2)
{
    if (r2 >= h2) return 0.0;
    float t = h2 - r2;
    float h4 = h2 * h2;
    return 4.0 / (3.14159265 * h4 * h4) * t * t * t;
}

// Spiky kernel gradient (2D): ∇W = -(30 / π h^5) * (h - r)² * r̂
float2 grad_W_spiky(float2 r_vec, float r, float h)
{
    if (r >= h || r < 1e-8) return float2(0, 0);
    float t = h - r;
    float h2 = h * h;
    float h5 = h2 * h2 * h;
    float coeff = -30.0 / (3.14159265 * h5) * t * t;
    return coeff * r_vec / r;
}

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    int total = cb.num_bristles * cb.verts_per_bristle;
    if (idx >= total) return;

    // Root vertex (vi == 0) is pinned to brush, skip
    int vi = idx % cb.verts_per_bristle;
    if (vi == 0) {
        lambda_buf[idx] = 0.0;
        return;
    }

    float2 pos_i = bristle_data[idx].pos;

    // Smoothing radius: ~2× bristle spacing on the circle
    // spacing ≈ 2π·brush_radius / num_bristles
    float h = max(2.0 * 3.14159265 * cb.brush_radius / float(cb.num_bristles), cb.cell_size);
    float h2 = h * h;

    // Rest density: expected number of vertices within kernel support (π h²)
    // scaled so that normal bristle spacing gives ρ ≈ ρ_0
    float rho_0 = 4.0;
    float eps = 600.0; // relaxation parameter (PBF ε)

    if (mode_cb.mode == 0) {
        // === Pass 0: Compute density ρ_i and Lagrange multiplier λ_i ===
        float rho_i = 0.0;
        float grad_sq_sum = 0.0;

        for (int j = 0; j < total; j++) {
            if (j == idx) continue;

            float2 diff = pos_i - bristle_data[j].pos;
            float r2 = dot(diff, diff);
            float r = sqrt(r2);

            // Accumulate density (Poly6 kernel)
            rho_i += W_poly6(r2, h2);

            // Accumulate |∇_k C_i|² = (1/ρ_0)² · |∇W|²
            float2 grad = grad_W_spiky(diff, r, h);
            grad_sq_sum += dot(grad, grad);
        }

        // Constraint: C_i = ρ_i / ρ_0 - 1 ≤ 0
        float C_i = rho_i / rho_0 - 1.0;

        if (C_i > 0.0) {
            // λ_i = -C_i / (Σ_k |∇_k C_i|² + ε)
            lambda_buf[idx] = -C_i / (grad_sq_sum / (rho_0 * rho_0) + eps);
        } else {
            lambda_buf[idx] = 0.0;
        }
    }
    else {
        // === Pass 1: Apply position correction ===
        float lambda_i = lambda_buf[idx];
        float2 dp = float2(0, 0);

        for (int j = 0; j < total; j++) {
            if (j == idx) continue;

            float2 diff = pos_i - bristle_data[j].pos;
            float r = length(diff);
            float2 grad = grad_W_spiky(diff, r, h);

            // Δp_i += (1/ρ_0) * (λ_i + λ_j) * ∇W_ij
            dp += (lambda_i + lambda_buf[j]) * grad;
        }

        dp /= rho_0;

        // Apply with relaxation factor s (Macklin & Müller recommend 0.5)
        float s = 0.5;
        bristle_data[idx].pos = pos_i + s * dp;
    }
}
