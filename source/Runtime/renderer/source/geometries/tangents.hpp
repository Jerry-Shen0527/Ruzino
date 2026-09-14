// tangents.hpp -- pure CPU tangent-frame math for meshes, shared by the
// renderer (mesh.cpp) and the mesh_tangent_test unit tests.
//
// Everything here is the single source of truth for how per-vertex /
// per-corner tangents, normals and texcoords line up. The unit tests
// (source/Runtime/renderer/tests/mesh_tangent_test.cpp) lock the
// correspondence down; if you change a convention here (handedness sign,
// fallback axis, corner-vs-vertex indexing), a test will tell you.

#pragma once

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4f.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ruzino_render {
namespace tangents {

    struct TangentFrame {
        pxr::GfVec3f tangent;
        pxr::GfVec3f bitangent;
        bool valid;
    };

    // Exact dP/du, dP/dv solve for one triangle. On a degenerate UV triangle
    // (zero-area in UV space) returns valid=false with an axis-aligned
    // placeholder frame, mirroring the renderer's fallback.
    inline TangentFrame compute_triangle_tangent_frame(
        const pxr::GfVec3f& v0,
        const pxr::GfVec3f& v1,
        const pxr::GfVec3f& v2,
        const pxr::GfVec2f& uv0,
        const pxr::GfVec2f& uv1,
        const pxr::GfVec2f& uv2)
    {
        TangentFrame frame;
        frame.tangent = pxr::GfVec3f(1.0f, 0.0f, 0.0f);
        frame.bitangent = pxr::GfVec3f(0.0f, 1.0f, 0.0f);
        frame.valid = false;

        const pxr::GfVec3f dp1 = v1 - v0;
        const pxr::GfVec3f dp2 = v2 - v0;
        const pxr::GfVec2f duv1 = uv1 - uv0;
        const pxr::GfVec2f duv2 = uv2 - uv0;

        const float det = duv1[0] * duv2[1] - duv1[1] * duv2[0];
        if (std::abs(det) < 1e-10f) {
            return frame;
        }

        const float r = 1.0f / det;
        frame.tangent = (dp1 * duv2[1] - dp2 * duv1[1]) * r;
        frame.bitangent = (dp2 * duv1[0] - dp1 * duv2[0]) * r;
        frame.valid = true;
        return frame;
    }

    // Gram-Schmidt the tangent against the (given) normal and return
    // (t, handedness) with |t| == 1. handedness follows the mikktspace
    // convention: w = sign(dot(cross(n, t), bitangent)); a tangent parallel to
    // the normal falls back to an arbitrary perpendicular with w = +1.
    inline pxr::GfVec4f orthogonalize_tangent(
        const pxr::GfVec3f& normal,
        const pxr::GfVec3f& tangent,
        const pxr::GfVec3f& bitangent)
    {
        pxr::GfVec3f n = normal;
        n.Normalize();

        pxr::GfVec3f t = tangent - n * pxr::GfDot(n, tangent);
        const float tLen = t.GetLength();
        if (tLen > 1e-6f) {
            t = t / tLen;
            const float handedness =
                pxr::GfDot(pxr::GfCross(n, t), bitangent) >= 0.0f ? 1.0f
                                                                  : -1.0f;
            return pxr::GfVec4f(t[0], t[1], t[2], handedness);
        }

        if (std::abs(n[0]) < 0.9f) {
            t = pxr::GfVec3f(1.0f, 0.0f, 0.0f);
        }
        else {
            t = pxr::GfVec3f(0.0f, 1.0f, 0.0f);
        }
        t = t - n * pxr::GfDot(n, t);
        t.Normalize();
        return pxr::GfVec4f(t[0], t[1], t[2], 1.0f);
    }

    // Area-weighted per-vertex normals gathered with the SAME corner indexing
    // as the triangle list: normals[] may be FaceVarying (one per corner), so
    // indexing it with a VERTEX id reads an arbitrary triangle's corner normal.
    // That mis-indexing was the all-normal-maps-dark bug of 2026-09-07.
    // Array-like params accept Vt arrays and std::vector alike.
    template<class Vec3Array, class Vec3iArray>
    std::vector<pxr::GfVec3f> gather_vertex_normals(
        const Vec3Array& points,
        const Vec3iArray& tris,
        const Vec3Array& normals)
    {
        std::vector<pxr::GfVec3f> out(
            points.size(), pxr::GfVec3f(0.0f, 0.0f, 0.0f));

        const bool face_varying = normals.size() == tris.size() * 3;
        const bool vertex_interp = normals.size() == points.size();

        for (size_t t = 0; t < tris.size(); ++t) {
            const uint32_t i0 = static_cast<uint32_t>(tris[t][0]);
            const uint32_t i1 = static_cast<uint32_t>(tris[t][1]);
            const uint32_t i2 = static_cast<uint32_t>(tris[t][2]);

            const pxr::GfVec3f v0 = points[i0];
            const pxr::GfVec3f v1 = points[i1];
            const pxr::GfVec3f v2 = points[i2];
            const float area =
                pxr::GfCross(v1 - v0, v2 - v0).GetLength() * 0.5f;

            pxr::GfVec3f face_normal(0.0f);
            if (!face_varying && !vertex_interp) {
                face_normal = pxr::GfCross(v1 - v0, v2 - v0).GetNormalized();
            }

            for (int c = 0; c < 3; ++c) {
                const uint32_t i = (c == 0) ? i0 : (c == 1) ? i1 : i2;
                pxr::GfVec3f n(0.0f);
                if (face_varying) {
                    n = normals[t * 3 + static_cast<size_t>(c)];
                }
                else if (vertex_interp) {
                    n = normals[i];
                }
                else {
                    n = face_normal;
                }
                out[i] += n * area;
            }
        }
        return out;
    }

    // Vertex-interpolation tangents: accumulate the area-weighted UV gradient
    // frame per vertex, then orthogonalize against the GATHERED vertex normal
    // (never a corner normal indexed by vertex id). UV-degenerate triangles are
    // skipped entirely -- they contribute neither tangent nor normal weight,
    // matching the renderer's long-standing contract.
    template<class Vec3Array, class Vec3iArray, class Vec2Array>
    std::vector<pxr::GfVec4f> compute_vertex_tangents(
        const Vec3Array& points,
        const Vec3iArray& tris,
        const Vec3Array& normals,
        const Vec2Array& texcoords)
    {
        std::vector<pxr::GfVec4f> acc_t(
            points.size(), pxr::GfVec4f(0.0f, 0.0f, 0.0f, 0.0f));
        std::vector<pxr::GfVec3f> acc_b(
            points.size(), pxr::GfVec3f(0.0f, 0.0f, 0.0f));
        std::vector<pxr::GfVec3f> vertex_normals(
            points.size(), pxr::GfVec3f(0.0f, 0.0f, 0.0f));

        const bool face_varying = normals.size() == tris.size() * 3;
        const bool vertex_interp = normals.size() == points.size();

        for (size_t t = 0; t < tris.size(); ++t) {
            const uint32_t i0 = static_cast<uint32_t>(tris[t][0]);
            const uint32_t i1 = static_cast<uint32_t>(tris[t][1]);
            const uint32_t i2 = static_cast<uint32_t>(tris[t][2]);
            if (texcoords.size() <= std::max({ i0, i1, i2 })) {
                continue;
            }

            const pxr::GfVec3f v0 = points[i0];
            const pxr::GfVec3f v1 = points[i1];
            const pxr::GfVec3f v2 = points[i2];

            const TangentFrame frame = compute_triangle_tangent_frame(
                v0, v1, v2, texcoords[i0], texcoords[i1], texcoords[i2]);
            if (!frame.valid) {
                continue;
            }

            const float area =
                pxr::GfCross(v1 - v0, v2 - v0).GetLength() * 0.5f;

            const pxr::GfVec4f tw(
                frame.tangent[0] * area,
                frame.tangent[1] * area,
                frame.tangent[2] * area,
                0.0f);
            const pxr::GfVec3f bw = frame.bitangent * area;
            acc_t[i0] += tw;
            acc_t[i1] += tw;
            acc_t[i2] += tw;
            acc_b[i0] += bw;
            acc_b[i1] += bw;
            acc_b[i2] += bw;

            pxr::GfVec3f face_normal(0.0f);
            if (!face_varying && !vertex_interp) {
                face_normal = pxr::GfCross(v1 - v0, v2 - v0).GetNormalized();
            }
            const uint32_t idx[3] = { i0, i1, i2 };
            for (int c = 0; c < 3; ++c) {
                pxr::GfVec3f n(0.0f);
                if (face_varying) {
                    n = normals[t * 3 + static_cast<size_t>(c)];
                }
                else if (vertex_interp) {
                    n = normals[idx[c]];
                }
                else {
                    n = face_normal;
                }
                vertex_normals[idx[c]] += n * area;
            }
        }

        std::vector<pxr::GfVec4f> out(points.size());
        for (size_t i = 0; i < out.size(); ++i) {
            out[i] = orthogonalize_tangent(
                vertex_normals[i],
                pxr::GfVec3f(acc_t[i][0], acc_t[i][1], acc_t[i][2]),
                acc_b[i]);
        }
        return out;
    }

}  // namespace tangents
}  // namespace ruzino_render
