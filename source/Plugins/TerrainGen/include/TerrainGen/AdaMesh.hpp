// AdaMesh.hpp -- adaptive heightfield meshing, tile/quadtree scheme.
//
// The field (res x res cells) is divided into C x C-cell tiles. Each tile
// gets a subdivision LEVEL: its mesh quad stride is s = C >> level, so a
// higher level means a finer mesh. After balancing (neighbor levels differ
// by at most 1), a tile "stitches" every side that faces a FINER neighbor:
// that side's edge vertices double to the neighbor's spacing, and the
// connecting ring of the tile is triangulated with a cyclic fan per quad
// cell. This makes the assembled mesh watertight (no T-junction cracks)
// with all shared vertices bit-identical across tiles.
//
// This header is the single source of truth for the slot layout, index
// math and the tile mesher. terrain_ada_common.slangh mirrors it for the
// GPU dispatch, and terrain_ada_stitch_test.cpp pins the semantics down
// (watertightness over all 16 stitch combinations, height fidelity,
// orientation).
//
// Everything here is pure CPU math with a height-sampler callback; no RHI,
// no USD.

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace TerrainGen {
namespace ada {

    struct TileMeshInfo {
        int stride;  // s = C >> level, cells per mesh quad edge (>= 1)
        bool stitch_n = false;  // +z side faces a finer neighbor
        bool stitch_e = false;  // +x side
        bool stitch_s = false;  // -z side
        bool stitch_w = false;  // -x side
    };

    // ---- Slot layout (must match terrain_ada_common.slangh) -----------------
    //
    // [ interior (C/s+1)^2 | S mids | N mids | W mids | E mids ]
    // side-mid slots exist only when that side is stitched.

    inline int interior_dim(int C, int s)
    {
        return C / s + 1;
    }

    inline int side_mid_count(int C, int s)
    {
        return C / s;
    }

    inline int tile_vertex_count(int C, const TileMeshInfo& t)
    {
        int n = interior_dim(C, t.stride) * interior_dim(C, t.stride);
        if (t.stitch_s)
            n += side_mid_count(C, t.stride);
        if (t.stitch_n)
            n += side_mid_count(C, t.stride);
        if (t.stitch_w)
            n += side_mid_count(C, t.stride);
        if (t.stitch_e)
            n += side_mid_count(C, t.stride);
        return n;
    }

    // Per-quad-cell triangle count: the cyclic fan over (4 + #stitched sides)
    // boundary points yields (points - 2) triangles. A stitched side touches
    // C/s cells; a corner cell where two stitched sides meet correctly gets
    // both (6 points -> 4 triangles), so the sums just add.
    inline int tile_index_count(int C, const TileMeshInfo& t)
    {
        const int cells = (C / t.stride) * (C / t.stride);
        const int segs = C / t.stride;
        int stitch_sides = 0;
        const bool sides[4] = {
            t.stitch_n, t.stitch_e, t.stitch_s, t.stitch_w
        };
        for (const bool b : sides)
            stitch_sides += b ? 1 : 0;
        return (2 * cells + stitch_sides * segs) * 3;
    }

    // Interior slot id from (i, j) lattice coords, i,j in [0, C/s].
    inline int interior_slot(int C, int s, int i, int j)
    {
        return j * interior_dim(C, s) + i;
    }

    // Side-mid slot base offsets (after the interior block). Order S, N, W, E.
    inline void side_mid_base(int C, const TileMeshInfo& t, int base[4])
    {
        const int d = interior_dim(C, t.stride);
        const int m = side_mid_count(C, t.stride);
        int cur = d * d;
        // S, N, W, E
        const bool flags[4] = {
            t.stitch_s, t.stitch_n, t.stitch_w, t.stitch_e
        };
        for (int k = 0; k < 4; ++k) {
            base[k] = flags[k] ? cur : -1;
            if (flags[k])
                cur += m;
        }
    }

    // Local cell coordinates of a vertex slot (mirror of slang slot_coords()).
    // Returns false for an inactive slot (e.g. a side-mid slot of an
    // unstitched side).
    inline bool
    slot_coords(int C, const TileMeshInfo& t, int slot, int& lx, int& ly)
    {
        const int s = t.stride;
        const int d = interior_dim(C, s);
        if (slot < d * d) {
            lx = (slot % d) * s;
            ly = (slot / d) * s;
            return true;
        }
        const int m = side_mid_count(C, s);
        int base[4];
        side_mid_base(C, t, base);
        if (t.stitch_s && slot >= base[0] && slot < base[0] + m) {
            lx = (slot - base[0]) * s + s / 2;
            ly = 0;
            return true;
        }
        if (t.stitch_n && slot >= base[1] && slot < base[1] + m) {
            lx = (slot - base[1]) * s + s / 2;
            ly = C;
            return true;
        }
        if (t.stitch_w && slot >= base[2] && slot < base[2] + m) {
            lx = 0;
            ly = (slot - base[2]) * s + s / 2;
            return true;
        }
        if (t.stitch_e && slot >= base[3] && slot < base[3] + m) {
            lx = C;
            ly = (slot - base[3]) * s + s / 2;
            return true;
        }
        return false;
    }

    // Side-mid slot for side k (0=S, 1=N, 2=W, 3=E), segment index seg.
    inline uint32_t
    mid_id(const TileMeshInfo& t, const int base[4], int side, int seg)
    {
        return static_cast<uint32_t>(base[side] + seg);
    }

    // Emit one tile's triangles into `indices` (appending). Winding is CCW
    // seen from +Y, matching mesh_from_heightfield's (i0, i2, i1).
    inline void emit_tile_indices(
        int C,
        const TileMeshInfo& t,
        std::vector<uint32_t>& indices)
    {
        const int s = t.stride;
        const int nseg = C / s;
        int mid_base[4];
        side_mid_base(C, t, mid_base);

        for (int b = 0; b < nseg; ++b) {
            for (int a = 0; a < nseg; ++a) {
                // Corner slots of this s-cell (interior lattice).
                const uint32_t p00 =
                    static_cast<uint32_t>(interior_slot(C, s, a, b));
                const uint32_t p10 =
                    static_cast<uint32_t>(interior_slot(C, s, a + 1, b));
                const uint32_t p11 =
                    static_cast<uint32_t>(interior_slot(C, s, a + 1, b + 1));
                const uint32_t p01 =
                    static_cast<uint32_t>(interior_slot(C, s, a, b + 1));

                // Cyclic boundary walk in CCW-from-+Y order, starting at p00:
                // p00 -> (+z) p01 -> (+x) p11 -> (-z) p10 -> (-x) back to
                // p00. A side mid exists only when the cell touches that tile
                // boundary AND the boundary is stitched.
                uint32_t ring[8];
                int n = 0;
                ring[n++] = p00;
                if (t.stitch_w && a == 0)
                    ring[n++] = mid_id(t, mid_base, 2, b);  // W
                ring[n++] = p01;
                if (t.stitch_n && b == nseg - 1)
                    ring[n++] = mid_id(t, mid_base, 1, a);  // N
                ring[n++] = p11;
                if (t.stitch_e && a == nseg - 1)
                    ring[n++] = mid_id(t, mid_base, 3, b);  // E
                ring[n++] = p10;
                if (t.stitch_s && b == 0)
                    ring[n++] = mid_id(t, mid_base, 0, a);  // S

                // Fan apex: a corner whose two adjacent sides carry no mids
                // (a mid on an apex-adjacent side would fan a zero-area
                // triangle). The CELL's effective stitches decide -- a cell
                // only touches the tile boundary rows/cols it overlaps, so
                // opposite stitched tile sides never land on one cell (and a
                // full-stride cell can never stitch: level 0 has no finer
                // neighbor). For any 1- or 2-adjacent-stitched set a clean
                // corner exists.
                const bool eff_n = t.stitch_n && b == nseg - 1;
                const bool eff_e = t.stitch_e && a == nseg - 1;
                const bool eff_s = t.stitch_s && b == 0;
                const bool eff_w = t.stitch_w && a == 0;
                uint32_t apex = p00;
                if (eff_w && eff_s)
                    apex = p11;
                else if (eff_s && eff_e)
                    apex = p01;
                else if (eff_n && eff_e)
                    apex = p00;
                else if (eff_n && eff_w)
                    apex = p10;
                else if (eff_w)
                    apex = p11;
                else if (eff_s)
                    apex = p01;
                else if (eff_n)
                    apex = p10;
                else if (eff_e)
                    apex = p00;

                int apex_pos = 0;
                for (int k = 0; k < n; ++k) {
                    if (ring[k] == apex) {
                        apex_pos = k;
                        break;
                    }
                }
                for (int k = 1; k + 1 < n; ++k) {
                    indices.push_back(ring[apex_pos]);
                    indices.push_back(ring[(apex_pos + k) % n]);
                    indices.push_back(ring[(apex_pos + k + 1) % n]);
                }
            }
        }
    }

}  // namespace ada
}  // namespace TerrainGen
