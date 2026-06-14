// Bristle density constraint — Wetbrush Section 4.1
// Position-Based Fluids (Macklin & Müller 2013) applied to bristle vertices.
// Prevents bristles from clustering by enforcing a maximal density constraint.
//
// Two-pass dispatch:
//   mode=0: compute density ρ and Lagrange multiplier λ per vertex
//   mode=1: apply position correction using λ values
//
// Bristle vertices are 3D, packed as 2× float4 (pos.xyz+pad, vel.xyz+pad)
// to match bristle_simulate.slang and avoid the RWStructuredBuffer<float3>
// stride hazard.

#include "common.slangh"

struct ConstraintModeCB { int mode; float3 _pad; };

ConstantBuffer<BristleConstants> cb : register(b0);
RWStructuredBuffer<float4> bristle_data : register(u0);  // Nb * M * 2 (pos,vel)
RWStructuredBuffer<float> lambda_buf : register(u1);     // Nb * M (temp)
ConstantBuffer<ConstraintModeCB> mode_cb : register(b1);

#define BRV_STRIDE 2

float3 load_pos(int idx) { return bristle_data[idx * BRV_STRIDE].xyz; }
void store_pos(int idx, float3 p) { bristle_data[idx * BRV_STRIDE] = float4(p, 0.0); }

// Poly6 kernel (3D): W(r,h) = (315 / (64π h⁹)) * (h² - r²)³
float W_poly6(float r2, float h2)
{
    if (r2 >= h2) return 0.0;
    float t = h2 - r2;
    float h3 = h2 * sqrt(h2);
    return 315.0 / (64.0 * 3.14159265 * h3 * h3 * h3) * t * t * t;
}

// Spiky kernel gradient (3D): ∇W = -(45 / (π h⁶)) * (h - r)² * r̂
float3 grad_W_spiky(float3 r_vec, float r, float h)
{
    if (r >= h || r < 1e-8) return float3(0,0,0);
    float hr = h - r;
    float h3 = h * h * h;
    return -(45.0 / (3.14159265 * h3 * h3 * h)) * hr * hr * r_vec / r;
}

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    int Nb = cb.num_bristles;
    int M = cb.verts_per_bristle;
    int total = Nb * M;
    if (idx >= total) return;

    float3 pos_i = load_pos(idx);

    // Smoothing radius: ~2× bristle spacing on the circle
    float h = max(2.0 * 3.14159265 * cb.brush_radius / float(Nb), cb.cell_size);
    float h2 = h * h;

    // Rest density tuned for 3D Poly6 (typical neighbor count in a h-ball).
    float rho_0 = 8.0;
    float eps = 600.0; // PBF relaxation ε

    if (mode_cb.mode == 0) {
        // === Pass 0: density ρ_i and λ_i ===
        float rho_i = 0.0;
        float grad_sq_sum = 0.0;

        for (int j = 0; j < total; j++) {
            if (j == idx) continue;

            float3 diff = pos_i - load_pos(j);
            float r2 = dot(diff, diff);
            float r = sqrt(r2);

            rho_i += W_poly6(r2, h2);

            float3 grad = grad_W_spiky(diff, r, h);
            grad_sq_sum += dot(grad, grad);
        }

        float C_i = rho_i / rho_0 - 1.0;
        if (C_i > 0.0) {
            lambda_buf[idx] = -C_i / (grad_sq_sum / (rho_0 * rho_0) + eps);
        } else {
            lambda_buf[idx] = 0.0;
        }
    }
    else {
        // === Pass 1: position correction ===
        float lambda_i = lambda_buf[idx];
        float3 dp = float3(0,0,0);

        for (int j = 0; j < total; j++) {
            if (j == idx) continue;

            float3 diff = pos_i - load_pos(j);
            float r = length(diff);
            float3 grad = grad_W_spiky(diff, r, h);

            dp += (lambda_i + lambda_buf[j]) * grad;
        }

        dp /= rho_0;

        // Relaxation (Macklin & Müller recommend s=0.5)
        float s = 0.5;
        store_pos(idx, pos_i + s * dp);
    }
}
