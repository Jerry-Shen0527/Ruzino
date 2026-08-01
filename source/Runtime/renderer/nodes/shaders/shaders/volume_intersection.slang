// Volume rendering helpers for the Wetbrush density slab.
//
// This file is a pure IMPORTABLE MODULE (no [shader] entry points, no
// references to RayPayload/ShadowPayload which live inside path_tracing.slang
// as file-level declarations and so cannot be imported). The 3 hit-group
// entry points (VolumeIntersection / VolumeClosestHit / VolumeShadowHit) are
// injected as raw source code by the wetbrush_render node, compiled into the
// same translation unit as path_tracing.slang so they can see RayPayload /
// ShadowPayload / the global buffers.
//
// What lives here: the VolumeHitAttributes struct, the slab ray-test, and the
// per-cell (density,r,g,b) field sampler, all of which only need the bindless
// volume descriptor + the per-cell Float4 buffer.
import Scene.SceneTypes;
import Scene.BindlessVertexBuffer;

// Hit attribute carrying the slab entry/exit ray parameters so the
// closest-hit shader knows the raymarch segment bounds.
struct VolumeHitAttributes {
    float tEnter;  // Ray param at slab AABB entry.
    float tExit;   // Ray param at slab AABB exit (clamped to RayTCurrent).
};

// Density above which a cell counts as "paint surface". Chosen well below the
// typical stroke density (~1-5/cell after the ×10 deposit): the surface
// must be a solid slab, not the 0.02-threshold contour of a diluted field
// (a field near the threshold gives ~30% ray hit rates → gray, speckled
// strokes).
static const float kDensitySurface = 0.002;

// RYB -> RGB (Gossett & Chen 2004). The sim stores the pigment as a
// red-yellow-blue vector (§3, Algorithm 2), so the blended pigment must be
// converted to display RGB before it can be emitted as radiance. Emitting
// RYB directly as RGB makes pure yellow (0,1,0) show as green and turns
// every mixed color (e.g. RYB green (0,0.5,0.5)) into a muddy approximation.
float3 ryb_to_rgb(float3 c)
{
    float rm = 1.0 - c.r, ym = 1.0 - c.g, bm = 1.0 - c.b;
    return rm*ym*bm*float3(1,1,1)   + c.r*ym*bm*float3(1,0,0) +
           rm*c.g*bm*float3(1,1,0)  + rm*ym*c.b*float3(0.163,0.373,0.6) +
           c.r*c.g*bm*float3(1,0.5,0) + c.r*ym*c.b*float3(0.5,0,0.5) +
           rm*c.g*c.b*float3(0,0.66,0.2) + c.r*c.g*c.b*float3(0.2,0.094,0.029);
}

// Fetch one Float4 voxel, returning (0,0,0,0) outside the grid.
float4 fetchVoxel(VolumeDesc vd, int ix, int iy, int iz)
{
    if (ix < 0 || iy < 0 || iz < 0 ||
        ix >= int(vd.gridResX) || iy >= int(vd.gridResY) || iz >= int(vd.gridResZ))
        return float4(0, 0, 0, 0);
    uint idx = uint((iz * int(vd.gridResY) + iy) * int(vd.gridResX) + ix);
    return asfloat(t_BindlessBuffers[vd.bindlessIndex].Load4(idx * 16));
}

// Fetch one oil-density voxel (paper §6 penetration distance). Returns -1
// when no oil buffer is bound (primvar fallback path) so the caller can fall
// back to density-driven penetration.
float fetchOilVoxel(VolumeDesc vd, int ix, int iy, int iz)
{
    if (vd.oilIndex < 0)
        return -1.0;
    if (ix < 0 || iy < 0 || iz < 0 ||
        ix >= int(vd.gridResX) || iy >= int(vd.gridResY) || iz >= int(vd.gridResZ))
        return 0.0;
    uint idx = uint((iz * int(vd.gridResY) + iy) * int(vd.gridResX) + ix);
    return asfloat(t_BindlessBuffers[vd.oilIndex].Load(idx * 4));
}

// Sample the per-cell (density, r, g, b) field at world-space position p.
// Trilinear interpolation across the 8 surrounding voxels — the paper §6
// raymarch samples "every one quarter of the density grid size"; sub-cell
// trilinear filtering is what turns the discrete voxel field into a smooth
// surface for the first-cross test. Nearest-voxel sampling left cell-aligned
// stairsteps on the paint surface (top-edge roughness ~65px at res 512).
float4 samplePaintField(VolumeDesc vd, float3 p)
{
    float3 rel = (p - vd.gridMin) / vd.cellSize - 0.5;
    int x0 = int(floor(rel.x));
    int y0 = int(floor(rel.y));
    int z0 = int(floor(rel.z));
    int x1 = x0 + 1, y1 = y0 + 1, z1 = z0 + 1;
    float sx = rel.x - float(x0);
    float sy = rel.y - float(y0);
    float sz = rel.z - float(z0);

    float4 c000 = fetchVoxel(vd,x0,y0,z0), c100 = fetchVoxel(vd,x1,y0,z0);
    float4 c010 = fetchVoxel(vd,x0,y1,z0), c110 = fetchVoxel(vd,x1,y1,z0);
    float4 c001 = fetchVoxel(vd,x0,y0,z1), c101 = fetchVoxel(vd,x1,y0,z1);
    float4 c011 = fetchVoxel(vd,x0,y1,z1), c111 = fetchVoxel(vd,x1,y1,z1);
    float4 c00 = lerp(c000,c100,sx), c10 = lerp(c010,c110,sx);
    float4 c01 = lerp(c001,c101,sx), c11 = lerp(c011,c111,sx);
    float4 c0 = lerp(c00,c10,sy), c1 = lerp(c01,c11,sy);
    return lerp(c0, c1, sz);
}

// Density channel only (paper §6 first-cross + penetration only read the
// oil/paint density to drive surface detection and the penetration distance).
float sampleDensity(VolumeDesc vd, float3 p)
{
    return samplePaintField(vd, p).x;
}

// Trilinearly sample the oil density at world position p (paper §6: "we use
// the oil density to calculate a penetration distance"). Returns -1 when no
// oil buffer is bound (primvar fallback path).
float sampleOilField(VolumeDesc vd, float3 p)
{
    float3 rel = (p - vd.gridMin) / vd.cellSize - 0.5;
    int x0 = int(floor(rel.x));
    int y0 = int(floor(rel.y));
    int z0 = int(floor(rel.z));
    int x1 = x0 + 1, y1 = y0 + 1, z1 = z0 + 1;
    float sx = rel.x - float(x0);
    float sy = rel.y - float(y0);
    float sz = rel.z - float(z0);

    float c000 = fetchOilVoxel(vd,x0,y0,z0), c100 = fetchOilVoxel(vd,x1,y0,z0);
    float c010 = fetchOilVoxel(vd,x0,y1,z0), c110 = fetchOilVoxel(vd,x1,y1,z0);
    float c001 = fetchOilVoxel(vd,x0,y0,z1), c101 = fetchOilVoxel(vd,x1,y0,z1);
    float c011 = fetchOilVoxel(vd,x0,y1,z1), c111 = fetchOilVoxel(vd,x1,y1,z1);
    // Any missing oil buffer (oilIndex < 0) propagates the -1 sentinel.
    float c00 = lerp(c000,c100,sx), c10 = lerp(c010,c110,sx);
    float c01 = lerp(c001,c101,sx), c11 = lerp(c011,c111,sx);
    float c0 = lerp(c00,c10,sy), c1 = lerp(c01,c11,sy);
    return lerp(c0, c1, sz);
}

// Paint surface normal from the density gradient (paper §6). The gradient
// points toward increasing density; the outward-facing surface normal is its
// negation. Central differences over 2 cells: paper's grid is pixel-level so
// a 1-cell stencil is sub-pixel and smooth, but at our coarser res (512) a
// 1-cell stencil rides cell-level density noise and makes the Lambertian
// shading (0.4+0.6*facing) flicker across the stroke. A 2-cell stencil
// averages over the noise. Paper does not specify the stencil radius.
float3 surfaceNormal(VolumeDesc vd, float3 p)
{
    float h = vd.cellSize * 2.0;
    float dx = sampleDensity(vd, p + float3(h, 0, 0)) - sampleDensity(vd, p - float3(h, 0, 0));
    float dy = sampleDensity(vd, p + float3(0, h, 0)) - sampleDensity(vd, p - float3(0, h, 0));
    float dz = sampleDensity(vd, p + float3(0, 0, h)) - sampleDensity(vd, p - float3(0, 0, h));
    float3 g = float3(dx, dy, dz);
    float len = length(g);
    if (len < 1e-8)
        return float3(0, 0, 1);  // degenerate: canvas up
    return -g / len;  // outward
}

// Slab AABB ray test in world space. Returns true on hit and writes the entry
// and exit ray parameters (within [RayTMin, RayTCurrent]).
bool intersectSlab(VolumeDesc vd, out float tEnter, out float tExit)
{
    float3 invD = 1.0 / WorldRayDirection();
    float3 t0 = (vd.boundsMin - WorldRayOrigin()) * invD;
    float3 t1 = (vd.boundsMax - WorldRayOrigin()) * invD;
    float3 tsmaller = min(t0, t1);
    float3 tbigger = max(t0, t1);
    tEnter = max(tsmaller.x, max(tsmaller.y, tsmaller.z));
    tExit = min(tbigger.x, min(tbigger.y, tbigger.z));
    tEnter = max(tEnter, RayTMin());
    tExit = min(tExit, RayTCurrent());
    return tEnter < tExit;
}
