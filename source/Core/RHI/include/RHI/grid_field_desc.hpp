#pragma once

// GridFieldDesc — the interchange contract for a generic GPU voxel field
// shared through SharedGPUBufferRegistry.
//
// A producer (e.g. a simulation node packing density+color) registers a
// buffer under a "grid_field/" key with a GridFieldDesc as the opaque
// metadata blob; the consumer (the grid volume rprim in hd_RUZINO) adopts
// the buffer zero-copy and renders it. Both sides include this one header
// instead of agreeing on a private POD layout out of band — which is exactly
// what the wetbrush paint field did before (PaintFieldMeta duplicated in
// node_brush_wb_commit.cpp and wetbrush_volume_impl.cpp).
//
// The CPU/USD mirror of the same data is the bake fallback path: primvars
// (paintField Float4 array + gridRes/cellSize/gridMin) authored on the
// UsdVolVolume prim. See render_wetbrush.py.

#include <cstdint>

#include "RHI/api.h"

RUZINO_NAMESPACE_OPEN_SCOPE

/// Voxel-payload layout of a grid field buffer.
enum class GridFieldFormat : uint32_t {
    Unknown = 0,
    /// float4 per voxel: (density, r, g, b). Density drives the surface
    /// threshold / penetration blend / AO; rgb is the per-voxel albedo.
    /// Matches VolumeKind::GridVolume in the renderer's SceneTypes.slang.
    DensityColorRGBA = 1,
};

/// Descriptor for one shared voxel grid field.
///
/// The registered buffer holds resX*resY*resZ voxel elements of the layout
/// named by `format`, indexed (z*resY + y)*resX + x. Voxel (0,0,0) sits at
/// the world-space corner `origin`; one voxel spans `spacing` per axis
/// (per-axis: a producer may use anisotropic cells, e.g. a thin Z stack).
struct GridFieldDesc {
    uint32_t resX = 0;
    uint32_t resY = 0;
    uint32_t resZ = 0;
    float spacingX = 0.0f;
    float spacingY = 0.0f;
    float spacingZ = 0.0f;
    float originX = 0.0f;
    float originY = 0.0f;
    float originZ = 0.0f;
    GridFieldFormat format = GridFieldFormat::Unknown;
    uint32_t reserved = 0;  // must be 0
};

/// Every zero-copy grid field is registered under a key with this prefix;
/// the renderer core polls the prefix (not individual key names) for
/// version bumps.
inline constexpr const char* kGridFieldKeyPrefix = "grid_field/";

/// The wetbrush paint field's canonical key (under the prefix). Rendering is
/// generic — this name only identifies the producer's buffer.
inline constexpr const char* kGridFieldPaintKey = "grid_field/paint";

RUZINO_NAMESPACE_CLOSE_SCOPE
