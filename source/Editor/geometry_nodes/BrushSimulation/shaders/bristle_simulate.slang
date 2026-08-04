// Bristle simulation — Wetbrush Section 4.1, Eq.2 (3D)
// One thread per bristle. Each bristle is a chain of M vertices.
//
// Pipeline (faithful to the paper):
//   1. Root vertex (0) is pinned to the brush footprint.
//   2. Integrate Eq.2 in the brush frame. The external acceleration a_i
//      contains ONLY true external forces (gravity + grid-liquid drag). It
//      does NOT contain the structural/shear springs — the paper's bristles
//      follow the brush via β_B → 0 (small inertia) + PBD constraints, NOT
//      via spring forces fed into the Newtonian integration. Feeding spring
//      forces into a_i made them amplify frame-boundary acceleration spikes
//      (|a_B| up to 48) and fling bristles off the brush (the "横纹 /
//      orbiting" artifact — bristle tips measured 20-150× brush radius away).
//   3. Enforce PBD constraints (inextensible + bending + rest-shape restore +
//      canvas collision) on positions and velocities.
//
// β_B ∈ [0, 0.1]: small → bristle follows brush tightly (position-based
// adhesion). At β_B = 0 the inertial terms vanish and the vertex is carried
// rigidly with the brush, only reshaped by PBD constraints.
//
// Storage: BristleV3 vertices packed as float4 (pos.xyz + pad, vel.xyz + pad)
// in a RWStructuredBuffer<float4> to avoid the float3 stride hazard.

#include "common.slangh"

// Each bristle vertex occupies 2 consecutive float4 slots: [pos, vel].
#define BRV_STRIDE 2

ConstantBuffer<BristleConstants> cb : register(b0);
RWStructuredBuffer<float4> bristle_data : register(u0);  // Nb * M * 2
// Grid-based liquid velocity field (paper §4.1 Eq.2: a_i includes "drag force
// due to the grid-based liquid flow"). Read-only SRVs so bristles feel the
// surrounding liquid motion. Velocity is stored normalized to the grid: a unit
// velocity moves one XY cell per (dt·N), one Z cell per (dt·D).
StructuredBuffer<float> grid_vel_x : register(t0);
StructuredBuffer<float> grid_vel_y : register(t1);
StructuredBuffer<float> grid_vel_z : register(t2);

float3 load_pos(int idx) { return bristle_data[idx * BRV_STRIDE].xyz; }
float3 load_vel(int idx) { return bristle_data[idx * BRV_STRIDE + 1].xyz; }
void store_pos(int idx, float3 p) { bristle_data[idx * BRV_STRIDE] = float4(p, 0.0); }
void store_vel(int idx, float3 v) { bristle_data[idx * BRV_STRIDE + 1] = float4(v, 0.0); }

// Trilinear sample of a single scalar grid channel at fractional cell coords.
// Cell indices + weights are precomputed by the caller; this just does the 8-
// corner fetch + 3-axis lerp.
float sample_channel(StructuredBuffer<float> buf,
                     int x0,int x1,int y0,int y1,int z0,int z1,
                     float sx,float sy,float sz,int N,int D)
{
    float v000 = buf[grid_idx_3d((uint)x0,(uint)y0,(uint)z0,N,D)];
    float v100 = buf[grid_idx_3d((uint)x1,(uint)y0,(uint)z0,N,D)];
    float v010 = buf[grid_idx_3d((uint)x0,(uint)y1,(uint)z0,N,D)];
    float v110 = buf[grid_idx_3d((uint)x1,(uint)y1,(uint)z0,N,D)];
    float v001 = buf[grid_idx_3d((uint)x0,(uint)y0,(uint)z1,N,D)];
    float v101 = buf[grid_idx_3d((uint)x1,(uint)y0,(uint)z1,N,D)];
    float v011 = buf[grid_idx_3d((uint)x0,(uint)y1,(uint)z1,N,D)];
    float v111 = buf[grid_idx_3d((uint)x1,(uint)y1,(uint)z1,N,D)];
    float v00 = lerp(v000,v100,sx), v10 = lerp(v010,v110,sx);
    float v01 = lerp(v001,v101,sx), v11 = lerp(v011,v111,sx);
    float v0 = lerp(v00,v10,sy),   v1 = lerp(v01,v11,sy);
    return lerp(v0, v1, sz);
}

// Trilinearly sample the grid velocity field at a world-space point (paper
// §4.1: the drag uses the liquid velocity AT the bristle vertex). Returns the
// grid-normalized velocity (same convention as fluid_advect.slang). Out of
// bounds or in empty cells → zero (no drag).
float3 sample_grid_vel(float3 world_pos)
{
    int N = cb.grid_res;
    int D = cb.grid_res_z;
    if (N <= 0 || D <= 0) return float3(0.0, 0.0, 0.0);

    float half_p = cb.paper_size * 0.5;
    float half_h = cb.height_extent * 0.5;
    float cell_z = cb.height_extent / float(D);

    float gx = (world_pos.x - cb.grid_center_x + half_p) / cb.cell_size;
    float gy = (world_pos.y - cb.grid_center_y + half_p) / cb.cell_size;
    float gz = (world_pos.z - cb.grid_center_z + half_h) / cell_z;

    // Outside the grid → no liquid → no drag.
    if (gx < 0.5 || gy < 0.5 || gz < 0.5) return float3(0.0, 0.0, 0.0);
    float max_xy = float(N) - 1.5;
    float max_z = float(D) - 1.5;
    if (gx > max_xy || gy > max_xy || gz > max_z) return float3(0.0, 0.0, 0.0);

    int x0 = int(gx), y0 = int(gy), z0 = int(gz);
    int x1 = min(x0 + 1, N - 1);
    int y1 = min(y0 + 1, N - 1);
    int z1 = min(z0 + 1, D - 1);
    float sx = gx - float(x0), sy = gy - float(y0), sz = gz - float(z0);

    return float3(
        sample_channel(grid_vel_x, x0,x1,y0,y1,z0,z1, sx,sy,sz, N,D),
        sample_channel(grid_vel_y, x0,x1,y0,y1,z0,z1, sx,sy,sz, N,D),
        sample_channel(grid_vel_z, x0,x1,y0,y1,z0,z1, sx,sy,sz, N,D));
}

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int bi = int(tid.x);
    if (bi >= cb.num_bristles) return;

    int M = cb.verts_per_bristle;
    int base = bi * M;

    // Root position: distribute bristles across the 2D brush footprint (a
    // filled disk) using a golden-angle spiral (Vogel/sunflower pattern) —
    // gives a near-uniform area filling rather than a hollow ring.
    float golden_angle = 2.39996323;  // radians (~137.5°)
    float angle = golden_angle * float(bi);
    float r_frac = sqrt((float(bi) + 0.5) / float(cb.num_bristles));
    float3 root_offset = cb.brush_radius * r_frac * float3(cos(angle), sin(angle), 0.0);

    float3 brush_pos = float3(cb.brush_pos_x, cb.brush_pos_y, cb.brush_pos_z);
    float3 brush_vel = float3(cb.brush_vel_x, cb.brush_vel_y, cb.brush_vel_z);

    // Vertex 0: pinned to root
    float3 root_pos = brush_pos + root_offset;
    store_pos(base, root_pos);
    store_vel(base, brush_vel);

    // Bristle rest length per segment (chain points from root toward canvas).
    float beta_B = cb.beta_B;
    float dt = cb.dt;
    float rest_len = cb.brush_radius * 1.5 / float(M);
    // Vertical extent of the bristle (root → tip toward the canvas, -Z).
    float3 down_dir = float3(0.0, 0.0, -1.0);

    // Non-inertial frame quantities (full 3D). omega is rad/s; omega_dot is
    // rad/s², both finite-differenced on the host from the previous frame.
    float3 omega = float3(cb.brush_angular_vel_x, cb.brush_angular_vel_y, cb.brush_angular_vel_z);
    float3 omega_dot = float3(cb.brush_angular_accel_x, cb.brush_angular_accel_y, cb.brush_angular_accel_z);
    // Frame-origin linear acceleration a_B (Eq.2 rectilinear term).
    float3 a_B = float3(cb.brush_accel_x, cb.brush_accel_y, cb.brush_accel_z);

    // Light damping on brush-frame relative velocity so any transient motion
    // (from β_B > 0) decays smoothly. Acts on v_B (relative to brush), so a
    // bristle rigidly carried by the brush (v_B ≈ 0) is not damped at all.
    float damping_B = cb.damping;

    // Simulate vertices 1..M-1
    for (int vi = 1; vi < M; vi++) {
        int idx = base + vi;
        float3 pos = load_pos(idx);
        float3 vel = load_vel(idx);

        // First-frame initialization: lay the bristle out as a straight
        // vertical chain from the root toward the canvas (down -Z). Detected
        // by an all-zero persisted position (buffer is zero-initialized).
        if (pos.x == 0.0 && pos.y == 0.0 && pos.z == 0.0) {
            pos = root_pos + down_dir * rest_len * float(vi);
        }

        // === Non-inertial brush frame integration (Wetbrush Eq.2) ===
        // Convert into the brush frame, integrate there, convert back. With
        // β_B → 0 the vertex is carried rigidly by the brush (position-based
        // adhesion); β_B = 1 recovers canvas-frame Newton. R_B = I here
        // (brush frame differs from canvas frame by translation only).
        float3 x_B = pos - brush_pos;     // brush-frame position
        float3 v_B = vel - brush_vel;     // brush-frame velocity

        // External acceleration a_i: per the paper, only TRUE external forces
        // (gravity + grid-liquid drag). Structural/shear springs are NOT part
        // of a_i — bristle shape is enforced by PBD constraints below. This
        // prevents spring forces from being amplified by frame-boundary
        // acceleration spikes and flinging bristles off the brush. We keep a
        // small brush-frame velocity damping so β_B-induced transients settle.
        float3 external_a = -damping_B * v_B;

        // Gravity (paper §4.1: a_i "including the gravity force and the drag
        // force due to the grid-based liquid flow"). The particle path uses
        // -0.2 in Z (particle_update.slang); bristles need the same so they
        // sag toward the canvas and their tips lie flat instead of being
        // shaped purely by the splay geometry (which fanned them into a cone).
        // The paper's bristles are under gravity + drag only; without gravity
        // the tip shape came entirely from the canvas-collision splay and
        // produced a pointed "尖尖" brush end.
        external_a += float3(0.0, 0.0, -0.2);

        // Grid-liquid drag (paper §4.1 Eq.2: a_i includes "drag force due to
        // the grid-based liquid flow"; §4.3: "velocity field provides the drag
        // force used for brush simulation"). The surrounding liquid velocity
        // drags the bristle toward the flow. drag_coeff reuses cb.damping; the
        // grid velocity is grid-normalized, so this is a gentle pull (a unit
        // grid velocity crossing the brush in one frame applies one damping
        // unit of accel). No drag where there's no liquid (empty cells = 0).
        float3 gvel = sample_grid_vel(pos);
        external_a += damping_B * (gvel - vel);

        // === All four non-inertial frame accelerations (Eq.2, 3D) ===
        // 1. Rectilinear: linear acceleration of the frame origin (a_B).
        float3 euler_rect = a_B;

        // 2. Centrifugal: omega × (omega × x_B)
        float3 centrifugal = cross(omega, cross(omega, x_B));

        // 3. Euler angular acceleration: omega_dot × x_B
        float3 euler_angular = cross(omega_dot, x_B);

        // 4. Coriolis: -2 * omega × v_B
        float3 coriolis = -2.0 * cross(omega, v_B);

        // Eq.2: dv_i^B/dt = R_B·a_i − β_B·(rect + centrifugal + euler + coriolis)
        float3 inertial_sum = euler_rect + centrifugal + euler_angular + coriolis;
        float3 accel_B = external_a - beta_B * inertial_sum;

        // Integrate IN THE BRUSH FRAME (semi-implicit Euler).
        v_B += accel_B * dt;
        x_B += v_B * dt;

        // Convert back to canvas frame.
        vel = v_B + brush_vel;
        pos = x_B + brush_pos;

        store_pos(idx, pos);
        store_vel(idx, vel);
    }

    // === Constraint enforcement (Position-Based Dynamics) ===
    // Paper §4.1: "After explicit time integration of the forces, we enforce
    // inextensible and bending constraints on vertex positions and velocities
    // by the position-based method [Müller et al. 2007]."
    //
    // These constraints are what hold the bristle chain together and make it
    // follow the brush. With β_B small the integration barely moves vertices,
    // and these PBD passes (a) keep segment lengths at rest_len, (b) keep the
    // chain straight (low bending), and (c) restore each vertex toward its
    // rest pose relative to the brush root — so the bristle "remembers" its
    // vertical rest shape and follows the brush.
    float3 delta_pos[64];
    [unroll] for (int zi = 0; zi < 64; zi++) delta_pos[zi] = float3(0,0,0);

    for (int iter = 0; iter < 3; iter++) {
        // Inextensible constraint — enforce segment length = rest_len (both
        // directions: pull apart if compressed, pull together if stretched).
        // This is a bilateral distance constraint, not just a max-length cap.
        for (int vi = 1; vi < M; vi++) {
            int idx = base + vi;
            float3 diff = load_pos(idx) - load_pos(idx - 1);
            float d = length(diff) + 1e-8;
            float3 n = diff / d;
            // Position-based distance projection: move both endpoints to make
            // the segment exactly rest_len. Root (vi-1==0) is pinned, so only
            // the non-root vertex moves in that case.
            float3 correction = (d - rest_len) * n * 0.5;
            if (vi == 1) {
                // Root is pinned: apply the full correction to vertex vi.
                store_pos(idx, load_pos(idx) - 2.0 * correction);
                delta_pos[vi] -= 2.0 * correction;
            } else {
                store_pos(idx, load_pos(idx) - correction);
                delta_pos[vi] -= correction;
                store_pos(idx - 1, load_pos(idx - 1) + correction);
                delta_pos[vi - 1] += correction;
            }
        }

        // Bending constraint — project each interior vertex toward the
        // midpoint of its neighbors (straight-chain rest shape).
        float bend_stiffness = 0.3; // [0, 1]
        for (int vi = 2; vi < M - 1; vi++) {
            int idx = base + vi;
            float3 p0 = load_pos(idx - 1);
            float3 p1 = load_pos(idx);
            float3 p2 = load_pos(idx + 1);

            float3 mid = (p0 + p2) * 0.5;
            float3 correction = (mid - p1) * bend_stiffness;
            store_pos(idx, p1 + correction);
            delta_pos[vi] += correction;
        }

        // Rest-shape restore — gently pull each vertex toward its vertical
        // rest position relative to the brush root. This makes the bristle
        // follow the brush (the chain "wants" to hang straight down from the
        // root) and counteracts any drift from β_B > 0. Without this, the
        // bilateral length + bending constraints alone cannot anchor the
        // chain to the brush — they only preserve chain shape, not position.
        float restore_stiffness = 0.5; // [0, 1]
        for (int vi = 1; vi < M; vi++) {
            int idx = base + vi;
            float3 p = load_pos(idx);
            float3 rest_p = root_pos + down_dir * rest_len * float(vi);
            float3 correction = (rest_p - p) * restore_stiffness;
            store_pos(idx, p + correction);
            delta_pos[vi] += correction;
        }

        // Canvas collision + pressure splaying (§4.1, Fig 3a).
        // Vertices cannot penetrate below the canvas plane. Vertices in
        // contact are pushed radially outward (away from the brush XY
        // center) proportional to pressure — this produces the fan/splay
        // footprint when the brush is pressed down.
        float pressure = max(cb.brush_pressure, 0.0);
        for (int vi = 1; vi < M; vi++) {
            int idx = base + vi;
            float3 p = load_pos(idx);

            // Non-penetration: clamp to canvas plane.
            if (p.z < cb.canvas_z) {
                float penetration = cb.canvas_z - p.z;
                float3 old_p = p;
                p.z = cb.canvas_z;

                // Splay: push the contact point radially outward in XY by
                // an amount proportional to penetration depth and pressure.
                // The deeper the press / higher the pressure, the wider the
                // footprint spreads. Tip vertices (large vi) splay more.
                float3 radial = float3(p.x - brush_pos.x, p.y - brush_pos.y, 0.0);
                float rlen = length(radial);
                if (rlen > 1e-6) {
                    float tip_weight = float(vi) / float(M);  // tips splay more
                    float splay = penetration * pressure * tip_weight * cb.brush_radius;
                    float3 push = normalize(radial) * splay;
                    p.x += push.x;
                    p.y += push.y;
                }
                store_pos(idx, p);
                delta_pos[vi] += p - old_p;
            }
        }
    }

    // PBD velocity update: fold accumulated position correction into velocity.
    // Stiffness damping η controls how strongly the correction rescales the
    // velocity (Müller et al. 2007 Eq.21-style, η∈[0,1]).
    float inv_dt = (cb.dt > 1e-6f) ? 1.0f / cb.dt : 0.0f;
    float vel_damp = 1.0f;  // full correction; reduce to soften
    for (int vi = 1; vi < M; vi++) {
        int idx = base + vi;
        float3 v = load_vel(idx);
        v += delta_pos[vi] * (inv_dt * vel_damp);
        store_vel(idx, v);
    }
}
