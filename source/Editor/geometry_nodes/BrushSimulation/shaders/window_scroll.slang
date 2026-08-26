// Scroll a persistent WINDOW-SIZED field when the active window moves
// (paper §4.2: "We update the window location at the beginning of each time
// step"). The velocity field and the pressure warm-start persist across
// frames INSIDE the window; when the window origin shifts, the overlap region
// must be re-based to the new window-local coordinates (cells left behind the
// window are discarded — there is no liquid motion outside the window).
//
// One thread per NEW window cell: read the same GLOBAL cell from the old
// window buffer if it was inside the old window, else 0. src and dst are
// different buffers (the host swaps handles after), so no read/write race.

#include "common.slangh"

struct ScrollCB {
    int old_ox, old_oy, old_oz;  // previous window origin (global cells)
    int new_ox, new_oy, new_oz;  // new window origin (global cells)
    int wsx;                     // window XY size
    int wsz;                     // window Z size
};

ConstantBuffer<ScrollCB> cb : register(b0);

StructuredBuffer<float> src_field : register(t0);
RWStructuredBuffer<float> dst_field : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int total = cb.wsx * cb.wsx * cb.wsz;
    if (int(tid.x) >= total) return;

    int lz = int(tid.x) / (cb.wsx * cb.wsx);
    int rem = int(tid.x) - lz * cb.wsx * cb.wsx;
    int ly = rem / cb.wsx;
    int lx = rem - ly * cb.wsx;

    int gx = lx + cb.new_ox, gy = ly + cb.new_oy, gz = lz + cb.new_oz;
    int sidx = win_field_idx(
        gx, gy, gz, cb.old_ox, cb.old_oy, cb.old_oz, cb.wsx, cb.wsz);
    dst_field[tid.x] = (sidx >= 0) ? src_field[sidx] : 0.0;
}
