// Pixel shader for the G-Buffer rasterization pass.
//
// Fills the multi-render-target G-Buffer. Material decode lives in
// Scene.MaterialEvaluation (decode_material_surface), imported by BOTH the VS
// and PS — the merged VS+PS reflection drops PS-only imported resources, so the
// module must be imported in both stages for its buffer declarations to appear.
//
// STATUS: the material-decode path (reading materialBlobBuffer) currently
// removes the device intermittently (header read works, blob read is flaky —
// see rasterization-gbuffer-pipeline memory). Until that's stable, base_color
// falls back to a constant. The decode call + GBufferMaterialData struct are
// kept so the path is one line-flip away from active.
#include "utils/view_cb.h"
import Scene.SceneTypes;
import Scene.VertexInfo;
import Scene.MaterialEvaluation;

// Input structure from vertex shader
struct VSOutput {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float3 worldPos : POSITION;
    float3 tangent : TANGENT;

#if defined(ENABLE_TEXCOORD)
    float2 texcoord : TEXCOORD0;
#endif

    nointerpolation uint materialID : MATERIAL_ID;
};

// Main pixel shader function
void main(
    in VSOutput input,
    out float4 o_pos: SV_Target0,
    out float2 o_texcoord: SV_Target1,
    out float4 o_diffuse_color: SV_Target2,
    out float4 o_metallic_roughness: SV_Target3,
    out float4 o_normal: SV_Target4,
    out float o_material_id: SV_Target5,
    out float depth: SV_Depth)
{
    // Build a VertexInfo (world-space) the material decode expects.
    VertexInfo vi;
    vi.posW    = input.worldPos;
    vi.normalW = normalize(input.normal);
    vi.tangentW = float4(normalize(input.tangent), 1.0);
    vi.faceNormalW = normalize(input.normal);
#if defined(ENABLE_TEXCOORD)
    vi.texC = input.texcoord;
#else
    vi.texC = float2(0, 0);
#endif
    vi.curveRadius = 0.0;
    vi.coneTexLODValue = 0.0;
    vi.subsetMaterialID = input.materialID;

    // Resolve the material type from the pool-index LUT. (The LUT upload is
    // currently not reaching the GPU correctly — see memory — so this reads 0.
    // The decode still runs but may hit the fallback branch.)
    MaterialHeader header = materialHeaderBuffer[input.materialID];
    uint material_type = kMaterialType_Fallback;
    if (header.material_type_id < 65536u) {
        material_type = materialTypeLUT[header.material_type_id];
    }

    // decode_material_surface reads materialBlobBuffer which currently removes
    // the device — commented out until that's fixed. The GBufferMaterialData
    // fields below use fallback constants.
    // GBufferMaterialData m =
    //     decode_material_surface(header.material_blob_id, material_type, vi);

    // Output world position in the position buffer
    o_pos = float4(input.worldPos, 1.0);

    // Output normalized shading normal vector (geometry normal; m.normal would
    // be the material-specified normal once decode is stable).
    o_normal = float4(normalize(input.normal), 1.0);

    // Output depth value
    depth = input.position.z;

    // Output material properties. materialBlobBuffer reads currently remove the
    // device (see rasterization-gbuffer-pipeline memory), so m.base_color is
    // unreliable; fall back to a constant red so the G-Buffer renders.
    // TODO: fix materialBlobBuffer binding/read, then use m.base_color.
    o_diffuse_color = float4(0.8, 0.2, 0.2, 1.0);
    o_metallic_roughness = float4(0.0, 0.5, 0.0, 1.0);

    // Echo the material id (stored as float — see VS comment re: R32_FLOAT).
    o_material_id = float(input.materialID);

#if defined(ENABLE_TEXCOORD)
    // Output texture coordinates if enabled
    o_texcoord = input.texcoord;
#else
    o_texcoord = float2(0, 0);
#endif
}
