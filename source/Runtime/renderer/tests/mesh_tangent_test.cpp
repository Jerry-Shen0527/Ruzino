// mesh_tangent_test: locks down the normal / tangent / texcoord
// correspondence in the renderer's CPU tangent computation (tangents.hpp,
// shared with mesh.cpp). Pure CPU tests -- no RHI, no GPU.
//
// The regression in the cube test is real: mesh.cpp used to index a
// FaceVarying corner normals[] array with VERTEX ids while
// orthogonalizing per-vertex tangents, which produced garbage tangent
// frames and made every normal-mapped render go dark (2026-09-07).

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "geometries/tangents.hpp"

using namespace pxr;
using ruzino_render::tangents::compute_triangle_tangent_frame;
using ruzino_render::tangents::compute_vertex_tangents;
using ruzino_render::tangents::gather_vertex_normals;
using ruzino_render::tangents::orthogonalize_tangent;

namespace {

bool vec_close(const GfVec3f& a, const GfVec3f& b, float eps = 1e-5f)
{
    return (a - b).GetLength() < eps;
}

// Flat quad in the XZ plane split into two triangles. Vertex-interp
// normal is +Y.
struct QuadMesh {
    std::vector<GfVec3f> points = { GfVec3f(0, 0, 0),
                                    GfVec3f(1, 0, 0),
                                    GfVec3f(1, 0, 1),
                                    GfVec3f(0, 0, 1) };
    std::vector<GfVec3i> tris = { GfVec3i(0, 1, 2), GfVec3i(0, 2, 3) };
    std::vector<GfVec3f> normals = { GfVec3f(0, 1, 0),
                                     GfVec3f(0, 1, 0),
                                     GfVec3f(0, 1, 0),
                                     GfVec3f(0, 1, 0) };
};

}  // namespace

// ---- Tangent direction ----

TEST(MeshTangent, PlaneTangentFollowsUVDirection)
{
    QuadMesh quad;
    // u along +X, v along +Z.
    const std::vector<GfVec2f> uvs = {
        GfVec2f(0, 0), GfVec2f(1, 0), GfVec2f(1, 1), GfVec2f(0, 1)
    };

    const auto tangents =
        compute_vertex_tangents(quad.points, quad.tris, quad.normals, uvs);

    ASSERT_EQ(tangents.size(), quad.points.size());
    for (const auto& t : tangents) {
        EXPECT_TRUE(
            vec_close(GfVec3f(t[0], t[1], t[2]), GfVec3f(1, 0, 0), 1e-4f));
        // B = cross(N, T) * w = cross(+Y, +X) * w = -Z * w, but the UV
        // bitangent is +Z, so w must be -1.
        EXPECT_NEAR(t[3], -1.0f, 1e-6f);
    }
}

TEST(MeshTangent, FlippedVFlipsHandedness)
{
    QuadMesh quad;
    // u along +X, v along -Z.
    const std::vector<GfVec2f> uvs = {
        GfVec2f(0, 0), GfVec2f(1, 0), GfVec2f(1, -1), GfVec2f(0, -1)
    };

    const auto tangents =
        compute_vertex_tangents(quad.points, quad.tris, quad.normals, uvs);

    for (const auto& t : tangents) {
        EXPECT_TRUE(
            vec_close(GfVec3f(t[0], t[1], t[2]), GfVec3f(1, 0, 0), 1e-4f));
        // Now cross(N, T) = -Z matches the UV bitangent (-Z): w = +1.
        EXPECT_NEAR(t[3], 1.0f, 1e-6f);
    }
}

TEST(MeshTangent, TriangleFrameMatchesHandDerivation)
{
    // One skewed triangle; the solved frame must reproduce dP/du, dP/dv.
    const GfVec3f v0(0, 0, 0), v1(2, 0, 0), v2(0, 3, 1);
    const GfVec2f uv0(0, 0), uv1(1, 0), uv2(1, 1);

    const auto frame =
        compute_triangle_tangent_frame(v0, v1, v2, uv0, uv1, uv2);
    ASSERT_TRUE(frame.valid);

    // uv1 - uv0 = (1,0) -> tangent along (v1 - v0) = (2,0,0).
    EXPECT_TRUE(vec_close(frame.tangent, GfVec3f(2, 0, 0), 1e-4f));
    // uv2 - uv1 = (0,1) -> bitangent along (v2 - v1) = (-2,3,1).
    EXPECT_TRUE(vec_close(frame.bitangent, GfVec3f(-2, 3, 1), 1e-4f));
}

// ---- Vertex normal gathering (THE regression) ----

namespace {

// Regular tetrahedron: 4 equal-area faces, and every vertex touches
// exactly 3 faces, so the area-weighted gathered normal has an exact
// closed form: normalize(v_k). A quad-split cube cannot do this -- the
// quad diagonal doubles the weight of the two corners it passes through.
struct TetMesh {
    std::vector<GfVec3f> points = { GfVec3f(1, 1, 1),
                                    GfVec3f(1, -1, -1),
                                    GfVec3f(-1, 1, -1),
                                    GfVec3f(-1, -1, 1) };
    // Outward winding: face normals point away from the opposite vertex.
    std::vector<GfVec3i> tris = {
        GfVec3i(1, 3, 2),  // opposite v0, normal -v0
        GfVec3i(0, 2, 3),  // opposite v1, normal -v1
        GfVec3i(0, 3, 1),  // opposite v2, normal -v2
        GfVec3i(0, 1, 2),  // opposite v3, normal -v3
    };
    std::vector<GfVec3f> corner_normals;  // 3 per triangle (FaceVarying)
    std::vector<GfVec3f> vertex_normals;  // FaceVarying-free variant

    TetMesh()
    {
        for (const auto& t : tris) {
            const GfVec3f n =
                GfCross(
                    points[t[1]] - points[t[0]], points[t[2]] - points[t[0]])
                    .GetNormalized();
            for (int k = 0; k < 3; ++k) {
                corner_normals.push_back(n);
            }
        }
        for (const auto& p : points) {
            vertex_normals.push_back(p.GetNormalized());
        }
    }
};

}  // namespace

TEST(MeshTangent, FaceVaryingNormalsGatheredByCornerNotByVertexId)
{
    TetMesh tet;
    ASSERT_EQ(tet.corner_normals.size(), tet.tris.size() * 3);

    const auto gathered =
        gather_vertex_normals(tet.points, tet.tris, tet.corner_normals);
    ASSERT_EQ(gathered.size(), 4);
    for (size_t i = 0; i < gathered.size(); ++i) {
        const GfVec3f expected = tet.points[i].GetNormalized();
        EXPECT_TRUE(vec_close(gathered[i].GetNormalized(), expected, 1e-4f))
            << "vertex " << i << ": got (" << gathered[i][0] << ", "
            << gathered[i][1] << ", " << gathered[i][2] << ")";
        // The old bug indexed corner_normals[] by VERTEX id: for every
        // vertex that is not corner 0/1/2 of the matching face, that reads
        // a foreign face's normal. A gathered normal must never equal a
        // single face normal here.
        for (const auto& cn : tet.corner_normals) {
            EXPECT_FALSE(vec_close(gathered[i].GetNormalized(), cn, 1e-3f));
        }
    }
}

TEST(MeshTangent, VertexTangentsOrthogonalToGatheredNormal)
{
    TetMesh tet;
    // Arbitrary NON-collinear vertex UVs (collinear UVs make every UV
    // triangle degenerate and skip the whole mesh): the tangent DIRECTION
    // depends on them, but after Gram-Schmidt every tangent must be
    // orthogonal to the gathered vertex normal -- orthogonalizing against
    // any single face normal (the old mis-indexed path) fails this.
    const std::vector<GfVec2f> uvs = {
        GfVec2f(0, 0), GfVec2f(1, 0), GfVec2f(0, 1), GfVec2f(1, 1)
    };

    const auto tangents =
        compute_vertex_tangents(tet.points, tet.tris, tet.corner_normals, uvs);
    ASSERT_EQ(tangents.size(), 4);

    for (size_t i = 0; i < tangents.size(); ++i) {
        const GfVec3f t(tangents[i][0], tangents[i][1], tangents[i][2]);
        EXPECT_NEAR(t.GetLength(), 1.0f, 1e-5f);
        EXPECT_NEAR(GfDot(t, tet.points[i].GetNormalized()), 0.0f, 1e-4f);
        EXPECT_NEAR(std::abs(tangents[i][3]), 1.0f, 1e-6f);
    }
}

TEST(MeshTangent, VertexInterpNormalsPassThrough)
{
    TetMesh tet;
    const auto gathered =
        gather_vertex_normals(tet.points, tet.tris, tet.vertex_normals);
    ASSERT_EQ(gathered.size(), 4);
    for (size_t i = 0; i < gathered.size(); ++i) {
        EXPECT_TRUE(vec_close(
            gathered[i].GetNormalized(), tet.vertex_normals[i], 1e-4f));
    }
}

// ---- Degenerate UV handling ----

TEST(MeshTangent, DegenerateUVTriangleIsSkipped)
{
    QuadMesh quad;
    // Second triangle collapses to a point in UV space: it must contribute
    // neither tangent nor normal weight, so the shared vertices keep the
    // exact +X tangent of the first triangle.
    const std::vector<GfVec2f> uvs = {
        GfVec2f(0, 0),
        GfVec2f(1, 0),
        GfVec2f(1, 1),
        GfVec2f(0, 0)  // makes tri (0,2,3) degenerate in UV
    };

    const auto tangents =
        compute_vertex_tangents(quad.points, quad.tris, quad.normals, uvs);

    for (const auto& t : tangents) {
        EXPECT_TRUE(
            vec_close(GfVec3f(t[0], t[1], t[2]), GfVec3f(1, 0, 0), 1e-4f));
    }
}

TEST(MeshTangent, DegenerateUVFrameReportsInvalid)
{
    const auto frame = compute_triangle_tangent_frame(
        GfVec3f(0, 0, 0),
        GfVec3f(1, 0, 0),
        GfVec3f(0, 0, 1),
        GfVec2f(0.5f, 0.5f),
        GfVec2f(0.5f, 0.5f),
        GfVec2f(0.5f, 0.5f));
    EXPECT_FALSE(frame.valid);
}

// ---- Fallback path ----

TEST(MeshTangent, TangentParallelToNormalFallsBackPerpendicular)
{
    // UV gradient exactly parallel to the normal -> after Gram-Schmidt the
    // tangent collapses; the fallback must still emit a unit tangent
    // orthogonal to the normal with w = +1.
    const GfVec3f n(0, 1, 0);
    const GfVec4f t =
        orthogonalize_tangent(n, GfVec3f(0, 2, 0), GfVec3f(0, 0, 1));
    EXPECT_NEAR(GfVec3f(t[0], t[1], t[2]).GetLength(), 1.0f, 1e-5f);
    EXPECT_NEAR(GfDot(GfVec3f(t[0], t[1], t[2]), n), 0.0f, 1e-5f);
    EXPECT_NEAR(t[3], 1.0f, 1e-6f);
}
