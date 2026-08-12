#pragma once

#include "api.h"
#include "nvrhi/nvrhi.h"
#include "pxr/base/gf/vec3f.h"

#include <cstdint>
#include <vector>

// PackedNode / LeafNode / InternalNode — shared host/device layout.
// Included outside any namespace (the slang file defines its own).
#include "../../nodes/shaders/Scene/Lights/LightBVHTypes.slang"

RUZINO_NAMESPACE_OPEN_SCOPE

/// Per-triangle data needed for BVH construction (computed from the emissive
/// triangle table built by EmissiveMeshRegistry).
struct TriangleSortData {
    float3 boundsMin;       ///< World-space AABB min.
    float3 boundsMax;       ///< World-space AABB max.
    float3 center;          ///< Triangle centroid (for sorting/binning).
    float3 coneDirection;   ///< Emission normal direction.
    float  cosConeAngle;    ///< Cosine of cone half-angle (1.0 for flat triangle).
    float  flux;            ///< Pre-computed radiant flux.
    uint32_t triangleIndex; ///< Global index into the emissive triangle array.
};

/// Build options for the LightBVH (Conty-Kulla SAOH defaults).
struct LightBVHBuildOptions {
    uint32_t maxTriangleCountPerLeaf = 10; ///< Max triangles per leaf (≤16, PackedNode uses 4 bits).
    uint32_t binCount = 16;                ///< Number of bins per axis for SAOH.
    bool usePreintegration = true;         ///< Cost includes flux weighting.
    bool useLightingCones = true;          ///< Cost includes orientation cone factor.
    bool useLeafCreationCost = true;       ///< Compare split cost against leaf cost.
    bool createLeavesASAP = true;          ///< Build leaf as soon as ≤ maxTriangleCountPerLeaf.
};

/// Builds a LightBVH from emissive triangle data. Pure CPU, no GPU deps.
/// Output: packed nodes (32B each), triangle indices, and per-triangle
/// traversal bitmasks (for MIS pdf evaluation).
class HD_RUZINO_API LightBVHBuilder {
   public:
    LightBVHBuilder() = default;

    /// Build the BVH. Returns true on success.
    /// Inputs: triangle data (positions/normals/flux), build options.
    /// Outputs: nodes, triangleIndices, triangleBitmasks.
    bool build(
        const std::vector<TriangleSortData>& triangles,
        const LightBVHBuildOptions& options,
        std::vector<PackedNode>& outNodes,
        std::vector<uint32_t>& outTriangleIndices,
        std::vector<uint2>& outTriangleBitmasks);

   private:
    struct Range {
        uint32_t begin;
        uint32_t end;
        uint32_t length() const { return end - begin; }
    };

    struct SplitResult {
        uint32_t axis;         ///< Split axis (0=x, 1=y, 2=z).
        uint32_t triangleIndex; ///< Partition point: [begin, triangleIndex) left, [triangleIndex, end) right.
        bool valid = false;
    };

    struct Bin {
        float3 boundsMin = float3(1e30f);
        float3 boundsMax = float3(-1e30f);
        float3 coneDirection = float3(0.0f);
        float  cosConeAngle = 1.0f;
        float  flux = 0.0f;
        uint32_t triangleCount = 0;
    };

    struct BuildingData {
        std::vector<TriangleSortData> triangles;
        std::vector<PackedNode> nodes;
        std::vector<uint32_t> triangleIndices;
        std::vector<uint2> triangleBitmasks;
        LightBVHBuildOptions options;
    };

    /// Recursive build. Returns node index in BuildingData::nodes.
    uint32_t buildInternal(
        BuildingData& data,
        uint64_t bitmask,
        uint32_t depth,
        Range range);

    /// Binned SAOH split heuristic (Conty-Kulla 2018).
    SplitResult computeSplitSAOH(
        const BuildingData& data,
        Range range,
        const float3& nodeBoundsMin,
        const float3& nodeBoundsMax);

    /// Compute the lighting cone for a range of triangles (leaf cone).
    void computeLightingCone(
        const BuildingData& data,
        Range range,
        float3& outConeDirection,
        float& outCosConeAngle);

    /// Merge two cones into a conservative union.
    void coneUnion(
        const float3& dirA, float cosA,
        const float3& dirB, float cosB,
        float3& outDir, float& outCos);

    /// Post-order traversal: compute cones for internal nodes from children.
    void computeInternalCones(BuildingData& data, uint32_t nodeIndex);
};

RUZINO_NAMESPACE_CLOSE_SCOPE
