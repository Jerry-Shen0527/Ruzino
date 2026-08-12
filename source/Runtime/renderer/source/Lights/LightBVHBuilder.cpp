#include "LightBVHBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

RUZINO_NAMESPACE_OPEN_SCOPE

namespace {
constexpr uint32_t kMaxBVHDepth = 64;
constexpr uint32_t kMaxLeafTriangleCount = 16;       // 4 bits in PackedNode
constexpr uint32_t kMaxLeafTriangleOffset = 1u << 27; // 27 bits in PackedNode
constexpr float kInvalidConeAngle = -1.0f;

/// IEEE 754 float16-to-float32 conversion (bit manipulation).
float f16tof32cpu(uint16_t h) {
    uint32_t sign = (h >> 15) & 1;
    uint32_t exponent = (h >> 10) & 0x1f;
    uint32_t mantissa = h & 0x3ff;
    uint32_t result;
    if (exponent == 0) {
        if (mantissa == 0) {
            result = sign << 31;
        } else {
            // Subnormal: normalize
            int e = -1;
            uint32_t m = mantissa;
            do { e++; m <<= 1; } while (!(m & 0x400));
            exponent = 127 - 15 - e;
            mantissa = (m & 0x3ff) << 13;
            result = (sign << 31) | (exponent << 23) | mantissa;
        }
    } else if (exponent == 0x1f) {
        result = (sign << 31) | (0xff << 23) | (mantissa << 13); // Inf/NaN
    } else {
        exponent += 127 - 15;
        result = (sign << 31) | (exponent << 23) | (mantissa << 13);
    }
    return *(const float*)&result;
}

/// IEEE 754 float32-to-float16 conversion (bit manipulation).
uint16_t f32tof16cpu(float f) {
    uint32_t x;
    std::memcpy(&x, &f, sizeof(float));
    uint32_t sign = (x >> 16) & 0x8000;
    int32_t exponent = ((x >> 23) & 0xff) - 127 + 15;
    uint32_t mantissa = x & 0x7fffff;
    if (exponent <= 0) {
        if (exponent < -10) return sign;
        mantissa |= 0x800000;
        uint32_t shift = 14 - exponent;
        uint16_t m = mantissa >> shift;
        return sign | m;
    }
    if (exponent == 0xff - (127 - 15)) {
        if (mantissa) return sign | 0x7e00;  // NaN
        return sign | 0x7c00;                // Inf
    }
    if (exponent > 30) return sign | 0x7c00;  // overflow -> Inf
    return sign | (uint16_t)(exponent << 10) | (mantissa >> 13);
}

float3 minFloat3(const float3& a, const float3& b) {
    return float3(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z));
}
float3 maxFloat3(const float3& a, const float3& b) {
    return float3(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z));
}
float3 crossF3(const float3& a, const float3& b) {
    return float3(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x);
}

/// SAOH orientation cost (Conty & Kulla 2018, Eq. 1). Assumes planar diffuse
/// emitter (emission angle = pi/2), so theta_e = pi/2 and the cost simplifies.
float computeOrientationCost(float cosTheta) {
    constexpr float kPi = 3.14159265358979323846f;
    float theta_s = std::acos(std::max(-1.0f, std::min(1.0f, cosTheta)));
    float theta_e = kPi / 2.0f; // half emission angle for diffuse
    float theta = std::min(theta_s + theta_e, kPi);
    return theta * theta - theta_e * theta_e; // ~ pi..4pi
}

/// Compute cosConeAngle for merging a cone (dirA, cosA) with a direction dirB.
/// Returns the new cosConeAngle that includes dirB.
float computeCosConeAngle(const float3& dirA, float cosA, const float3& dirB) {
    float cosDiff = std::max(-1.0f, std::min(1.0f, dot(dirA, dirB)));
    float cosTotalTheta = 0.0f;

    // If dirB is already inside cone A, no change.
    if (cosDiff >= cosA) {
        return cosA;
    }

    // Rotate cone A to include dirB.
    float sinDiff = std::sqrt(std::max(0.0f, 1.0f - cosDiff * cosDiff));
    float sinA = std::sqrt(std::max(0.0f, 1.0f - cosA * cosA));
    float sinTotalTheta = sinA * cosDiff + cosA * sinDiff;
    if (sinTotalTheta > 0.0f) {
        cosTotalTheta = cosA * cosDiff - sinA * sinDiff;
        return std::min(cosA, cosTotalTheta);
    }
    return kInvalidConeAngle; // entire sphere
}

/// Pack a node into the 32-byte PackedNode format (matching LightBVHTypes.slang).
/// For internal nodes: data[0].x = rightChildIdx (MSB=0).
/// For leaf nodes: data[0].x = (1<<31) | (count<<27) | offset.
PackedNode packInternalNode(
    const float3& origin, const float3& extent,
    float flux, uint32_t rightChildIdx,
    const float3& coneDirection, float cosConeAngle)
{
    PackedNode node;
    node.data[0].x = rightChildIdx; // MSB=0 → internal

    // Origin (3 × float32).
    node.data[0].y = *(const uint32_t*)&origin.x;
    node.data[0].z = *(const uint32_t*)&origin.y;
    node.data[0].w = *(const uint32_t*)&origin.z;

    // Extent.x/y packed as f16.
    uint32_t ex16 = f32tof16cpu(extent.x);
    uint32_t ey16 = f32tof16cpu(extent.y);
    node.data[1].x = ex16 | (ey16 << 16);

    // Extent.z packed as f16 + cosConeAngle quantized to 16 bit.
    uint32_t ez16 = f32tof16cpu(extent.z);
    uint32_t packedAngle = (uint32_t)((cosConeAngle + 1.0f) * 32767.0f);
    node.data[1].y = ez16 | (packedAngle << 16);

    // Cone direction (octahedral 2×16 snorm).
    node.data[1].z = encodeNormal2x16(coneDirection);

    // Flux (float32).
    node.data[1].w = *(const uint32_t*)&flux;

    return node;
}

PackedNode packLeafNode(
    const float3& origin, const float3& extent,
    float flux, uint32_t triangleOffset, uint32_t triangleCount,
    const float3& coneDirection, float cosConeAngle)
{
    PackedNode node;
    node.data[0].x = (1u << 31) | (triangleCount << 27) | triangleOffset;

    node.data[0].y = *(const uint32_t*)&origin.x;
    node.data[0].z = *(const uint32_t*)&origin.y;
    node.data[0].w = *(const uint32_t*)&origin.z;

    uint32_t ex16 = f32tof16cpu(extent.x);
    uint32_t ey16 = f32tof16cpu(extent.y);
    node.data[1].x = ex16 | (ey16 << 16);

    uint32_t ez16 = f32tof16cpu(extent.z);
    uint32_t packedAngle = (uint32_t)((cosConeAngle + 1.0f) * 32767.0f);
    node.data[1].y = ez16 | (packedAngle << 16);

    node.data[1].z = encodeNormal2x16(coneDirection);
    node.data[1].w = *(const uint32_t*)&flux;

    return node;
}

/// Compute AABB surface area.
float surfaceArea(const float3& bmin, const float3& bmax) {
    float3 extent = bmax - bmin;
    return 2.0f * (extent.x * extent.y + extent.y * extent.z + extent.x * extent.z);
}

/// Compute AABB volume.
float volume(const float3& bmin, const float3& bmax) {
    float3 extent = bmax - bmin;
    return extent.x * extent.y * extent.z;
}

}  // namespace

bool LightBVHBuilder::build(
    const std::vector<TriangleSortData>& triangles,
    const LightBVHBuildOptions& options,
    std::vector<PackedNode>& outNodes,
    std::vector<uint32_t>& outTriangleIndices,
    std::vector<uint2>& outTriangleBitmasks)
{
    if (triangles.empty()) return false;
    if (options.maxTriangleCountPerLeaf > kMaxLeafTriangleCount) return false;

    BuildingData data;
    data.triangles = triangles;
    data.options = options;
    data.triangleBitmasks.resize(triangles.size(), uint2(0, 0));

    // Recursively build from root (bitmask=0, depth=0, full range).
    buildInternal(data, 0, 0, {0, (uint32_t)triangles.size()});

    // Post-process: compute internal node cones (bottom-up).
    if (options.useLightingCones && !data.nodes.empty()) {
        computeInternalCones(data, 0);
    }

    outNodes = std::move(data.nodes);
    outTriangleIndices = std::move(data.triangleIndices);
    outTriangleBitmasks = std::move(data.triangleBitmasks);
    return true;
}

uint32_t LightBVHBuilder::buildInternal(
    BuildingData& data,
    uint64_t bitmask,
    uint32_t depth,
    Range range)
{
    const auto& tris = data.triangles;
    const auto& opts = data.options;

    // Compute node AABB + flux.
    float3 boundsMin(1e30f), boundsMax(-1e30f);
    float nodeFlux = 0.0f;
    for (uint32_t i = range.begin; i < range.end; i++) {
        boundsMin = minFloat3(boundsMin, tris[i].boundsMin);
        boundsMax = maxFloat3(boundsMax, tris[i].boundsMax);
        nodeFlux += tris[i].flux;
    }
    float3 origin = (boundsMin + boundsMax) * 0.5f;
    float3 extent = (boundsMax - boundsMin) * 0.5f;

    // Decide whether to split.
    uint32_t leafThreshold = opts.createLeavesASAP ? opts.maxTriangleCountPerLeaf : 1;
    bool trySplit = range.length() > leafThreshold && depth < kMaxBVHDepth;

    SplitResult split;
    if (trySplit) {
        split = computeSplitSAOH(data, range, boundsMin, boundsMax);
    }

    if (!trySplit || !split.valid) {
        // Create leaf node.
        float3 coneDir(0.0f);
        float cosCone = 1.0f;
        if (opts.useLightingCones) {
            computeLightingCone(data, range, coneDir, cosCone);
        }

        uint32_t triOffset = (uint32_t)data.triangleIndices.size();
        uint32_t triCount = range.length();

        // Append triangle indices + bitmasks.
        for (uint32_t i = range.begin; i < range.end; i++) {
            uint32_t globalIdx = tris[i].triangleIndex;
            data.triangleIndices.push_back(globalIdx);
            data.triangleBitmasks[globalIdx] =
                uint2((uint32_t)(bitmask & 0xFFFFFFFF), (uint32_t)(bitmask >> 32));
        }

        uint32_t nodeIdx = (uint32_t)data.nodes.size();
        data.nodes.push_back(packLeafNode(
            origin, extent, nodeFlux, triOffset, triCount, coneDir, cosCone));
        return nodeIdx;
    }

    // Partition triangles at the split point using nth_element by centroid[axis].
    std::nth_element(
        data.triangles.begin() + range.begin,
        data.triangles.begin() + split.triangleIndex,
        data.triangles.begin() + range.end,
        [&](const TriangleSortData& a, const TriangleSortData& b) {
            return (&a.center.x)[split.axis] < (&b.center.x)[split.axis];
        });

    // Reserve slot for this internal node (will be filled after children).
    uint32_t nodeIdx = (uint32_t)data.nodes.size();
    data.nodes.push_back(PackedNode{}); // placeholder

    // Recurse left (bit 0 at this depth).
    uint32_t leftIdx = buildInternal(
        data, bitmask | (0ull << depth), depth + 1,
        {range.begin, split.triangleIndex});

    // Left child must be nodeIdx+1 (preorder layout invariant).
    // (leftIdx is not stored in the internal node; the GPU uses nodeIndex+1.)

    // Recurse right (bit 1 at this depth).
    uint32_t rightIdx = buildInternal(
        data, bitmask | (1ull << depth), depth + 1,
        {split.triangleIndex, range.end});

    // Now pack the internal node. Cone will be filled by post-processing.
    data.nodes[nodeIdx] = packInternalNode(
        origin, extent, nodeFlux, rightIdx,
        float3(0.0f, 1.0f, 0.0f), 1.0f); // placeholder cone; post-process fixes it

    return nodeIdx;
}

LightBVHBuilder::SplitResult LightBVHBuilder::computeSplitSAOH(
    const BuildingData& data,
    Range range,
    const float3& nodeBoundsMin,
    const float3& nodeBoundsMax)
{
    const auto& tris = data.triangles;
    const auto& opts = data.options;
    const uint32_t binCount = opts.binCount;

    SplitResult bestSplit;
    float bestCost = 1e30f;

    float3 extent = nodeBoundsMax - nodeBoundsMin;

    // Try all 3 axes.
    for (uint32_t axis = 0; axis < 3; axis++) {
        float axisExtent = (&extent.x)[axis];
        if (axisExtent < 1e-6f) continue;

        float invExtent = 1.0f / axisExtent;

        // Initialize bins.
        std::vector<Bin> bins(binCount);
        for (auto& b : bins) {
            b.boundsMin = float3(1e30f);
            b.boundsMax = float3(-1e30f);
            b.cosConeAngle = 1.0f;
        }

        // Bin triangles by centroid on this axis.
        for (uint32_t i = range.begin; i < range.end; i++) {
            float center = (&tris[i].center.x)[axis];
            uint32_t binIdx = std::min(
                binCount - 1,
                (uint32_t)((center - (&nodeBoundsMin.x)[axis]) * invExtent * binCount));
            Bin& b = bins[binIdx];
            b.boundsMin = minFloat3(b.boundsMin, tris[i].boundsMin);
            b.boundsMax = maxFloat3(b.boundsMax, tris[i].boundsMax);
            b.coneDirection = b.coneDirection + tris[i].coneDirection;
            b.cosConeAngle = computeCosConeAngle(
                tris[i].coneDirection, b.cosConeAngle, b.coneDirection);
            // Simpler: just expand cone
            float cosDiff = std::max(-1.0f, std::min(1.0f,
                dot(tris[i].coneDirection, normalize(b.coneDirection))));
            b.flux += tris[i].flux;
            b.triangleCount++;
        }

        // Sweep from left: accumulate prefix AABBs + flux.
        std::vector<float3> leftMin(binCount - 1), leftMax(binCount - 1);
        std::vector<float> leftFlux(binCount - 1);
        std::vector<uint32_t> leftCount(binCount - 1);
        float3 curMin(1e30f), curMax(-1e30f);
        float curFlux = 0.0f;
        uint32_t curCount = 0;
        for (uint32_t i = 0; i < binCount - 1; i++) {
            curMin = minFloat3(curMin, bins[i].boundsMin);
            curMax = maxFloat3(curMax, bins[i].boundsMax);
            curFlux += bins[i].flux;
            curCount += bins[i].triangleCount;
            leftMin[i] = curMin;
            leftMax[i] = curMax;
            leftFlux[i] = curFlux;
            leftCount[i] = curCount;
        }

        // Sweep from right + evaluate cost at each split point.
        curMin = float3(1e30f);
        curMax = float3(-1e30f);
        curFlux = 0.0f;
        curCount = 0;
        for (int i = binCount - 1; i >= 1; i--) {
            curMin = minFloat3(curMin, bins[i].boundsMin);
            curMax = maxFloat3(curMax, bins[i].boundsMax);
            curFlux += bins[i].flux;
            curCount += bins[i].triangleCount;

            int li = i - 1; // left prefix index
            if (leftCount[li] == 0 || curCount == 0) continue;

            float leftArea = surfaceArea(leftMin[li], leftMax[li]);
            float rightArea = surfaceArea(curMin, curMax);

            float leftCost = leftArea;
            float rightCost = rightArea;
            if (opts.usePreintegration) {
                leftCost *= leftFlux[li];
                rightCost *= curFlux;
            }

            float totalCost = leftCost + rightCost;

            // Penalize splits along non-largest axes.
            float axisPenalty = axisExtent / std::max(extent.x, std::max(extent.y, extent.z));
            totalCost *= axisPenalty;

            // Leaf cost comparison.
            if (opts.useLeafCreationCost) {
                float leafCost = surfaceArea(nodeBoundsMin, nodeBoundsMax) *
                    (opts.usePreintegration ?
                        (leftFlux[li] + curFlux) : (float)(leftCount[li] + curCount));
                if (totalCost >= leafCost) continue;
            }

            if (totalCost < bestCost) {
                bestCost = totalCost;
                bestSplit.axis = axis;
                bestSplit.triangleIndex = range.begin + leftCount[li];
                bestSplit.valid = true;
            }
        }
    }

    // Equal split fallback if no valid SAOH split found.
    if (!bestSplit.valid) {
        uint32_t axis = 0;
        if (extent.y > extent.x) axis = 1;
        if (extent.z > (&extent.x)[axis]) axis = 2;
        bestSplit.axis = axis;
        bestSplit.triangleIndex = range.begin + range.length() / 2;
        bestSplit.valid = true;
    }

    return bestSplit;
}

void LightBVHBuilder::computeLightingCone(
    const BuildingData& data,
    Range range,
    float3& outConeDirection,
    float& outCosConeAngle)
{
    const auto& tris = data.triangles;

    float3 dirSum(0.0f);
    for (uint32_t i = range.begin; i < range.end; i++) {
        dirSum = dirSum + tris[i].coneDirection;
    }

    float len = std::sqrt(dot(dirSum, dirSum));
    if (len < 1e-6f) {
        outConeDirection = float3(0.0f, 1.0f, 0.0f);
        outCosConeAngle = kInvalidConeAngle;
        return;
    }

    outConeDirection = dirSum * (1.0f / len);
    outCosConeAngle = 1.0f;

    for (uint32_t i = range.begin; i < range.end; i++) {
        outCosConeAngle = computeCosConeAngle(
            outConeDirection, outCosConeAngle, tris[i].coneDirection);
    }
}

void LightBVHBuilder::coneUnion(
    const float3& dirA, float cosA,
    const float3& dirB, float cosB,
    float3& outDir, float& outCos)
{
    if (cosA == kInvalidConeAngle || cosB == kInvalidConeAngle) {
        outDir = float3(0.0f, 1.0f, 0.0f);
        outCos = kInvalidConeAngle;
        return;
    }

    float3 dirSum = dirA + dirB;
    float len = std::sqrt(dot(dirSum, dirSum));
    if (len < 1e-6f) {
        outDir = dirA;
    } else {
        outDir = dirSum * (1.0f / len);
    }

    float cos1 = computeCosConeAngle(outDir, cosA, dirB);
    float cos2 = computeCosConeAngle(outDir, cosB, dirA);
    outCos = std::min(cos1, cos2);
}

void LightBVHBuilder::computeInternalCones(BuildingData& data, uint32_t nodeIndex)
{
    PackedNode& node = data.nodes[nodeIndex];
    bool isLeaf = (node.data[0].x >> 31) != 0;
    if (isLeaf) return;

    uint32_t rightChildIdx = node.data[0].x;
    uint32_t leftChildIdx = nodeIndex + 1;

    // Recurse into children first (post-order).
    computeInternalCones(data, leftChildIdx);
    computeInternalCones(data, rightChildIdx);

    // Decode child cones.
    PackedNode& leftChild = data.nodes[leftChildIdx];
    PackedNode& rightChild = data.nodes[rightChildIdx];

    // Decode cone direction + angle from packed format.
    auto decodeCone = [](const PackedNode& n, float3& dir, float& cosAngle) {
        dir = decodeNormal2x16(n.data[1].z);
        uint32_t packedAngle = n.data[1].y >> 16;
        cosAngle = (float)packedAngle * (1.0f / 32767.0f) - 1.0f;
    };

    float3 leftDir, rightDir;
    float leftCos, rightCos;
    decodeCone(leftChild, leftDir, leftCos);
    decodeCone(rightChild, rightDir, rightCos);

    float3 unionDir;
    float unionCos;
    coneUnion(leftDir, leftCos, rightDir, rightCos, unionDir, unionCos);

    // Re-pack the internal node with the computed cone.
    // Preserve everything except cone direction + angle.
    float3 origin;
    origin.x = *(const float*)&node.data[0].y;
    origin.y = *(const float*)&node.data[0].z;
    origin.z = *(const float*)&node.data[0].w;

    uint32_t ex16 = node.data[1].x & 0xFFFF;
    uint32_t ey16 = node.data[1].x >> 16;
    uint32_t ez16 = node.data[1].y & 0xFFFF;
    float3 extent;
    extent.x = f16tof32cpu(ex16);
    extent.y = f16tof32cpu(ey16);
    extent.z = f16tof32cpu(ez16);

    float flux = *(const float*)&node.data[1].w;

    node = packInternalNode(
        origin, extent, flux, rightChildIdx, unionDir, unionCos);
}

RUZINO_NAMESPACE_CLOSE_SCOPE
