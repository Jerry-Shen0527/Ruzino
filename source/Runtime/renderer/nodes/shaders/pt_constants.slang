struct PathTracingConstants {
    uint lightCount;
    uint domeLightCallableIndex;  // Only valid when USE_DOME_LIGHT_CALLABLE
    uint materialFetchCallableBaseIndex;
    uint materialOpacityCallableOffset;  // Offset to add to fetch index for
                                         // opacity callables
    uint emissiveTriangleCount;  // Total emissive triangles (0 = no emissive mesh)
    uint emissiveMeshCount;      // Number of emissive mesh lights
};

ConstantBuffer<PathTracingConstants> ptConstants;
RaytracingAccelerationStructure SceneBVH;
