// Copy the ACTIVE WINDOW region of a source field into a destination field.
//
// Both buffers are global-sized (res × res × res_z); only the cells inside the
// active window [win_origin, win_origin+WIN) are copied, one thread per window
// cell. The window is a non-contiguous block inside the global buffer (each
// row of WIN_XY cells is separated by res−WIN_XY stride cells), so a plain
// copyBuffer(src, dst, win_n3d*sizeof(float)) would copy the corner block at
// offset 0 instead — a leftover from the pre-window era that made the FLIP
// velocity snapshot (vel → vel_old) read the wrong cells once the window
// moved off the corner. Dispatched with win_n3d threads.

#include "common.slangh"

ConstantBuffer<SimConstants> cb : register(b0);
StructuredBuffer<float> src_field : register(t0);
RWStructuredBuffer<float> dst_field : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    WindowCell c;
    if (!window_map(cb, tid.x, c)) return;
    dst_field[c.idx] = src_field[c.idx];
}
