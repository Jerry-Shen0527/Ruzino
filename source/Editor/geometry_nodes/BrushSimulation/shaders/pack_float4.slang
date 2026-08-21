// Pack density + 3 color channels (4 separate float buffers) into one
// interleaved float4 buffer. Used by brush_wb_commit to produce the Float4
// paint-field buffer that the render rprim consumes (via the shared GPU buffer
// registry or the USD primvar fallback).
//
// Dispatched with one thread per global grid cell (res * res * res_z).
//
// PARTICLE-SWARM COMPOSITE: grid_to_particle drains the grid density within D0
// of the brush into (invisible-to-the-render) particles every frame, so a
// grid-only pack leaves a "moat" of missing paint around the brush — the stroke
// renders broken at its trailing end (paper §5.2: near the brush the liquid IS
// particles). Composite the per-frame rasterized swarm (ptcl_density /
// ptcl_color_*, window-sized, filled by particle_rasterize in the fluid node)
// on top of the grid field, exactly like paper §6 renders the active-window
// liquid. Both sides are mass-premultiplied, so the composite is a plain sum
// before normalization. One-frame lag: the raster predates this frame's §5.2
// conversions, and a particle that deposited this frame is counted on both
// sides for one frame (slightly brighter fresh trail, self-correcting).

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> density : register(t0);
StructuredBuffer<float> color_r : register(t1);
StructuredBuffer<float> color_y : register(t2);
StructuredBuffer<float> color_b : register(t3);
// Window-sized particle raster grids (particle_rasterize outputs).
StructuredBuffer<float> ptcl_density : register(t4);
StructuredBuffer<float> ptcl_color_r : register(t5);
StructuredBuffer<float> ptcl_color_y : register(t6);
StructuredBuffer<float> ptcl_color_b : register(t7);

RWStructuredBuffer<float4> packed_out : register(u0);

// Soft-normalize a raw mass-scaled density into [0,1) for the volume renderer.
// The sim stores density as accumulated paint mass per cell (values from ~1 to
// hundreds), but the render shaders assume a [0,1] fill fraction
// (kDensitySurface = 0.02; AO clamps density via min(d, 1.0); penetration depth
// scales by density). Feeding raw mass saturates every clamp and drives
// (1 - ao) to 0, rendering thick paint as pure black. d / (d + D_REF) maps
// D_REF → 0.5 and asymptotes to 1, so a typical cell (tens) reads as a strong
// but non-saturating surface. Chosen so kDensitySurface=0.02 ⟺ raw d ≈ 1.0.
static const float D_REF = 50.0;

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint total = uint(cb.res) * uint(cb.res) * uint(cb.res_z);
    if (tid.x >= total) return;

    float d = density[tid.x];
    float3 col_pre = float3(color_r[tid.x], color_y[tid.x], color_b[tid.x]);

    // Swarm composite: add the rasterized particle mass/pigment for cells
    // inside the (brush-following) active window. Window buffers are indexed
    // window-locally (lz*WSX*WSX + ly*WSX + lx); invert the global linear
    // index with the same order grid_idx_3d uses.
    if (cb.ptcl_render_scale > 0.0) {
        uint N = uint(cb.res);
        uint x = tid.x % N;
        uint y = (tid.x / N) % N;
        uint z = tid.x / (N * N);
        int lx = int(x) - cb.window_origin_x;
        int ly = int(y) - cb.window_origin_y;
        int lz = int(z) - cb.window_origin_z;
        int WSX = cb.window_size_x;
        if (lx >= 0 && lx < WSX && ly >= 0 && ly < WSX && lz >= 0 &&
            lz < cb.window_size_z) {
            uint win_idx = uint(lz) * uint(WSX * WSX) + uint(ly) * uint(WSX) +
                           uint(lx);
            float s = cb.ptcl_render_scale;
            d += s * ptcl_density[win_idx];
            col_pre += s * float3(ptcl_color_r[win_idx],
                                  ptcl_color_y[win_idx],
                                  ptcl_color_b[win_idx]);
        }
    }

    // Color channels are premultiplied (Σ pigment·mass). Recover the
    // normalized RYB pigment by dividing by density, then convert to RGB for
    // the renderer (Gossett & Chen, common.slangh::ryb_to_rgb). The renderer
    // uses packed.xyz directly as RGB radiance — it has no RYB→RGB step.
    float3 rgb = float3(0.0);
    if (d > 1e-6) {
        float3 ryb = col_pre / d;
        // Clamp to valid RYB [0,1]: the grid↔particle color transfer
        // (grid_to_particle / particle_to_grid) is not perfectly conservative
        // under racing -= on RWStructuredBuffer, so prolonged circulation can
        // drift a channel slightly negative or above 1. Gossett&Chen RYB→RGB
        // is only defined on [0,1]; out-of-range values blow up to white/black.
        ryb = clamp(ryb, float3(0.0), float3(1.0));
        rgb = ryb_to_rgb(ryb.x, ryb.y, ryb.z);
    }
    // Linear-normalize raw mass-per-cell into [0,1] for the volume renderer.
    // Measured per-voxel density for a 1024²×64 stroke: min 0.001, median
    // 0.041, p90 0.109, max ~0.285. D_MAX=0.05 saturated 43% of the painted
    // voxels to 1.0, flattening the field the renderer shades — gradient
    // normals, AO and penetration all read a uniform slab. D_MAX=0.3 maps the
    // measured max to ~0.95 and keeps the interior (0.04-0.28) spread across
    // [0.13,0.95], so the density gradient carries spatial variation again.
    // First-cross still fires: kDensitySurface=0.02 now ⟺ raw d ≈ 0.006, well
    // below the interior floor (~0.04).
    float d_norm = clamp(d / 0.3, 0.0, 1.0);

    packed_out[tid.x] = float4(d_norm, rgb);
}
