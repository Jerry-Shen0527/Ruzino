// Canvas commit — collapse a 3D window column into the 2D canvas layer.
// Wetbrush §4.2: dried/committed paint lives on the canvas as a height field.
// For each (x,y) cell in the active window, sum the Z-column of density and
// mass-weight the RYB color, then ADD into the 2D canvas layer. The canvas
// layer is the persistent "memory" that survives after the brush window
// moves elsewhere — without it, paint outside the window would be lost.
//
// One thread per (x,y) column. Reads the FULL window volume (WIN_XY*WIN_XY*WIN_Z),
// writes one cell to each of the 5 canvas fields.

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);

// 3D window fields (read) — indexed by window-local coords [0,WIN)³
StructuredBuffer<float> density  : register(t0);
StructuredBuffer<float> color_r  : register(t1);
StructuredBuffer<float> color_y  : register(t2);
StructuredBuffer<float> color_b  : register(t3);
StructuredBuffer<float> wetness  : register(t4);

// 2D canvas layer (read-modify-write via atomic add on ByteAddressBuffer)
// Using RWByteAddressBuffer so we can atomicFloatAdd — canvas is global and
// may be written by overlapping windows over time, but within a single commit
// pass each (x,y) is unique, so plain RWStructuredBuffer is safe too.
RWStructuredBuffer<float> canvas_density : register(u0);
RWStructuredBuffer<float> canvas_color_r : register(u1);
RWStructuredBuffer<float> canvas_color_y : register(u2);
RWStructuredBuffer<float> canvas_color_b : register(u3);
RWStructuredBuffer<float> canvas_wetness : register(u4);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    // Thread id maps to window-local (x,y). WIN_XY*WIN_XY columns total.
    int wsz_xy = cb.window_size_x;   // WIN_XY
    int wsz_z  = cb.window_size_z;   // WIN_Z
    int total_cols = wsz_xy * wsz_xy;
    if ((int)tid.x >= total_cols) return;

    int lx = (int)tid.x % wsz_xy;    // window-local x
    int ly = (int)tid.x / wsz_xy;    // window-local y

    // Global grid cell this window cell maps to.
    int gx = lx + cb.window_origin_x;
    int gy = ly + cb.window_origin_y;
    int N = cb.res;
    if (gx < 0 || gx >= N || gy < 0 || gy >= N) return;

    // Collapse the Z-column: accumulate density, mass-weight color.
    float sum_density = 0.0;
    float sum_r = 0.0, sum_y = 0.0, sum_b = 0.0;
    float max_wet = 0.0;
    for (int lz = 0; lz < wsz_z; lz++) {
        // Window-local linear index: z * wsz_xy*wsz_xy + y * wsz_xy + x
        uint widx = (uint)(lz * wsz_xy * wsz_xy + ly * wsz_xy + lx);
        float d = density[widx];
        if (d > 1e-6) {
            sum_density += d;
            sum_r += color_r[widx] * d;
            sum_y += color_y[widx] * d;
            sum_b += color_b[widx] * d;
        }
        max_wet = max(max_wet, wetness[widx]);
    }

    // Global 2D canvas index (row-major: y*N + x)
    uint cidx = (uint)(gy * N + gx);

    // Add into canvas (accumulate over multiple frames/windows).
    canvas_density[cidx] += sum_density;
    if (sum_density > 1e-6) {
        // Mass-weighted average blended additively into existing canvas color.
        canvas_color_r[cidx] += sum_r;
        canvas_color_y[cidx] += sum_y;
        canvas_color_b[cidx] += sum_b;
    }
    canvas_wetness[cidx] = max(canvas_wetness[cidx], max_wet);
}
