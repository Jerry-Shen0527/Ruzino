// Compute velocity divergence for pressure projection — 3D version
// Wetbrush Section 4.2 + Algorithm 1: "D = ∇·u"
// 3D: div = ∂u/∂x + ∂v/∂y + ∂w/∂z
//
// The velocity field and div_out are WINDOW-SIZED buffers (paper §4.2: the
// solve lives in the active window only) — indexed window-locally via wnb().
// Outside-window neighbors read as 0 velocity: the window edge acts as a
// no-flux wall. It sits far from the brush (window 128 cells vs footprint
// ~41), where velocity has already decayed to ~0. The BC fields
// (wetness/density) stay GLOBAL (canvas state) and are indexed globally.

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> vel_x : register(t0);
StructuredBuffer<float> vel_y : register(t1);
StructuredBuffer<float> vel_z : register(t2);
// Boundary-condition fields (Wetbrush §4.2): wetness+density marks cells
// whose paint has dried. Dried cells act as solid during pressure projection.
StructuredBuffer<float> wetness    : register(t3);
StructuredBuffer<float> density    : register(t4);
// Bristle rasterized density field (paper §4.1/§4.2: "the discretized bristle
// density and velocity fields are treated as boundary conditions in pressure
// projection"). Window-sized buffer, indexed by window-local coords.
StructuredBuffer<float> bristle_density : register(t5);
// Bristle rasterized VELOCITY (§4.1/§4.2: the density AND velocity fields are
// the pressure-projection boundary conditions). Window-sized; bristle_vel/
// bristle_density is the average bristle sample velocity at the cell — the
// moving-wall velocity of the brush boundary.
StructuredBuffer<float> bristle_vel_x : register(t6);
StructuredBuffer<float> bristle_vel_y : register(t7);
StructuredBuffer<float> bristle_vel_z : register(t8);
// Rasterized swarm density (paper §4.1/§4.3 joint liquid field: the FLIP
// particles ARE liquid). Window-sized like bristle_density.
StructuredBuffer<float> ptcl_density : register(t9);

RWStructuredBuffer<float> div_out : register(u0);

// Solid in pressure projection (paper §4.2: "Once the dryness reaches a
// threshold, a cell is considered as a solid cell"). The density gate on the
// dryness test is essential: an EMPTY cell (no paint ever deposited) has
// wetness == 0 by initialization, but it is NOT solid — fluid must flow into
// empty space. Only cells that HAVE paint and have dried below the wetness
// threshold block the fluid.
bool is_solid(uint idx)
{
    return density[idx] > 1e-6f && wetness[idx] < 0.01f;
}

// Brush interior boundary (paper §4.2): a cell occupied by bristles acts as a
// no-flux wall during pressure projection, so grid liquid is deflected around
// the brush footprint rather than flowing through it. bristle_density is the
// §4.1 rasterized field (window-sized); the gate threshold is small so only
// cells genuinely under bristles count. Takes GLOBAL coords, converts to
// window-local; out-of-window cells are not brush.
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

// A cell blocks flow if it is dried-solid OR brush-interior (both no-flux).
bool is_blocked(uint idx, uint gx, uint gy, uint gz)
{
    return is_solid(idx) || is_brush_g(gx, gy, gz);
}

// LIQUID OCCUPANCY (paper §4.1/§4.3 joint field): a cell holds liquid if it
// has grid paint, rasterized swarm, or bristles (the brush carries liquid).
// A cell with NONE of these is AIR: the projection must not enforce
// incompressibility of empty space — without this test a brush moving
// through empty space "displaces" a fictional fluid, the pressure field it
// pumps is sampled by the FLIP swarm (the only real mass around), and the
// swarm surfs a fluid that does not exist (blob-test f12 eruption root).
// Air is the standard free surface: div = 0 here, p = 0 (Dirichlet, set in
// the Jacobi/gradient), and faces of liquid cells toward air are FREE (no
// divergence term).
bool is_air(uint idx, uint gx, uint gy, uint gz)
{
    if (is_brush_g(gx, gy, gz)) return false;  // bristles carry liquid
    if (density[idx] > 1e-6f) return false;    // grid paint
    int w = win_field_idx(
        int(gx), int(gy), int(gz), cb.window_origin_x, cb.window_origin_y,
        cb.window_origin_z, cb.window_size_x, cb.window_size_z);
    if (w >= 0 && ptcl_density[w] > 1e-6f) return false;  // swarm
    return true;
}

// MOVING-WALL velocity of a brush-interior cell (rasterized average bristle
// sample velocity; §4.2's velocity boundary condition). Out of window / not
// brush → 0.
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

[numthreads(256, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    int RES  = cb.res;
    int RESZ = cb.res_z;
    int WSX = cb.window_size_x;

    WindowCell c;
    if (!window_map(cb, dtid.x, c)) return;
    uint gx = c.x, gy = c.y, gz = c.z;
    uint idx = c.idx;
    uint RNN = (uint)(RES * RES);

    // This cell's window-local index (vel/div buffers are window-sized).
    int w0 = wnb(c, 0, 0, 0, WSX, cb.window_size_z);
    int wxm = wnb(c, -1, 0, 0, WSX, cb.window_size_z);
    int wxp = wnb(c, 1, 0, 0, WSX, cb.window_size_z);
    int wym = wnb(c, 0, -1, 0, WSX, cb.window_size_z);
    int wyp = wnb(c, 0, 1, 0, WSX, cb.window_size_z);
    int wzm = wnb(c, 0, 0, -1, WSX, cb.window_size_z);
    int wzp = wnb(c, 0, 0, 1, WSX, cb.window_size_z);

    // Brush interior or solid cell: treat as a wall (no divergence, no-flux
    // at the interface). Paper §4.2 boundary conditions. AIR cells likewise
    // produce no divergence — they are the free surface (p=0), not fluid.
    if (is_blocked(idx, gx, gy, gz) || is_air(idx, gx, gy, gz)) {
        div_out[w0] = 0.0f;
        return;
    }

    float div = 0.0f;
    float vx = ((w0 >= 0) ? vel_x[w0] : 0.0f), vy = ((w0 >= 0) ? vel_y[w0] : 0.0f), vz = ((w0 >= 0) ? vel_z[w0] : 0.0f);

    // Moving-wall boundary (§4.2) with SEPARATING-FACE EXEMPTION: a face
    // toward a BRUSH cell carries the wall's velocity component while the
    // brush DISPLACES liquid — i.e. the wall is approaching this cell along
    // the face normal, or stationary (plain no-flux). When the wall moves
    // AWAY along the face normal the interface is SEPARATING: the liquid
    // detaches from the brush (free surface), so the face contributes no
    // divergence term (same mirror treatment as a dried-solid face). Without
    // the exemption a lifting brush acts as a rising syringe plunger over the
    // paint pool and the projection suctions the whole pool into the air
    // (blob-test f12 eruption). A face toward DRIED-SOLID paint stays skipped
    // (stationary no-flux wall).
    // Per axis: low face substitutes while wv >= 0 (wall moving toward
    // +axis or stationary); high face substitutes while wv <= 0. Strictly
    // separating (low: wv < 0, high: wv > 0) → exempt (no term).

    // ∂u/∂x
    if (gx > 0 && gx < (uint)RES - 1) {
        if (is_brush_g(gx - 1, gy, gz)) {
            float wv = brush_wall_vel(gx - 1, gy, gz).x;
            if (wv >= 0.0f) div += vx - wv;
        }
        else if (!is_blocked(idx - 1, gx - 1, gy, gz) && !is_air(idx - 1, gx - 1, gy, gz))
            div += vx - ((wxm >= 0) ? vel_x[wxm] : 0.0f);
        if (is_brush_g(gx + 1, gy, gz)) {
            float wv = brush_wall_vel(gx + 1, gy, gz).x;
            if (wv <= 0.0f) div += wv - vx;
        }
        else if (!is_blocked(idx + 1, gx + 1, gy, gz) && !is_air(idx + 1, gx + 1, gy, gz))
            div += ((wxp >= 0) ? vel_x[wxp] : 0.0f) - vx;
    } else if (gx == 0) {
        if (is_brush_g(gx + 1, gy, gz)) {
            float wv = brush_wall_vel(gx + 1, gy, gz).x;
            if (wv <= 0.0f) div += wv - vx;
        }
        else if (!is_blocked(idx + 1, gx + 1, gy, gz) && !is_air(idx + 1, gx + 1, gy, gz))
            div += ((wxp >= 0) ? vel_x[wxp] : 0.0f) - vx;
    } else {
        if (is_brush_g(gx - 1, gy, gz)) {
            float wv = brush_wall_vel(gx - 1, gy, gz).x;
            if (wv >= 0.0f) div += vx - wv;
        }
        else if (!is_blocked(idx - 1, gx - 1, gy, gz) && !is_air(idx - 1, gx - 1, gy, gz))
            div += vx - ((wxm >= 0) ? vel_x[wxm] : 0.0f);
    }

    // ∂v/∂y
    if (gy > 0 && gy < (uint)RES - 1) {
        if (is_brush_g(gx, gy - 1, gz)) {
            float wv = brush_wall_vel(gx, gy - 1, gz).y;
            if (wv >= 0.0f) div += vy - wv;
        }
        else if (!is_blocked(idx - (uint)RES, gx, gy - 1, gz) && !is_air(idx - (uint)RES, gx, gy - 1, gz))
            div += vy - ((wym >= 0) ? vel_y[wym] : 0.0f);
        if (is_brush_g(gx, gy + 1, gz)) {
            float wv = brush_wall_vel(gx, gy + 1, gz).y;
            if (wv <= 0.0f) div += wv - vy;
        }
        else if (!is_blocked(idx + (uint)RES, gx, gy + 1, gz) && !is_air(idx + (uint)RES, gx, gy + 1, gz))
            div += ((wyp >= 0) ? vel_y[wyp] : 0.0f) - vy;
    } else if (gy == 0) {
        if (is_brush_g(gx, gy + 1, gz)) {
            float wv = brush_wall_vel(gx, gy + 1, gz).y;
            if (wv <= 0.0f) div += wv - vy;
        }
        else if (!is_blocked(idx + (uint)RES, gx, gy + 1, gz) && !is_air(idx + (uint)RES, gx, gy + 1, gz))
            div += ((wyp >= 0) ? vel_y[wyp] : 0.0f) - vy;
    } else {
        if (is_brush_g(gx, gy - 1, gz)) {
            float wv = brush_wall_vel(gx, gy - 1, gz).y;
            if (wv >= 0.0f) div += vy - wv;
        }
        else if (!is_blocked(idx - (uint)RES, gx, gy - 1, gz) && !is_air(idx - (uint)RES, gx, gy - 1, gz))
            div += vy - ((wym >= 0) ? vel_y[wym] : 0.0f);
    }

    // ∂w/∂z
    if (RESZ > 1) {
        if (gz > 0 && gz < (uint)RESZ - 1) {
            if (is_brush_g(gx, gy, gz - 1)) {
                float wv = brush_wall_vel(gx, gy, gz - 1).z;
                if (wv >= 0.0f) div += vz - wv;
            }
            else if (!is_blocked(idx - RNN, gx, gy, gz - 1) && !is_air(idx - RNN, gx, gy, gz - 1))
                div += vz - ((wzm >= 0) ? vel_z[wzm] : 0.0f);
            if (is_brush_g(gx, gy, gz + 1)) {
                float wv = brush_wall_vel(gx, gy, gz + 1).z;
                if (wv <= 0.0f) div += wv - vz;
            }
            else if (!is_blocked(idx + RNN, gx, gy, gz + 1) && !is_air(idx + RNN, gx, gy, gz + 1))
                div += ((wzp >= 0) ? vel_z[wzp] : 0.0f) - vz;
        } else if (gz == 0) {
            if (is_brush_g(gx, gy, gz + 1)) {
                float wv = brush_wall_vel(gx, gy, gz + 1).z;
                if (wv <= 0.0f) div += wv - vz;
            }
            else if (!is_blocked(idx + RNN, gx, gy, gz + 1) && !is_air(idx + RNN, gx, gy, gz + 1))
                div += ((wzp >= 0) ? vel_z[wzp] : 0.0f) - vz;
        } else {
            if (is_brush_g(gx, gy, gz - 1)) {
                float wv = brush_wall_vel(gx, gy, gz - 1).z;
                if (wv >= 0.0f) div += vz - wv;
            }
            else if (!is_blocked(idx - RNN, gx, gy, gz - 1) && !is_air(idx - RNN, gx, gy, gz - 1))
                div += vz - ((wzm >= 0) ? vel_z[wzm] : 0.0f);
        }
    }

    div_out[w0] = -div;
}
