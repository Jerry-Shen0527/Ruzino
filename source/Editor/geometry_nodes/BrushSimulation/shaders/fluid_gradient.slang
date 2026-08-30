// Subtract pressure gradient from velocity — 3D version
// Wetbrush Algorithm 1: "u ← u - ∇P"
// 3D: subtract ∂P/∂x, ∂P/∂y, ∂P/∂z from vel_x, vel_y, vel_z
//
// Pressure and the velocity family are WINDOW-SIZED buffers (paper §4.2),
// indexed window-locally via wnb(). Outside-window neighbors mirror the
// center value (Neumann BC — no pressure flux across the window edge; the
// edge is far from the brush). The BC fields (wetness/density) stay GLOBAL.

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> pressure : register(t0);
// Boundary-condition fields (Wetbrush §4.2): solid cells (dried paint)
// keep zero velocity and mirror pressure at their interface (Neumann BC).
// Empty cells are NOT solid even though their wetness is 0; the density
// gate enforces that (see fluid_jacobi.slang for the matching definition).
StructuredBuffer<float> wetness    : register(t1);
StructuredBuffer<float> density    : register(t2);
// Bristle rasterized density (paper §4.2 boundary condition). Window-sized.
StructuredBuffer<float> bristle_density : register(t3);
// Bristle rasterized VELOCITY (§4.1: "we rasterize them into a density field
// and a velocity field. They will be used as boundary conditions"; §4.2:
// "the discretized bristle density and velocity fields are treated as
// boundary conditions in pressure projection"). Window-sized; accumulates
// Σ v·w, so bristle_vel/bristle_density is the mass-weighted average bristle
// sample velocity — the brush's local wall velocity.
StructuredBuffer<float> bristle_vel_x : register(t4);
StructuredBuffer<float> bristle_vel_y : register(t5);
StructuredBuffer<float> bristle_vel_z : register(t6);
// Rasterized swarm density (§4.1/§4.3 joint liquid field). Window-sized.
StructuredBuffer<float> ptcl_density : register(t7);

RWStructuredBuffer<float> vel_x : register(u0);
RWStructuredBuffer<float> vel_y : register(u1);
RWStructuredBuffer<float> vel_z : register(u2);

bool is_solid(uint idx)
{
    return density[idx] > 1e-6f && wetness[idx] < 0.01f;
}

// Brush interior boundary (paper §4.2). Global coords -> window-local lookup.
bool is_brush_g(uint gx, uint gy, uint gz)
{
    int lx = int(gx) - cb.window_origin_x;
    int ly = int(gy) - cb.window_origin_y;
    int lz = int(gz) - cb.window_origin_z;
    int WSX = cb.window_size_x;
    if (lx < 0 || lx >= WSX || ly < 0 || ly >= WSX || lz < 0 || lz >= cb.window_size_z)
        return false;
    uint gi = uint(lz) * uint(WSX * WSX) + uint(ly) * uint(WSX) + uint(lx);
    return bristle_density[gi] > cb.brush_boundary_gate;
}

// MOVING-WALL velocity of a brush-interior cell: the rasterized average
// bristle sample velocity (§4.2's boundary condition pair). The brush is not
// a static wall — its bristles drag the liquid with them.
float3 brush_wall_vel(uint gx, uint gy, uint gz)
{
    int w = win_field_idx(
        int(gx), int(gy), int(gz), cb.window_origin_x, cb.window_origin_y,
        cb.window_origin_z, cb.window_size_x, cb.window_size_z);
    if (w < 0) return float3(0.0, 0.0, 0.0);
    float bd = bristle_density[w];
    if (bd <= cb.brush_boundary_gate) return float3(0.0, 0.0, 0.0);
    float inv = 1.0 / max(bd, 1e-6);
    return float3(bristle_vel_x[w], bristle_vel_y[w], bristle_vel_z[w]) * inv;
}

bool is_blocked(uint idx, uint gx, uint gy, uint gz)
{
    return is_solid(idx) || is_brush_g(gx, gy, gz);
}

// LIQUID OCCUPANCY (§4.1/§4.3 joint field): grid paint + swarm + bristles.
// No liquid → AIR: velocity there is fictional and is cleared (see
// fluid_divergence.slang's is_air note).
bool is_air(uint idx, uint gx, uint gy, uint gz)
{
    if (is_brush_g(gx, gy, gz)) return false;
    if (density[idx] > 1e-6f) return false;
    int w = win_field_idx(
        int(gx), int(gy), int(gz), cb.window_origin_x, cb.window_origin_y,
        cb.window_origin_z, cb.window_size_x, cb.window_size_z);
    if (w >= 0 && ptcl_density[w] > 1e-6f) return false;
    return true;
}

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    int RES  = cb.res;
    int RESZ = cb.res_z;
    uint RNN = (uint)(RES * RES);
    int WSX = cb.window_size_x;
    int WSZ = cb.window_size_z;

    WindowCell c;
    if (!window_map(cb, dtid.x, c)) return;
    uint gx = c.x, gy = c.y, gz = c.z;
    uint idx = c.idx;

    int w0 = wnb(c, 0, 0, 0, WSX, WSZ);
    float p_center = pressure[w0];
    int wxm = wnb(c, -1, 0, 0, WSX, WSZ);
    int wxp = wnb(c, 1, 0, 0, WSX, WSZ);
    int wym = wnb(c, 0, -1, 0, WSX, WSZ);
    int wyp = wnb(c, 0, 1, 0, WSX, WSZ);
    int wzm = wnb(c, 0, 0, -1, WSX, WSZ);
    int wzp = wnb(c, 0, 0, 1, WSX, WSZ);

    // Dried-solid cells keep zero velocity (§4.2). Brush-interior cells get
    // the MOVING-WALL velocity (the rasterized bristle velocity) — the
    // brush's bristles push the liquid as they sweep (§4.2's velocity BC).
    // Zeroing these instead made the brush a static bollard: whatever the
    // swarm merge injected under the brush was erased each projection.
    if (is_solid(idx)) {
        vel_x[w0] = 0.0f;
        vel_y[w0] = 0.0f;
        vel_z[w0] = 0.0f;
        return;
    }
    if (is_brush_g(gx, gy, gz)) {
        float3 wv = brush_wall_vel(gx, gy, gz);
        vel_x[w0] = wv.x;
        vel_y[w0] = wv.y;
        vel_z[w0] = wv.z;
        return;
    }
    // AIR (free surface): no liquid → no liquid velocity. Clear it so the
    // fictional velocity the projection/diffuse stages leave in empty space
    // can never be sampled back by advection or the swarm.
    if (is_air(idx, gx, gy, gz)) {
        vel_x[w0] = 0.0f;
        vel_y[w0] = 0.0f;
        vel_z[w0] = 0.0f;
        return;
    }

    // Pressure at the faces. Neumann BC at solid/brush interfaces: a blocked
    // neighbor mirrors the center (zero gradient, no suction toward the
    // wall). Dirichlet p = 0 at AIR interfaces (free surface).
    float px_lo = (gx > 0)             ? (is_blocked(idx - 1, gx - 1, gy, gz) ? p_center : (is_air(idx - 1, gx - 1, gy, gz) ? 0.0 : ((wxm >= 0) ? pressure[wxm] : p_center))) : p_center;
    float px_hi = (gx < (uint)RES - 1) ? (is_blocked(idx + 1, gx + 1, gy, gz) ? p_center : (is_air(idx + 1, gx + 1, gy, gz) ? 0.0 : ((wxp >= 0) ? pressure[wxp] : p_center))) : p_center;
    vel_x[w0] -= 0.5f * (px_hi - px_lo);

    // ∂P/∂y
    float py_lo = (gy > 0)             ? (is_blocked(idx - (uint)RES, gx, gy - 1, gz) ? p_center : (is_air(idx - (uint)RES, gx, gy - 1, gz) ? 0.0 : ((wym >= 0) ? pressure[wym] : p_center))) : p_center;
    float py_hi = (gy < (uint)RES - 1) ? (is_blocked(idx + (uint)RES, gx, gy + 1, gz) ? p_center : (is_air(idx + (uint)RES, gx, gy + 1, gz) ? 0.0 : ((wyp >= 0) ? pressure[wyp] : p_center))) : p_center;
    vel_y[w0] -= 0.5f * (py_hi - py_lo);

    // ∂P/∂z
    if (RESZ > 1) {
        float pz_lo = (gz > 0)             ? (is_blocked(idx - RNN, gx, gy, gz - 1) ? p_center : (is_air(idx - RNN, gx, gy, gz - 1) ? 0.0 : ((wzm >= 0) ? pressure[wzm] : p_center))) : p_center;
        float pz_hi = (gz < (uint)RESZ - 1) ? (is_blocked(idx + RNN, gx, gy, gz + 1) ? p_center : (is_air(idx + RNN, gx, gy, gz + 1) ? 0.0 : ((wzp >= 0) ? pressure[wzp] : p_center))) : p_center;
        vel_z[w0] -= 0.5f * (pz_hi - pz_lo);
    }

    // CANVAS FLOOR no-flux (the paper's solid boundary under the paint
    // volume — paint cannot sink into the paper). REQUIRED, not cosmetic:
    // fluid_divergence skips the bottom face at gz==0 (the floor IS a wall
    // for the divergence), so the projection can never see or correct the
    // floor-normal velocity — gravity (damp_dry, −0.2 z on wet painted
    // cells) keeps vz<0 there frame after frame. The scalar advection then
    // backtraces with dtz = dt/cell_z ≈ 68 cells per unit velocity (cell_z
    // is 4x smaller than cell_xy), i.e. a −0.2 vz displaces the backtrace
    // 14 cells UP: the thin deposited sheet at gz=0 reads the empty layers
    // above and is erased in ONE step (measured by [wb-xfer]: −66% of the
    // whole canvas mass at the pen-up megadeposit). Clamping the floor-
    // normal component to outflow-free here — AFTER the gradient subtract,
    // i.e. immediately before the scalar advection consumes the field —
    // makes bottom-layer backtraces reach DOWN into the sheet (clamped to
    // itself) instead of up into emptiness.
    if (gz == 0)
        vel_z[w0] = max(vel_z[w0], 0.0f);
}
