// GPU compute pass: build emissive triangle table from mesh bindless vertex
// buffers. This replaces the CPU-side per-triangle processing (transform,
// normal/area computation, fp16/octahedral encoding) with a parallel GPU
// dispatch — zero CPU→GPU vertex copies.
//
// One thread per emissive triangle. The CPU provides a "mesh work list"
// (EmissiveMeshWorkItem array) with pre-computed triangle offsets and
// emission radiance/textureIndex. Each thread reads its mesh's bindless
// vertexBuffer to get local-space positions, transforms to world space via
// the instance transform, computes normal + area, and packs into
// PackedEmissiveTriangle.

import Scene.SceneTypes;
import Scene.Lights.LightCollectionShared;
import Scene.Lights.MeshLightData;
import utils.Math.PackedFormats;

// Bindless vertex buffers (same declaration as BindlessVertexBuffer.slang).
[[vk::binding(0, 1)]]
ByteAddressBuffer t_BindlessBuffers[] : register(t0, space1);

// Instance + mesh descriptors (read-only SRVs, same as path_tracing.slang).
StructuredBuffer<GeometryInstanceData> instanceDescBuffer;
StructuredBuffer<MeshDesc> meshDescBuffer;

// CPU-provided work item: one per emissive mesh.
struct EmissiveMeshWorkItem {
    uint instanceID;            // instance_pool slot (== InstanceID())
    uint meshDescIndex;         // mesh_pool index (== GeometryInstanceData.geometryID)
    uint triangleOffset;        // base emissive triangle index for this mesh
    uint triangleCount;         // number of triangles in this mesh
    float3 emissionRadiance;    // pre-computed emission_color * emission (CPU)
    uint emissionTextureIndex;  // bindless texture index, or 0xffffffff
    uint materialLocation;      // material_header_pool index
    uint _pad0;
    uint _pad1;
};

StructuredBuffer<EmissiveMeshWorkItem> g_MeshWorkList : register(t0);

// Outputs (UAV).
RWStructuredBuffer<PackedEmissiveTriangle> g_EmissiveTriangles : register(u0);
RWStructuredBuffer<EmissiveFlux> g_EmissiveFlux : register(u1);
RWStructuredBuffer<uint> g_PerInstanceOffset : register(u2);

cbuffer g_Params : register(b0)
{
    uint g_TotalTriangles;
    uint g_MeshCount;
    uint g_InstancePoolSize;
    uint g_Pad;
};

/// Load local-space position for vertex k of triangle primId.
/// Mirrors BindlessVertexBuffer::load_vertex position path.
float3 loadLocalPosition(MeshDesc mesh, ByteAddressBuffer vb, uint primId, uint k)
{
    uint flatId = primId * 3 + k;
    uint vertexId = flatId;
    if (mesh.ibOffset > 0) {
        vertexId = vb.Load(mesh.ibOffset + flatId * 4);
    }
    return asfloat(vb.Load3(mesh.vbOffset + vertexId * 12));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    uint globalTriIdx = dtid.x;
    if (globalTriIdx >= g_TotalTriangles) return;

    // Find which mesh this triangle belongs to. Linear scan of the work list
    // (meshCount is typically small — single digits).
    EmissiveMeshWorkItem item;
    uint localTriIdx = 0;
    bool found = false;
    [loop]
    for (uint m = 0; m < g_MeshCount; m++) {
        if (globalTriIdx >= g_MeshWorkList[m].triangleOffset &&
            globalTriIdx < g_MeshWorkList[m].triangleOffset + g_MeshWorkList[m].triangleCount) {
            item = g_MeshWorkList[m];
            localTriIdx = globalTriIdx - item.triangleOffset;
            found = true;
            break;
        }
    }
    if (!found) return;

    // Read instance transform + mesh descriptor.
    GeometryInstanceData inst = instanceDescBuffer[item.instanceID];
    MeshDesc mesh = meshDescBuffer[item.meshDescIndex];
    ByteAddressBuffer vb = t_BindlessBuffers[NonUniformResourceIndex(mesh.bindlessIndex)];

    // Load 3 local-space positions.
    float3 p0 = loadLocalPosition(mesh, vb, localTriIdx, 0);
    float3 p1 = loadLocalPosition(mesh, vb, localTriIdx, 1);
    float3 p2 = loadLocalPosition(mesh, vb, localTriIdx, 2);

    // Transform to world space: p_world = transpose(transform) * p_local.
    // (GeometryInstanceData.transform is row-major GfMatrix4f; the existing
    // ClosestHit / BindlessVertexBuffer path uses transpose for the same
    // reason — see BindlessVertexBuffer.slang:113.)
    float4x4 worldMat = transpose(inst.transform);
    float3 w0 = mul(worldMat, float4(p0, 1.0)).xyz;
    float3 w1 = mul(worldMat, float4(p1, 1.0)).xyz;
    float3 w2 = mul(worldMat, float4(p2, 1.0)).xyz;

    // Geometric normal + area.
    float3 e1 = w1 - w0;
    float3 e2 = w2 - w0;
    float3 n = cross(e1, e2);
    float lenN = length(n);
    float area = 0.5 * lenN;
    if (area < 1e-8) return;  // degenerate triangle, skip
    float3 normal = n / lenN;

    // Pack into PackedEmissiveTriangle.
    PackedEmissiveTriangle packed;
    packed.posAndTexCoords[0] = float4(w0, asfloat(packed.encodeTexCoord(float2(0, 0))));
    packed.posAndTexCoords[1] = float4(w1, asfloat(packed.encodeTexCoord(float2(0, 0))));
    packed.posAndTexCoords[2] = float4(w2, asfloat(packed.encodeTexCoord(float2(0, 0))));
    packed.normal = encodeNormal2x16(normal);
    packed.area = asuint(area);
    packed.materialID = item.materialLocation;
    packed.lightIdx = m;
    packed.textureIndex = item.emissionTextureIndex;
    packed._pad = 0;

    g_EmissiveTriangles[globalTriIdx] = packed;

    // Flux: average radiance * area.
    EmissiveFlux flux;
    float avgRadiance = (item.emissionRadiance.x + item.emissionRadiance.y + item.emissionRadiance.z) / 3.0;
    flux.flux = avgRadiance * area;
    flux.averageRadiance = item.emissionRadiance;
    g_EmissiveFlux[globalTriIdx] = flux;

    // Write per-instance offset (first thread of each mesh).
    if (localTriIdx == 0) {
        g_PerInstanceOffset[item.instanceID] = item.triangleOffset;
    }
}
