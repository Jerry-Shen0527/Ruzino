// Copy the ACTIVE WINDOW region between fields, in three modes (cb.copy_mode):
//   0: window→window — both buffers window-sized (e.g. the FLIP vel→vel_old
//      snapshot); src and dst are indexed window-locally.
//   1: global→window — src is a global canvas field (density/color), dst a
//      window-sized tmp (seeding grid_to_particle's Eq.15 subtract scratch).
//   2: window→global — src is a window-sized tmp, dst a global canvas field
//      (writing advected/scalar-subtracted results back).
//
// The transient solve fields (velocity family, pressure, divergence, scalar
// tmps) are window-sized per paper §4.2 — simulation runs only inside the
// brush-centered active window, so scratch never needs global extent. The
// global canvas fields (density/color/wetness/oil) persist everywhere paint
// exists; modes 1/2 bridge the two index spaces at the window boundary.

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);
StructuredBuffer<float> src_field : register(t0);
RWStructuredBuffer<float> dst_field : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    WindowCell c;
    if (!window_map(cb, tid.x, c)) return;
    int WSX = cb.window_size_x;
    uint widx = uint(c.lz) * uint(WSX * WSX) + uint(c.ly) * uint(WSX) +
                uint(c.lx);

    if (cb.copy_mode == 0)
        dst_field[widx] = src_field[widx];
    else if (cb.copy_mode == 1)
        dst_field[widx] = src_field[c.idx];
    else
        dst_field[c.idx] = src_field[widx];
}
