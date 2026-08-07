#include "utils/view_cb.h"
import Scene.SceneTypes;

// ---[ Constant Buffers ]---
ConstantBuffer<PlanarViewConstants> viewConstant;

import Scene.BindlessVertexBuffer;
// Import MaterialEvaluation in the VS too so its material-buffer declarations
// (materialBlobBuffer, materialHeaderBuffer) appear in BOTH stages. The merged
// VS+PS reflection was dropping PS-only imported resources (a slang/nvrhi
// reflection quirk), which left materialTypeLUT/materialBlob/Header unbound.
// Declaring them in a module both stages import makes the reflection collect
// them. The VS never reads them, but the declaration must be present.
import Scene.MaterialEvaluation;

// Vertex shader output structure
struct VSOutput {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float3 worldPos : POSITION;
    float3 tangent : TANGENT;

#if defined(ENABLE_TEXCOORD)
    float2 texcoord : TEXCOORD0;
#endif

    // Material header index — used by the pixel shader to look up the material
    // blob + type from materialHeaderBuffer / the material-type LUT.
    nointerpolation uint materialID : MATERIAL_ID;
};

void main(
    uint i_vertexID: SV_VertexID,
    uint i_instanceID: SV_InstanceID,
    out VSOutput o_output)
{
    var InstanceData = instanceDescBuffer[i_instanceID];
    var MeshData = meshDescBuffer[InstanceData.geometryID];

    // Get the model matrix for the current instance
    float4x4 modelMatrix = InstanceData.transform;

    // Fetch the bindless vertex buffer for this mesh and load the vertex.
    // load_vertex takes (MeshDesc, vertex_id, ByteAddressBuffer); the buffer
    // comes from the bindless descriptor table indexed by mesh.bindlessIndex.
    ByteAddressBuffer vertex_buffer =
        t_BindlessBuffers[NonUniformResourceIndex(MeshData.bindlessIndex)];
    StaticVertexData vertex = load_vertex(MeshData, i_vertexID, vertex_buffer);

    // Transform the vertex position to world space
    float4 worldPos = mul(float4(vertex.position, 1.0), modelMatrix);
    // Transform the vertex position to clip space
    o_output.position = mul(worldPos, viewConstant.matWorldToClip);

    o_output.worldPos = worldPos.xyz;
    // (The old stub VS had `o_output.position.y *= -1` here to flip clip-space Y.
    // That flips the image vertically and put the triangle at the TOP of the
    // frame. matWorldToClip already produces correct D3D12 NDC, so no flip is
    // needed — removing it puts the triangle where the camera actually sees it.)

    o_output.normal =
        normalize(mul(float4(vertex.normal, 0.0), modelMatrix).xyz);

    o_output.tangent =
        normalize(mul(float4(vertex.tangent.xyz, 0.0), modelMatrix).xyz);

    // Prefer the per-subset material id baked into the vertex buffer; fall back
    // to the instance's materialID otherwise.
    o_output.materialID =
        vertex.subset_material_id != 0xFFFFFFFFu
            ? vertex.subset_material_id
            : InstanceData.materialID;

#if defined(ENABLE_TEXCOORD)
    o_output.texcoord = vertex.texCrd;
#endif
}
