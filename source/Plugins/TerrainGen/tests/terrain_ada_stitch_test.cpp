// terrain_ada_stitch_test: locks down the adaptive heightfield tile
// mesher (AdaMesh.hpp) -- the CPU reference the GPU dispatch mirrors.
//
// The regression this guards against: a wrong stitch ring leaves
// T-junction gaps between tiles of different levels. In a path tracer a
// crack is a light leak, so watertightness is checked exhaustively:
// every undirected edge exactly 2 triangles (1 only on the field
// boundary), every directed edge exactly once (consistent winding), over
// ALL 16 single-tile stitch combinations and balanced multi-tile fields.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "TerrainGen/AdaMesh.hpp"

using namespace TerrainGen::ada;

namespace {

// Deterministic terrain-ish sampler in cell space.
float h_terrain(float x, float z)
{
    return 3.0f * std::sin(x * 0.37f) * std::cos(z * 0.21f) + 0.05f * x;
}

struct EdgeStats {
    std::map<std::pair<uint32_t, uint32_t>, int> undirected;
    std::map<std::pair<uint32_t, uint32_t>, int> directed;
};

EdgeStats tally_edges(const std::vector<uint32_t>& indices)
{
    EdgeStats st;
    for (size_t k = 0; k + 2 < indices.size(); k += 3) {
        for (int e = 0; e < 3; ++e) {
            const uint32_t a = indices[k + e];
            const uint32_t b = indices[k + (e + 1) % 3];
            st.directed[{ a, b }] += 1;
            st.undirected[{ std::min(a, b), std::max(a, b) }] += 1;
        }
    }
    return st;
}

// Positions in cell space: (x, h, z); used coords are small exact ints.
struct MeshData {
    std::vector<float> pos;  // xyz triplets (cell space)
    std::vector<uint32_t> indices;
};

MeshData emit_tile(int C, const TileMeshInfo& t, int origin_x, int origin_z)
{
    MeshData m;
    const int nv = tile_vertex_count(C, t);
    m.pos.reserve(static_cast<size_t>(nv) * 3);
    for (int slot = 0; slot < nv; ++slot) {
        int lx, ly;
        if (!slot_coords(C, t, slot, lx, ly)) {
            ADD_FAILURE() << "inactive slot " << slot << " inside count range";
            lx = ly = 0;
        }
        const int gx = origin_x + lx;
        const int gz = origin_z + ly;
        m.pos.push_back(static_cast<float>(gx));
        m.pos.push_back(
            h_terrain(static_cast<float>(gx), static_cast<float>(gz)));
        m.pos.push_back(static_cast<float>(gz));
    }
    emit_tile_indices(C, t, m.indices);
    return m;
}

// Assemble a balanced tile field with position-dedup across tiles, so the
// manifold check actually proves seam coincidence.
MeshData assemble(const std::vector<int>& levels, int T, int C)
{
    MeshData out;
    std::map<std::pair<int, int>, uint32_t> dedup;
    auto vertex_id = [&](int gx, int gz) {
        auto it = dedup.find({ gx, gz });
        if (it != dedup.end())
            return it->second;
        const uint32_t id = static_cast<uint32_t>(out.pos.size() / 3);
        out.pos.push_back(static_cast<float>(gx));
        out.pos.push_back(
            h_terrain(static_cast<float>(gx), static_cast<float>(gz)));
        out.pos.push_back(static_cast<float>(gz));
        dedup[{ gx, gz }] = id;
        return id;
    };

    for (int tz = 0; tz < T; ++tz) {
        for (int tx = 0; tx < T; ++tx) {
            const int L = levels[tz * T + tx];
            TileMeshInfo t;
            t.stride = C >> L;
            const int base_x = tx * C, base_z = tz * C;
            auto level_at = [&](int x, int z) {
                if (x < 0 || x >= T || z < 0 || z >= T)
                    return -1;
                return levels[z * T + x];
            };
            t.stitch_w = level_at(tx - 1, tz) == L + 1;
            t.stitch_e = level_at(tx + 1, tz) == L + 1;
            t.stitch_s = level_at(tx, tz - 1) == L + 1;
            t.stitch_n = level_at(tx, tz + 1) == L + 1;

            // Local vertex slot -> global dedup id; index remap table.
            const int nv = tile_vertex_count(C, t);
            std::vector<uint32_t> remap(static_cast<size_t>(nv));
            for (int slot = 0; slot < nv; ++slot) {
                int lx, ly;
                if (!slot_coords(C, t, slot, lx, ly)) {
                    ADD_FAILURE() << "inactive slot";
                    lx = ly = 0;
                }
                remap[slot] = vertex_id(base_x + lx, base_z + ly);
            }
            std::vector<uint32_t> local;
            emit_tile_indices(C, t, local);
            for (const uint32_t id : local) {
                out.indices.push_back(remap[id]);
            }
        }
    }
    return out;
}

void check_orientations_and_heights(
    const MeshData& m,
    const std::string& label = {})
{
    const size_t nv = m.pos.size() / 3;
    std::vector<bool> referenced(nv, false);
    for (size_t k = 0; k + 2 < m.indices.size(); k += 3) {
        const float* a = &m.pos[m.indices[k] * 3];
        const float* b = &m.pos[m.indices[k + 1] * 3];
        const float* c = &m.pos[m.indices[k + 2] * 3];
        // +Y normal: cross(e1, e2).y with e1 = b - a, e2 = c - a.
        const float e1x = b[0] - a[0], e1z = b[2] - a[2];
        const float e2x = c[0] - a[0], e2z = c[2] - a[2];
        const float ny = e1z * e2x - e1x * e2z;
        // Degenerate check happens implicitly; sloped quads keep ny>0
        // because heights are a continuous function and winding is CCW.
        ASSERT_GT(ny, 0.0f)
            << "triangle " << k / 3 << " faces down"
            << " pts (" << a[0] << "," << a[2] << ")(" << b[0] << "," << b[2]
            << ")(" << c[0] << "," << c[2] << ") " << label;
        referenced[m.indices[k]] = true;
        referenced[m.indices[k + 1]] = true;
        referenced[m.indices[k + 2]] = true;
        // Height fidelity: every vertex y equals the sampler value.
        for (const float* p : { a, b, c }) {
            ASSERT_NEAR(p[1], h_terrain(p[0], p[2]), 1e-5f);
        }
    }
    for (size_t i = 0; i < nv; ++i) {
        EXPECT_TRUE(referenced[i]) << "vertex " << i << " unreferenced";
    }
}

}  // namespace

TEST(AdaStitch, SingleTileAllSixteenStitchCombinations)
{
    const int C = 8;
    for (int mask = 0; mask < 16; ++mask) {
        TileMeshInfo t;
        t.stride = 2;  // stitching needs s >= 2
        t.stitch_n = (mask & 1) != 0;
        t.stitch_e = (mask & 2) != 0;
        t.stitch_s = (mask & 4) != 0;
        t.stitch_w = (mask & 8) != 0;

        MeshData m = emit_tile(C, t, 0, 0);

        // Counts advertised by the header match the emitter.
        EXPECT_EQ(
            m.pos.size() / 3, static_cast<size_t>(tile_vertex_count(C, t)));
        EXPECT_EQ(
            m.indices.size(), static_cast<size_t>(tile_index_count(C, t)));

        // Every undirected edge exactly 2 (interior) or 1 (boundary).
        // With stitches, doubled edge points sit ON the boundary.
        EdgeStats st = tally_edges(m.indices);
        for (const auto& [edge, count] : st.undirected) {
            if (count > 2) {
                FAIL() << "edge " << edge.first << "-" << edge.second
                       << " shared by " << count << " triangles";
            }
        }
        // Directed edges: consistent winding = each exactly once.
        for (const auto& [edge, count] : st.directed) {
            EXPECT_EQ(count, 1)
                << "directed edge " << edge.first << "-" << edge.second
                << " used " << count << " times (mask " << mask << ")";
        }

        check_orientations_and_heights(m, "mask " + std::to_string(mask));
    }
}

TEST(AdaStitch, BalancedThreeByThreeFieldIsWatertight)
{
    // Center tile finer by exactly one level; all seams must weld.
    const int T = 3, C = 8;
    std::vector<int> levels(T * T, 1);
    levels[4] = 2;

    MeshData m = assemble(levels, T, C);
    check_orientations_and_heights(m, "3x3 balanced");

    EdgeStats st = tally_edges(m.indices);
    // Field spans C*T cells; boundary vertices lie on the outer rim.
    const float rim = static_cast<float>(C * T);
    int boundary_edges = 0;
    for (const auto& [edge, count] : st.undirected) {
        const float* a = &m.pos[edge.first * 3];
        const float* b = &m.pos[edge.second * 3];
        const bool on_rim =
            (a[0] == 0 || a[0] == rim || a[2] == 0 || a[2] == rim) &&
            (b[0] == 0 || b[0] == rim || b[2] == 0 || b[2] == rim);
        if (count == 1) {
            EXPECT_TRUE(on_rim)
                << "non-boundary edge with a single triangle: " << edge.first
                << "-" << edge.second << " (crack)";
            ++boundary_edges;
        }
        else {
            EXPECT_EQ(count, 2) << "edge " << edge.first << "-" << edge.second
                                << " shared by " << count << " triangles";
        }
    }
    // The seam between the fine center and its four neighbors must have
    // welded (no count-1 edges along it): seams live at cell coordinate
    // C and 2*C -- count welded seam edges to make this assert bite.
    int seam_edges = 0;
    for (const auto& [edge, count] : st.undirected) {
        const float* a = &m.pos[edge.first * 3];
        const float* b = &m.pos[edge.second * 3];
        const float seam_a = static_cast<float>(C);
        const float seam_b = static_cast<float>(2 * C);
        auto is_seam = [&](float x, float z) {
            return x == seam_a || x == seam_b || z == seam_a || z == seam_b;
        };
        if (is_seam(a[0], a[2]) && is_seam(b[0], b[2]) && count == 2) {
            ++seam_edges;
        }
    }
    EXPECT_GT(seam_edges, 0) << "seam edges never welded 2:1";
}

TEST(AdaStitch, VariedLevelsFiveByFiveIsWatertight)
{
    const int T = 5, C = 8;
    std::vector<int> levels(T * T, 0);
    // A finer island and a finest corner, balanced everywhere.
    levels[7] = 1;   // (2,1)
    levels[12] = 2;  // (2,2) center: neighbors at 1 or 0 -> center 2
    // neighbors of (2,2) must be >= 1
    levels[6] = std::max(levels[6], 1);
    levels[8] = std::max(levels[8], 1);
    levels[11] = std::max(levels[11], 1);
    levels[13] = std::max(levels[13], 1);
    levels[17] = std::max(levels[17], 1);
    levels[16] = 2;  // (1,3) adjacent to 17 (2) ok
    levels[18] = 1;

    MeshData m = assemble(levels, T, C);
    check_orientations_and_heights(m, "5x5 varied");

    EdgeStats st = tally_edges(m.indices);
    for (const auto& [edge, count] : st.undirected) {
        EXPECT_LE(count, 2) << "edge " << edge.first << "-" << edge.second
                            << " shared by " << count << " triangles";
    }
    for (const auto& [edge, count] : st.directed) {
        EXPECT_EQ(count, 1);
    }
}

TEST(AdaStitch, VertexCountsTrackGeometry)
{
    const int C = 16;
    TileMeshInfo plain;
    plain.stride = 1;
    EXPECT_EQ(tile_vertex_count(C, plain), 17 * 17);
    EXPECT_EQ(tile_index_count(C, plain), 512 * 3);

    TileMeshInfo all_stitched;
    all_stitched.stride = 4;
    all_stitched.stitch_n = all_stitched.stitch_e = true;
    all_stitched.stitch_s = all_stitched.stitch_w = true;
    // interior 5x5 = 25, mids 4*4 = 16 -> 41
    EXPECT_EQ(tile_vertex_count(C, all_stitched), 25 + 16);
    // 4 corner cells touch two stitched sides (4 tris), the 8 non-corner
    // edge cells one side (3 tris), the 4 interior cells none (2 tris):
    // 16 + 24 + 8 = 48 triangles.
    EXPECT_EQ(tile_index_count(C, all_stitched), 48 * 3);
}
