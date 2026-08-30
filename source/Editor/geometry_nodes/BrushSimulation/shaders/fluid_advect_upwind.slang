// Conservative flux-form (upwind finite-volume) advection for the CANVAS
// SCALARS — density / color / wetness / oil (paper §4.2: "we advect all of
// the fields using the semi-Lagrangian method [Stam 1999]").
//
// WHY NOT SEMI-LAGRANGIAN FOR THE SCALARS: the canvas scalars are EXTENSIVE
// (accumulated paint mass per cell), and the [wb-xfer]/[wb-mass] ledger
// showed that value-based semi-Lagrangian interpolation of an extensive
// field does not conserve it — interpolation READS without draining the
// source cell, so any non-volume-preserving backtrace mapping silently
// creates or destroys mass. Measured at the pen-up megadeposit
// (1-2-cell-thin sheet at the canvas floor, cell_z 4x smaller than cell_xy,
// dtz = dt/cell_z ≈ 68): −66% of the whole canvas mass in ONE step without
// the floor velocity clamp, +15% creation with it. The velocity field keeps
// the paper's semi-Lagrangian (velocity is not a conserved quantity and is
// re-projected every substep) — see fluid_advect.slang.
//
// Scheme: first-order upwind fluxes on the colocated grid. Every interior
// face's flux is computed identically (and antisymmetrically) from both
// sides, so the window-sum change is EXACTLY the boundary flux — zero with
// the all-wall boundary treatment below. Face velocities are CFL-clamped to
// ±1 cell per step, which keeps the scheme unconditionally stable (the
// pen-up horizontal jet reached ~67 cells/step); clamping the shared face
// velocity preserves pairwise antisymmetry, so stability costs advection
// SPEED, never mass.
//
// Boundaries — all walls (no flux):
//   * canvas floor gz==0 (paint cannot sink into the paper),
//   * active-window side edges (no liquid motion outside the window,
//     paper §4.2 — velocity there is zero anyway),
//   * global-grid XY borders and the top z face.
// Dispatched over the active window; field_out is the window-sized tmp the
// host copies back over the window region (field_copy_window mode 2).

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> field_in : register(t0);  // GLOBAL canvas field
StructuredBuffer<float> vel_x    : register(t1);
StructuredBuffer<float> vel_y    : register(t2);
StructuredBuffer<float> vel_z    : register(t3);

RWStructuredBuffer<float> field_out : register(u0);  // WINDOW-sized tmp

// Face flux between the current cell (value d_self, normal velocity
// component v_c) and its neighbor (v_n, d_n). dir = ±1 outward normal.
// Positive result = mass leaving the current cell. Antisymmetric under the
// self<->neighbor role swap, hence exactly conservative.
float face_flux(
    float v_c,
    float v_n,
    float d_self,
    float d_n,
    float dir,
    float h,
    float dt)
{
        float v_f = 0.5f * (v_c + v_n) * dir;
        // CFL clamp on the SHARED face velocity. Multi-D explicit upwind is
        // monotone (positivity-preserving) only when the SUM of the three
        // axis Courant numbers stays ≤ 1, so each face is clamped to 1/3 —
        // a per-face h/dt clamp alone (sum ≤ 3) blew up: oscillatory
        // negatives from f60, density inf by f67 ([wb-mass] TOTAL 1.5e15).
        // Pairwise antisymmetry is preserved, so this costs transport speed
        // (grid scalar transport ≤ 1/3 cell/step per axis), never mass.
        float v_cap = h / (3.0f * dt);
        v_f = max(-v_cap, min(v_f, v_cap));
        // Upwind: positive face velocity carries the SELF cell's value out.
        float d_up = (v_f > 0.0f) ? d_self : d_n;
        return v_f / h * d_up;
}

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    int N = cb.res;
    int D = cb.res_z;
    int WSX = cb.window_size_x;
    int WSZ = cb.window_size_z;

    WindowCell c;
    if (!window_map(cb, dtid.x, c)) return;
    uint gx = c.x, gy = c.y, gz = c.z;
    uint gidx = grid_idx_3d(gx, gy, gz, uint(N), uint(D));
    float d_self = field_in[gidx];

    int w0 = win_field_idx(
        int(gx), int(gy), int(gz), cb.window_origin_x, cb.window_origin_y,
        cb.window_origin_z, WSX, WSZ);
    float vself_x = (w0 >= 0) ? vel_x[w0] : 0.0f;
    float vself_y = (w0 >= 0) ? vel_y[w0] : 0.0f;
    float vself_z = (w0 >= 0) ? vel_z[w0] : 0.0f;

    float cell_z = cb.height_extent / float(D);
    float dt = cb.dt;
    float total = 0.0f;

    // X faces
    {
        int nx = int(gx) + 1;
        if (nx < N) {
            int wn = win_field_idx(nx, int(gy), int(gz), cb.window_origin_x,
                cb.window_origin_y, cb.window_origin_z, WSX, WSZ);
            if (wn >= 0)
                total -= face_flux(vself_x, vel_x[wn], d_self,
                    field_in[grid_idx_3d(uint(nx), gy, gz, uint(N), uint(D))],
                    1.0f, cb.cell_size, dt);
        }
    }
    {
        int nx = int(gx) - 1;
        if (nx >= 0) {
            int wn = win_field_idx(nx, int(gy), int(gz), cb.window_origin_x,
                cb.window_origin_y, cb.window_origin_z, WSX, WSZ);
            if (wn >= 0)
                total -= face_flux(vself_x, vel_x[wn], d_self,
                    field_in[grid_idx_3d(uint(nx), gy, gz, uint(N), uint(D))],
                    -1.0f, cb.cell_size, dt);
        }
    }

    // Y faces
    {
        int ny = int(gy) + 1;
        if (ny < N) {
            int wn = win_field_idx(int(gx), ny, int(gz), cb.window_origin_x,
                cb.window_origin_y, cb.window_origin_z, WSX, WSZ);
            if (wn >= 0)
                total -= face_flux(vself_y, vel_y[wn], d_self,
                    field_in[grid_idx_3d(gx, uint(ny), gz, uint(N), uint(D))],
                    1.0f, cb.cell_size, dt);
        }
    }
    {
        int ny = int(gy) - 1;
        if (ny >= 0) {
            int wn = win_field_idx(int(gx), ny, int(gz), cb.window_origin_x,
                cb.window_origin_y, cb.window_origin_z, WSX, WSZ);
            if (wn >= 0)
                total -= face_flux(vself_y, vel_y[wn], d_self,
                    field_in[grid_idx_3d(gx, uint(ny), gz, uint(N), uint(D))],
                    -1.0f, cb.cell_size, dt);
        }
    }

    // Z faces (cell_z differs from cell_xy — anisotropic grid). The gz==0
    // face is the canvas floor and gz==D-1 the top lid: both walls, and the
    // bounds checks below already return zero flux for them.
    {
        int nz = int(gz) + 1;
        if (nz < D) {
            int wn = win_field_idx(int(gx), int(gy), nz, cb.window_origin_x,
                cb.window_origin_y, cb.window_origin_z, WSX, WSZ);
            if (wn >= 0)
                total -= face_flux(vself_z, vel_z[wn], d_self,
                    field_in[grid_idx_3d(gx, gy, uint(nz), uint(N), uint(D))],
                    1.0f, cell_z, dt);
        }
    }
    {
        int nz = int(gz) - 1;
        if (nz >= 0) {
            int wn = win_field_idx(int(gx), int(gy), nz, cb.window_origin_x,
                cb.window_origin_y, cb.window_origin_z, WSX, WSZ);
            if (wn >= 0)
                total -= face_flux(vself_z, vel_z[wn], d_self,
                    field_in[grid_idx_3d(gx, gy, uint(nz), uint(N), uint(D))],
                    -1.0f, cell_z, dt);
        }
    }

    // field_out is the WINDOW-SIZED tmp: window-local index (c.idx is the
    // GLOBAL index — wrong for this buffer).
    field_out[wnb(c, 0, 0, 0, WSX, WSZ)] = d_self + dt * total;
}
