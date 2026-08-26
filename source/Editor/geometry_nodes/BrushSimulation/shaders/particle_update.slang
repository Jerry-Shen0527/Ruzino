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

// Trilinear sample of a WINDOW-LOCAL scalar field (the grid velocity family)
// at world position. Outside the active window the value is 0 — there is no
// liquid motion outside the window (paper §4.2).
// Window-local fetch with outside-window → 0 (velocity family).
float fetch_win(StructuredBuffer<float> field, int gx, int gy, int gz)
{
    int w = win_field_idx(
        gx, gy, gz, cb.window_origin_x, cb.window_origin_y,
        cb.window_origin_z, cb.window_size_x, cb.window_size_z);
    return (w >= 0) ? field[w] : 0.0;
}

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


    float c000 = fetch_win(field, ix, iy, iz), c100 = fetch_win(field, ix+1, iy, iz);
    float c010 = fetch_win(field, ix, iy+1, iz), c110 = fetch_win(field, ix+1, iy+1, iz);
    float c001 = fetch_win(field, ix, iy, iz+1), c101 = fetch_win(field, ix+1, iy, iz+1);
    float c011 = fetch_win(field, ix, iy+1, iz+1), c111 = fetch_win(field, ix+1, iy+1, iz+1);

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
    // The sample frame TRANSLATES with the brush. Convert IN with the
    // sample's position at time t (origin − v_frame·dt) and OUT with its
    // current position (the buffer holds t+dt), so the brush's translation
    // carries the particle — this is the paper's two-step adhesion (Fig.8b).
    // Using the current origin on BOTH sides cancels the translation and was
    // why adhered particles never moved with the brush.
    float3 v_frame = float3(cb.brush_vel_x, cb.brush_vel_y, cb.brush_vel_z);
    float3 origin_prev = sample_origin - v_frame * dt;
    // Convert position/velocity into the local (non-inertial) frame; the
    // frame velocity is subtracted so v_L is the velocity RELATIVE to the
    // bristle (added back after the conversion out).
    float3 r_from_sample = pos - origin_prev;
    float3 p_L = to_local(r_from_sample, frame);
    float3 v_L = to_local(vel - v_frame, frame);

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

    // Gravity (§4.3 a_k), physically scaled: 1 unit ≈ 27.5 cm → g ≈
    // 35.7 units/s². The old hardcoded -0.2 was ~180× weak — flung
    // particles hovered and deposited mid-air.
    float3 gravity = float3(cb.gravity_x, cb.gravity_y, cb.gravity_z);

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

    // Convert local result back to canvas frame: origin at t+dt is the
    // buffer's current sample position; the frame velocity is added back.
    float3 pos_local = sample_origin + to_canvas(p_L_new, frame);
    float3 vel_local = to_canvas(v_L_new, frame) + v_frame;

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

    // No far+stalled reaper: a slow particle beyond D0 is exactly the §5.2
    // deposit case (particle_to_grid converts it and its mass lands on the
    // canvas). The old d_B > 2·D0 && speed < 0.01 kill destroyed that mass
    // outright — a silent conservation leak the pool-reclamation rationale
    // no longer needs now that deposition handles those particles.
    ptcl_pos_out[idx] = float4(new_pos, 0.0);
    ptcl_vel_out[idx] = float4(new_vel, 0.0);
    ptcl_alive_out[idx] = 1;
}
