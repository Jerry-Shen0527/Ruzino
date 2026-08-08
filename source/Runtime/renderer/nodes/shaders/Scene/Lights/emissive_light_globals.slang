/***************************************************************************
 # Emissive mesh light global buffers (Ruzino adaptation of Falcor LightCollection)
 #
 # Unlike Falcor's struct LightCollection (bound via parameter block / gScene),
 # Ruzino uses top-level global StructuredBuffers bound by name in
 # path_tracing.cpp (program_vars["name"] = buffer), matching the existing
 # lightBuffer / instanceDescBuffer / meshDescBuffer pattern.
 #
 # These buffers are populated CPU-side by LightCollection.cpp (build pass that
 # enumerates Hydra mesh instances, filters emissive materials, computes
 # world-space triangles + flux) and consumed GPU-side by the emissive NEE
 # sampler (sampleLights emissive branch) and the BSDF-hit emissive MIS path.
 **************************************************************************/
#pragma once
#include "utils/HostDeviceShared.slangh"

#ifndef HOST_CODE
import Scene.Lights.LightCollectionShared;
import Scene.Lights.MeshLightData;
import Scene.Lights.LightData;
import utils.Attributes;

/// Per-triangle geometry + texture data for emissive triangles.
/// Valid only when emissiveTriangleCount > 0.
StructuredBuffer<PackedEmissiveTriangle> emissiveTriangleData;

/// Per-triangle precomputed flux and average radiance.
StructuredBuffer<EmissiveFlux> emissiveFluxData;

/// Per-mesh data (instanceID / triangleOffset / triangleCount / materialID).
StructuredBuffer<MeshLightData> emissiveMeshData;

/// Per-geometry-instance offset into the emissive triangle array.
/// Indexed by GeometryInstanceID.index (== DXR InstanceID() here).
/// kInvalidIndex (0xffffffff) if this instance has no emissive triangles.
/// Used by ClosestHit to map (instanceID, primitiveIndex) -> emissive triangle
/// index for the BSDF-hit-emissive MIS path.
StructuredBuffer<uint> emissivePerMeshInstanceOffset;

/// Returns the emissive triangle index for a hit, or kInvalidIndex if the hit
/// instance/primitive is not emissive. This is the BSDF-hit-emissive reverse
/// lookup (Falcor's getTriangleIndex equivalent). The triangle count check is
/// the caller's responsibility (read ptConstants.emissiveTriangleCount).
static const uint kInvalidEmissiveIndex = 0xffffffff;

uint getEmissiveTriangleIndex(uint instanceID, uint primitiveIndex, uint triangleCount)
{
    if (triangleCount == 0) return kInvalidEmissiveIndex;
    uint offset = emissivePerMeshInstanceOffset[instanceID];
    return offset != kInvalidEmissiveIndex ? offset + primitiveIndex
                                            : kInvalidEmissiveIndex;
}

#endif // !HOST_CODE
