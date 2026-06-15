// Particle update — Wetbrush Section 4.3, Eq.8-10 (3D, per-sample local frame)
// Two-step update in non-inertial brush frame (Eq.9) + canvas frame, blended
// by adhesion distance (Eq.10). Also applies friction (Eq.8) near solid
// surfaces and grid-liquid drag (§4.1).
//
// Per §4.1 "Bristle samples and local frames": each particle is simulated
// in the local frame of its CLOSEST bristle sample. The sample frame
// (tangent/normal/binormal = R_L, omega_L) drives the four non-inertial
// accelerations in Eq.9.

#include "common.slangh"

ConstantBuffer<ParticleConstants> cb : register(b0);
StructuredBuffer<float4> ptcl_pos : register(t0);
StructuredBuffer<float4> ptcl_vel : register(t1);
StructuredBuffer<float4> ptcl_color : register(t2);
StructuredBuffer<uint> ptcl_alive : register(t3);
// Per-sample bristle frame data for Eq.9 local-frame simulation
StructuredBuffer<float4> sample_pos : register(t4);
StructuredBuffer<SampleFrame> sample_frame : register(t5);
// Grid velocity field — drag coupling (§4.1 "drag force due to the
// grid-based liquid flow" is part of a_i in Eq.9).
StructuredBuffer<float> grid_vel_x : register(t6);
StructuredBuffer<float> grid_vel_y : register(t7);
StructuredBuffer<float> grid_vel_z : register(t8);
RWStructuredBuffer<float4> ptcl_pos_out : register(u0);
RWStructuredBuffer<float4> ptcl_vel_out : register(u1);
RWStructuredBuffer<uint> ptcl_alive_out : register(u2);

// Trilinear sample of a 3D scalar field at world position.
float sample_grid_3d(StructuredBuffer<float> field, float3 pos)
{
    float half_p = cb.paper_size * 0.5;
    float half_h = cb.height_extent * 0.5;
    float cell_z = cb.height_extent / float(cb.grid_res_z);
    float gx = (pos.x - cb.grid_center_x + half_p) / cb.cell_size - 0.5;
    float gy = (pos.y - cb.grid_center_y + half_p) / cb.cell_size - 0.5;
    float gz = (pos.z - cb.grid_center_z + half_h) / cell_z - 0.5;

    int ix = int(floor(gx));
    int iy = int(floor(gy));
    int iz = int(floor(gz));
    float fx = gx - float(ix);
    float fy = gy - float(iy);
    float fz = gz - float(iz);

    int res = cb.grid_res, res_z = cb.grid_res_z;
    float c000 = 0.0, c100 = 0.0, c010 = 0.0, c110 = 0.0;
    float c001 = 0.0, c101 = 0.0, c011 = 0.0, c111 = 0.0;
    for (int dz = 0; dz <= 1; dz++) {
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) {
                int nx = ix + dx, ny = iy + dy, nz = iz + dz;
                if (nx >= 0 && nx < res && ny >= 0 && ny < res && nz >= 0 && nz < res_z) {
                    uint gi = grid_idx_3d(uint(nx), uint(ny), uint(nz), res, res_z);
                    float v = field[gi];
                    if (dz==0&&dy==0&&dx==0) c000=v; else if (dz==0&&dy==0&&dx==1) c100=v;
                    else if (dz==0&&dy==1&&dx==0) c010=v; else if (dz==0&&dy==1&&dx==1) c110=v;
                    else if (dz==1&&dy==0&&dx==0) c001=v; else if (dz==1&&dy==0&&dx==1) c101=v;
                    else if (dz==1&&dy==1&&dx==0) c011=v; else c111=v;
                }
            }
        }
    }
    float c00 = lerp(c000, c100, fx);
    float c10 = lerp(c010, c110, fx);
    float c01 = lerp(c001, c101, fx);
    float c11 = lerp(c011, c111, fx);
    float c0 = lerp(c00, c10, fy);
    float c1 = lerp(c01, c11, fy);
    return lerp(c0, c1, fz);
}

float3 sample_grid_vel(float3 pos)
{
    return float3(sample_grid_3d(grid_vel_x, pos),
                  sample_grid_3d(grid_vel_y, pos),
                  sample_grid_3d(grid_vel_z, pos));
}

// Convert canvas-frame vector v to the local frame of a sample (R_L is
// the matrix whose columns are the sample's tangent/normal/binormal).
float3 to_local(float3 v, SampleFrame f)
{
    return float3(dot(v, f.tangent.xyz),
                  dot(v, f.normal.xyz),
                  dot(v, f.binormal.xyz));
}
// Inverse: local → canvas (R_L^T, since columns are orthonormal).
float3 to_canvas(float3 v, SampleFrame f)
{
    return f.tangent.xyz * v.x + f.normal.xyz * v.y + f.binormal.xyz * v.z;
}

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int idx = int(tid.x);
    if (idx >= cb.max_particles) return;
    if (ptcl_alive[idx] == 0) {
        ptcl_alive_out[idx] = 0;
        return;
    }

    float3 pos = ptcl_pos[idx].xyz;
    float3 vel = ptcl_vel[idx].xyz;
    float3 brush_center = float3(cb.brush_pos_x, cb.brush_pos_y, cb.brush_pos_z);
    float dt = cb.dt;

    // --- Find closest bristle sample (linear scan; Nb*S ~ 2560 is cheap) ---
    int num_samples = cb.num_bristles * cb.samples_per_bristle;
    int best = 0;
    float best_d2 = 1e30;
    for (int s = 0; s < num_samples; s++) {
        float3 d = pos - sample_pos[s].xyz;
        float d2 = dot(d, d);
        if (d2 < best_d2) { best_d2 = d2; best = s; }
    }
    SampleFrame frame = sample_frame[best];
    float3 sample_origin = sample_pos[best].xyz;
    float3 omega_L = float3(frame.tangent.w, frame.normal.w, frame.binormal.w);

    // --- Step A: simulate in the closest sample's local frame (Eq.9) ---
    // Convert position/velocity into the local (non-inertial) frame.
    float3 r_from_sample = pos - sample_origin;
    float3 p_L = to_local(r_from_sample, frame);
    float3 v_L = to_local(vel, frame);

    // External acceleration (canvas-frame forces): gravity + friction (Eq.8)
    // + grid-liquid drag (§4.1: a_i "includes the gravity force and the drag
    // force due to the grid-based liquid flow").
    // Solid-surface distance d_k (Eq.8): the paper's "solid surface" includes
    // both the canvas AND bristles (§4.1). Take the minimum of the canvas
    // plane distance and the distance to the nearest bristle sample.
    float canvas_z = cb.grid_center_z - cb.height_extent * 0.5;
    float d_canvas = abs(pos.z - canvas_z);
    float d_bristle = sqrt(best_d2);
    float d_k = min(d_canvas, d_bristle);
    // Eq.8 friction multiplier — Wetbrush (1-δ·d_k)² with cutoff at d_k = 1/δ.
    // δ (cb.friction_delta) is the inverse friction range; paper uses δ=1/0.2cm
    // so the active band is D0/5 when friction_delta = 5/D0.
    float delta = cb.friction_delta;
    float t = 1.0 - delta * d_k;
    float delta_k = max(t * t, 0.0);

    // Grid-liquid drag: a viscous pull toward the local grid velocity.
    // κ scales the coupling strength; tuned so paint follows the grid flow
    // without stiffly locking to it.
    float3 u_grid = sample_grid_vel(pos);
    float kappa = 4.0;  // drag gain
    float3 drag = (u_grid - vel) * kappa;

    // Gravity (scaled to world units; small magnitude for thin paint layer).
    float3 gravity = float3(0.0, 0.0, -0.2);

    float3 a_ext_canvas = gravity + drag - delta_k * vel;

    // Frame-origin acceleration a_L (Eq.2 rectilinear) and angular
    // acceleration ω̇_L, finite-differenced on the host (rad/s²).
    float3 a_B = float3(cb.brush_accel_x, cb.brush_accel_y, cb.brush_accel_z);
    float3 omega_dot_L = float3(cb.brush_angular_accel_x, cb.brush_angular_accel_y, cb.brush_angular_accel_z);

    // Eq.2/Eq.9 four inertial accelerations in the local frame:
    //   rectilinear (a_B), centrifugal, Euler angular (ω̇), Coriolis.
    float3 a_ext_L = to_local(a_ext_canvas, frame);
    float3 a_B_L = to_local(a_B, frame);
    float3 centrifugal_L = cross(omega_L, cross(omega_L, p_L));
    float3 euler_ang_L = cross(omega_dot_L, p_L);
    float3 coriolis_L = -2.0 * cross(omega_L, v_L);
    float beta_L = 0.1;  // mid-range of [0, 0.2] per Table 1

    float3 a_L = a_ext_L - beta_L * (a_B_L + centrifugal_L + euler_ang_L + coriolis_L);
    float3 v_L_new = v_L + a_L * dt;
    float3 p_L_new = p_L + v_L_new * dt;

    // Convert local result back to canvas frame.
    float3 pos_local = sample_origin + to_canvas(p_L_new, frame);
    float3 vel_local = to_canvas(v_L_new, frame);

    // --- Step B: canvas-frame integration (no adhesion) ---
    float3 a_canvas = a_ext_canvas;
    float3 v_canvas_new = vel + a_canvas * dt;
    float3 pos_canvas = pos + v_canvas_new * dt;

    // Clamp Z to the paint volume.
    float half_h = cb.height_extent * 0.5;
    pos_local.z = clamp(pos_local.z, canvas_z, canvas_z + cb.height_extent);
    pos_canvas.z = clamp(pos_canvas.z, canvas_z, canvas_z + cb.height_extent);

    // --- Blend by adhesion (Eq.10): p_new = p'' + max(1 - d_B/D1, 0) * (p' - p'') ---
    float d_B = sqrt(best_d2);  // distance to closest bristle sample
    float blend = max(1.0 - d_B / max(cb.D1, 1e-6f), 0.0);

    float3 new_pos = lerp(pos_canvas, pos_local, blend);
    float3 new_vel = lerp(v_canvas_new, vel_local, blend);

    // Kill particles that drift very far and stall — reclaims pool slots.
    float speed = length(new_vel);
    if (d_B > cb.D0 * 2.0 && speed < 0.01) {
        ptcl_alive_out[idx] = 0;
        return;
    }

    ptcl_pos_out[idx] = float4(new_pos, 0.0);
    ptcl_vel_out[idx] = float4(new_vel, 0.0);
    ptcl_alive_out[idx] = 1;
}
