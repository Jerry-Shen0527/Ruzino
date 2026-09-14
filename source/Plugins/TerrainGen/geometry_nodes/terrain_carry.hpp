// Heightfield <-> grid-mesh carrier for the terrain node family.
//
// The terrain nodes do NOT introduce a new socket type: a Heightfield travels
// through the graph as a regular quad grid MESH whose vertices are lifted to
// the height (world Y) and whose solver layers ride as named vertex scalar
// quantities ("water" / "sediment" / "wear"). Transform, merge, write_usd and
// the viewport consume it unchanged; the erode nodes unpack, solve, and pack
// a fresh mesh.
//
// Everything here is `inline` so multiple node .cpp files can include this
// header without ODR violations.

#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include "GCore/Components/MeshComponent.h"
#include "GCore/GOP.h"
#include "TerrainGen/Heightfield.h"

namespace Ruzino {
namespace terrain_carry {

    inline constexpr const char* Q_WATER = "water";
    inline constexpr const char* Q_SEDIMENT = "sediment";
    inline constexpr const char* Q_WEAR = "wear";

    // Rebuild a Heightfield from a grid mesh produced by mesh_from_heightfield.
    // The resolution is inferred from the vertex count (res x res) and
    // validated against the quad topology; returns false (with `err`) for
    // meshes that are not a square grid. The caller owns `geom` (component
    // access is copy-on-write, so this never mutates a shared upstream mesh).
    inline bool heightfield_from_mesh(
        Geometry& geom,
        TerrainGen::Heightfield& out,
        std::string& err)
    {
        auto mesh = geom.get_component<MeshComponent>();
        if (!mesh) {
            err = "input geometry has no mesh component";
            return false;
        }

        const auto& vertices = mesh->get_vertices();
        const size_t n = vertices.size();
        const int res = static_cast<int>(std::llround(std::sqrt(double(n))));
        if (res < 2 || static_cast<size_t>(res) * res != n) {
            err = "input mesh is not a square grid (" + std::to_string(n) +
                  " vertices)";
            return false;
        }
        const auto& counts = mesh->get_face_vertex_counts();
        const auto& indices = mesh->get_face_vertex_indices();
        const size_t expected_faces =
            2ull * static_cast<size_t>(res - 1) * static_cast<size_t>(res - 1);
        if (counts.size() != expected_faces ||
            indices.size() != 3ull * expected_faces) {
            err = "input mesh topology does not match a " +
                  std::to_string(res) + "x" + std::to_string(res) +
                  " heightfield grid";
            return false;
        }

        // World extent from the vertex positions (the generator centers the
        // grid on the origin, so the X span is the full world size).
        float x_min = vertices[0].x, x_max = vertices[0].x;
        for (const auto& v : vertices) {
            x_min = std::min(x_min, v.x);
            x_max = std::max(x_max, v.x);
        }
        const float world_size = std::max(x_max - x_min, 1e-6f);

        out.alloc(res, world_size);
        for (size_t i = 0; i < n; ++i)
            out.height[i] = vertices[i].y;

        const auto& water = mesh->get_vertex_scalar_quantity(Q_WATER);
        if (water.size() == n) {
            out.water.assign(water.begin(), water.end());
            out.has_water = true;
        }
        const auto& sediment = mesh->get_vertex_scalar_quantity(Q_SEDIMENT);
        if (sediment.size() == n) {
            out.sediment.assign(sediment.begin(), sediment.end());
            out.has_sediment = true;
        }
        const auto& wear = mesh->get_vertex_scalar_quantity(Q_WEAR);
        if (wear.size() == n) {
            out.wear.assign(wear.begin(), wear.end());
            out.has_wear = true;
        }
        return true;
    }

    // Reconstruct a dense Heightfield from an ADAPTIVE mesh (the output of
    // terrain_adaptive_mesh) by RASTERIZING its triangles into the field
    // grid: every grid point covered by a triangle takes the exact
    // barycentric interpolation of that triangle's vertex values (height
    // and solver layers alike). The mesh vertices sit on the true surface,
    // so this invents no data. The previous scatter + onion-peel dilation
    // froze each uncovered cell after a single one-shot neighbor average,
    // and on smooth curved surfaces those frozen values undershoot by a
    // lattice-locked amount -- the quadric-surface test (2026-09-08) measured
    // a reconstructed gradient of +-6.3 against a true +-0.8, and the bake's
    // normal map rendered the mesh topology instead of the terrain.
    // Residual grid points (fp-rounding hairline gaps on shared edges) are
    // closed by neighbor averaging over NaN cells only.
    inline bool heightfield_from_adaptive_mesh(
        Geometry& geom,
        TerrainGen::Heightfield& out,
        std::string& err)
    {
        auto mesh = geom.get_component<MeshComponent>();
        if (!mesh) {
            err = "input geometry has no mesh component";
            return false;
        }
        const auto& vertices = mesh->get_vertices();
        const auto& uvs = mesh->get_texcoords_array();
        const auto& counts = mesh->get_face_vertex_counts();
        const auto& indices = mesh->get_face_vertex_indices();
        const size_t n = vertices.size();
        if (n < 4 || uvs.size() != n) {
            err = "adaptive mesh lacks per-vertex uv";
            return false;
        }
        if (counts.size() != indices.size() / 3) {
            err = "mesh face counts and indices disagree";
            return false;
        }
        for (int c : counts) {
            if (c != 3) {
                err = "adaptive mesh is not triangulated";
                return false;
            }
        }

        float x_min = vertices[0].x, x_max = vertices[0].x;
        for (const auto& v : vertices) {
            x_min = std::min(x_min, v.x);
            x_max = std::max(x_max, v.x);
        }
        const float world = std::max(x_max - x_min, 1e-6f);
        // Dense-equivalent heuristic: an adaptive mesh of a res-res field
        // carries roughly (res/2)^2 vertices. Cap 4096: the rasterized
        // field must stay ahead of 16k texture bakes (content above the
        // field resolution would just be bilinear).
        const int res =
            std::clamp(int(std::sqrt(double(n)) * 2.0) & ~1, 256, 4096);

        out.alloc(res, world);
        const size_t cells = static_cast<size_t>(res) * res;

        constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
        constexpr float kEdgeEps = 1e-5f;
        const float max_index = static_cast<float>(res - 1);

        // Rasterize one scalar field: grid point (gx, gy) sits at uv
        // (gx/max_index, gy/max_index), matching mesh_from_heightfield's
        // vertex-aligned convention.
        const auto rasterize = [&](const auto& value_at,
                                   std::vector<float>& field) {
            for (size_t f = 0; f < counts.size(); ++f) {
                const int i0 = indices[3 * f + 0];
                const int i1 = indices[3 * f + 1];
                const int i2 = indices[3 * f + 2];
                const glm::vec2& a = uvs[i0];
                const glm::vec2& b = uvs[i1];
                const glm::vec2& c = uvs[i2];
                const float den =
                    (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
                if (std::abs(den) < 1e-12f)
                    continue;
                const int x0 = std::max(
                    0, int(std::min({ a.x, b.x, c.x }) * max_index) - 1);
                const int x1 = std::min(
                    res - 1, int(std::max({ a.x, b.x, c.x }) * max_index) + 1);
                const int y0 = std::max(
                    0, int(std::min({ a.y, b.y, c.y }) * max_index) - 1);
                const int y1 = std::min(
                    res - 1, int(std::max({ a.y, b.y, c.y }) * max_index) + 1);
                for (int gy = y0; gy <= y1; ++gy) {
                    for (int gx = x0; gx <= x1; ++gx) {
                        const float px = static_cast<float>(gx) / max_index;
                        const float py = static_cast<float>(gy) / max_index;
                        const float w0 = ((b.y - c.y) * (px - c.x) +
                                          (c.x - b.x) * (py - c.y)) /
                                         den;
                        const float w1 = ((c.y - a.y) * (px - c.x) +
                                          (a.x - c.x) * (py - c.y)) /
                                         den;
                        const float w2 = 1.0f - w0 - w1;
                        if (w0 >= -kEdgeEps && w1 >= -kEdgeEps &&
                            w2 >= -kEdgeEps) {
                            field[size_t(gy) * res + gx] = w0 * value_at(i0) +
                                                           w1 * value_at(i1) +
                                                           w2 * value_at(i2);
                        }
                    }
                }
            }
        };

        // Close fp-rounding hairline gaps: neighbor-average NaN cells only
        // (in contrast to the old full-field dilation, these are isolated
        // single cells, so the one-shot fill cannot bias the surface).
        const auto close_gaps = [&](std::vector<float>& field) {
            std::vector<int> todo;
            for (size_t i = 0; i < cells; ++i)
                if (std::isnan(field[i]))
                    todo.push_back(int(i));
            std::vector<int> pending;
            for (int round = 0; round < 256 && !todo.empty(); ++round) {
                pending.clear();
                for (const int i : todo) {
                    const int x = i % res;
                    const int y = i / res;
                    float acc = 0.0f;
                    int cnt = 0;
                    for (const auto& [dx, dy] : { std::pair{ -1, 0 },
                                                  { 1, 0 },
                                                  { 0, -1 },
                                                  { 0, 1 } }) {
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= res || ny >= res)
                            continue;
                        const float v = field[size_t(ny) * res + nx];
                        if (!std::isnan(v)) {
                            acc += v;
                            ++cnt;
                        }
                    }
                    if (cnt > 0)
                        field[size_t(i)] = acc / static_cast<float>(cnt);
                    else
                        pending.push_back(i);
                }
                todo.swap(pending);
            }
            for (float& v : field)
                if (std::isnan(v))
                    v = 0.0f;
        };

        rasterize([&](int i) { return vertices[i].y; }, out.height);
        close_gaps(out.height);

        const auto gather_quantity =
            [&](const char* name, std::vector<float>& field, bool& flag) {
                const auto& q = mesh->get_vertex_scalar_quantity(name);
                if (q.size() != n)
                    return;
                field.assign(cells, kNaN);
                rasterize([&](int i) { return q[i]; }, field);
                close_gaps(field);
                flag = true;
            };
        gather_quantity(Q_WATER, out.water, out.has_water);
        gather_quantity(Q_SEDIMENT, out.sediment, out.has_sediment);
        gather_quantity(Q_WEAR, out.wear, out.has_wear);
        return true;
    }

    // Dense grid first (erode/bake fast path), adaptive scatter otherwise.
    inline bool heightfield_from_mesh_auto(
        Geometry& geom,
        TerrainGen::Heightfield& out,
        std::string& err)
    {
        if (heightfield_from_mesh(geom, out, err))
            return true;
        return heightfield_from_adaptive_mesh(geom, out, err);
    }

    // Pack a Heightfield into a fresh render-ready grid Geometry: vertices on
    // Y, analytic normals, world XY texcoords, a min/max-normalized height ramp
    // as display color, and the solver layers as vertex scalar quantities.
    inline Geometry mesh_from_heightfield(const TerrainGen::Heightfield& hf)
    {
        Geometry geom = Geometry::CreateMesh();
        auto mesh = geom.get_component<MeshComponent>();

        const int res = hf.res;
        const size_t n = hf.height.size();
        const float half = hf.world_size * 0.5f;
        const float step = hf.world_size / static_cast<float>(res - 1);

        std::vector<glm::vec3> vertices;
        std::vector<glm::vec2> uvs;
        vertices.reserve(n);
        uvs.reserve(n);
        for (int y = 0; y < res; ++y) {
            for (int x = 0; x < res; ++x) {
                vertices.emplace_back(
                    -half + static_cast<float>(x) * step,
                    hf.at(x, y),
                    -half + static_cast<float>(y) * step);
                uvs.emplace_back(
                    static_cast<float>(x) / static_cast<float>(res - 1),
                    static_cast<float>(y) / static_cast<float>(res - 1));
            }
        }

        std::vector<int> counts;
        std::vector<int> indices;
        counts.reserve(2ull * (res - 1) * (res - 1));
        indices.reserve(6ull * (res - 1) * (res - 1));
        for (int y = 0; y < res - 1; ++y) {
            for (int x = 0; x < res - 1; ++x) {
                const int i0 = y * res + x;
                const int i1 = y * res + (x + 1);
                const int i2 = (y + 1) * res + x;
                const int i3 = (y + 1) * res + (x + 1);
                counts.push_back(3);
                indices.insert(indices.end(), { i0, i2, i1 });
                counts.push_back(3);
                indices.insert(indices.end(), { i1, i2, i3 });
            }
        }

        std::vector<glm::vec3> normals;
        normals.reserve(n);
        for (int y = 0; y < res; ++y)
            for (int x = 0; x < res; ++x)
                normals.push_back(hf.normal(x, y));

        mesh->set_vertices(vertices);
        mesh->set_face_vertex_counts(counts);
        mesh->set_face_vertex_indices(indices);
        mesh->set_normals(normals);
        mesh->set_texcoords_array(uvs);

        // Display color: height ramp normalized by the ACTUAL range (the legacy
        // node normalized absolute heights by 1 and saturated everything
        // white).
        const float h_min = hf.min_height();
        const float h_max = std::max(hf.max_height(), h_min + 1e-6f);
        std::vector<glm::vec3> colors;
        colors.reserve(n);
        for (const auto& v : vertices) {
            const float t = (v.y - h_min) / (h_max - h_min);
            // Dark lowlands -> light peaks, slight blue-green tint in valleys.
            colors.emplace_back(
                0.15f + 0.85f * t, 0.25f + 0.75f * t, 0.35f + 0.65f * t);
        }
        mesh->set_display_color(colors);

        std::vector<float> zero(n, 0.0f);
        const std::vector<float>& water = hf.has_water ? hf.water : zero;
        const std::vector<float>& sediment =
            hf.has_sediment ? hf.sediment : zero;
        const std::vector<float>& wear = hf.has_wear ? hf.wear : zero;
        mesh->add_vertex_scalar_quantity(Q_WATER, water);
        mesh->add_vertex_scalar_quantity(Q_SEDIMENT, sediment);
        mesh->add_vertex_scalar_quantity(Q_WEAR, wear);

        return geom;
    }

}  // namespace terrain_carry
}  // namespace Ruzino
