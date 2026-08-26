// Pack density + 3 color channels (4 separate float buffers) into one
// interleaved float4 buffer. Used by brush_wb_commit to produce the Float4
// paint-field buffer that the render rprim consumes (via the shared GPU buffer
// registry or the USD primvar fallback).
//
// Dispatched with one thread per global grid cell (res * res * res_z).
//
// UNIFIED PAINT RENDERING: paper §6 renders the grid with raymarching and the
// near-brush liquid particles with a separate screen-space method, accepting a
// visible mode seam around the brush. We instead composite the per-frame
// particle raster (ptcl_density / ptcl_rast_*, mass-weighted, Σw=1 per
// particle — the same transient raster particle_rasterize.slang builds for
// the §4.3 joint velocity merge) into this field, so paint inside and outside
// the active window renders through ONE method. This is mass-consistent with
// the sim: grid_to_particle (Eq.15) subtracted exactly the mass the raster
// re-adds, so grid + composite ≈ the pre-drain total — no double counting
// (the earlier darker-disc bug splatted particles while the grid had NOT been
// drained; the §5.2 subtraction makes the composite the correct inverse).
// Particle POINT SPRITES (wetbrush_debug_particles) remain available as an
// opt-in debug visualization (WB_DRAW_PARTICLES=1), off by default.

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

StructuredBuffer<float> density : register(t0);
StructuredBuffer<float> color_r : register(t1);
StructuredBuffer<float> color_y : register(t2);
StructuredBuffer<float> color_b : register(t3);
// Window-sized particle raster (live swarm mass near the brush).
StructuredBuffer<float> ptcl_density : register(t4);
StructuredBuffer<float> ptcl_rast_r : register(t5);
StructuredBuffer<float> ptcl_rast_y : register(t6);
StructuredBuffer<float> ptcl_rast_b : register(t7);

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

    // Composite the swarm raster on window cells: the raster grids are
    // window-local, so map this global cell through win_field_idx (0 outside
    // the window — no particle mass there; the raster is cleared each frame
    // the brush is up, so a stale swarm never ghosts into the pack).
    {
        int res = cb.res;
        int gx = int(tid.x) % res;
        int gy = (int(tid.x) / res) % res;
        int gz = int(tid.x) / (res * res);
        int w = win_field_idx(
            gx, gy, gz, cb.window_origin_x, cb.window_origin_y,
            cb.window_origin_z, cb.window_size_x, cb.window_size_z);
        if (w >= 0) {
            d += ptcl_density[w];
            col_pre += float3(ptcl_rast_r[w], ptcl_rast_y[w], ptcl_rast_b[w]);
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
