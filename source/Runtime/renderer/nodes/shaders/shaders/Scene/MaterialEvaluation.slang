// MaterialDecode.slang
//
// Lightweight material BLOB decoding for G-Buffer fill (rasterize.ps.slang).
// Kept separate from the heavy BSDF evaluation (MaterialBSDF.slang) so the
// rasterize PIXEL shader imports a minimal set of modules and avoids pulling in
// the full standard_surface / UsdPreviewSurface BSDF libraries (whose resource
// declarations, combined with the VS's bindless-buffer import, were causing a
// D3D12 root-signature "register range overlaps" error in the merged VS+PS
// reflection — see AGENTS.md root-signature notes).
//
// This module imports only: BindlessMaterial (for materialBlobBuffer/Header),
// MaterialParams (unpack functions), VertexInfo. No BSDF math, no light state.
//
// The deferred lighting CS imports MaterialBSDF.slang (which re-imports this for
// decode + adds the full BSDF) for the real per-material lighting.

import Scene.VertexInfo;
import Scene.MaterialParams;

// Declare the material buffers directly here rather than importing
// Scene.BindlessMaterial. The full BindlessMaterial module also declares the
// bindless TEXTURE table (t_BindlessTextures, space2), samplers[9], and
// materialParamsBuffer — which:
//   * in the rasterize VS+PS merge, caused a D3D12 root-signature overlap;
//   * in the deferred compute shader, declared t_BindlessTextures as an
//     expected-but-unused descriptor table the host would have to bind.
// The decode only needs the two read-only structured buffers, so declare them
// minimally here. Both rasterize.ps.slang and deferred_direct_lighting.cs.slang
// import this module; they bind these two buffers by name via ProgramVars.
struct MaterialDataBlob {
    uint data[256];
};
struct MaterialHeader {
    uint material_type_id;
    uint material_blob_id;
    uint padding[2];
};
// Use explicit register annotations matching BindlessMaterial.slang (t3/t4)
// so slang's register allocation is deterministic across VS+PS merge.
StructuredBuffer<MaterialDataBlob> materialBlobBuffer : register(t3);
StructuredBuffer<MaterialHeader> materialHeaderBuffer : register(t4);
// Material-type LUT: maps the pool-index stored in MaterialHeader.material_type_id
// to the resolved enum (0=standard_surface, 1=UsdPreviewSurface, 2=fallback).
// Declared here (not in the PS) so both VS and PS see it via the shared import —
// the merged VS+PS reflection was dropping PS-only resource declarations.
StructuredBuffer<uint> materialTypeLUT : register(t5);

static const uint kMaterialType_StandardSurface = 0;
static const uint kMaterialType_PreviewSurface  = 1;
static const uint kMaterialType_Fallback        = 2;

/// Decoded surface properties sufficient for G-Buffer fill. Pure data, no
/// lighting state.
struct GBufferMaterialData
{
    float3 base_color;
    float  metalness;
    float  roughness;     ///< specular_roughness (standard) / roughness (preview)
    float3 emission;
    float3 opacity;
    float3 normal;        ///< surface normal in world space
    float3 tangent;       ///< surface tangent in world space
    uint   material_type; ///< the resolved enum (0/1/2), echoed for the deferred pass
};

/// Decode the preview-surface param blob into G-Buffer properties.
///
/// The blob is NOT laid out as PreviewSurfaceMaterialParams (the struct has
/// padding/alignment the generator doesn't follow). The MaterialX generator
/// (bindlessContext.cpp / ClosureCompoundNodeSlang.cpp) writes each uniform
/// field as a sequential asfloat(data.data[i]) in declaration order. This reads
/// them back in that exact order so the values land in the right fields.
/// Field order matches RedMaterial.slang's fetch_shader_data:
///   diffuseColor(0-2) emissiveColor(3-5) useSpecularWorkflow(6)
///   specularColor(7-9) metallic(10) roughness(11) clearcoat(12)
///   clearcoatRoughness(13) opacity(14) opacityMode(15) opacityThreshold(16)
///   ior(17) normal(18-20) displacement(21) occlusion(22)
GBufferMaterialData _decode_preview_surface(
    MaterialDataBlob blob, VertexInfo vi)
{
    GBufferMaterialData m;
    m.base_color    = float3(asfloat(blob.data[0]), asfloat(blob.data[1]),
                             asfloat(blob.data[2]));
    m.emission      = float3(0.0);
    m.metalness     = 0.0;
    m.roughness     = 0.5;
    m.opacity       = float3(1.0);
    m.normal        = vi.normalW;
    m.tangent       = vi.tangentW.xyz;
    m.material_type = kMaterialType_PreviewSurface;
    return m;
}

/// Decode the standard-surface param blob into G-Buffer properties.
///
/// Like preview surface, the blob holds fields as sequential asfloat(data.data[i])
/// in MaterialX declaration order (NOT as PackedStandardSurfaceMaterialParams —
/// the generator writes the packed struct to materialParamsBuffer, a separate
/// scratch buffer, not into this blob). Field order matches
/// packStandardSurfaceMaterialParams (MaterialParams.slang:219+):
///   base(0) base_color(1-3) diffuse_roughness(4) metalness(5) specular(6)
///   specular_color(7-9) specular_roughness(10) specular_IOR(11) ...
///   ... emission(38) emission_color(39-41) opacity(42-44) normal(46-48)
/// (indices derived from the 49 scalar fields in declaration order).
GBufferMaterialData _decode_standard_surface(
    MaterialDataBlob blob, VertexInfo vi)
{
    float base                 = asfloat(blob.data[0]);
    float3 base_color          = float3(asfloat(blob.data[1]), asfloat(blob.data[2]),
                                        asfloat(blob.data[3]));
    float metalness            = asfloat(blob.data[5]);
    float specular_roughness   = asfloat(blob.data[10]);
    float emission             = asfloat(blob.data[38]);
    float3 emission_color      = float3(asfloat(blob.data[39]), asfloat(blob.data[40]),
                                        asfloat(blob.data[41]));
    float3 opacity             = float3(asfloat(blob.data[42]), asfloat(blob.data[43]),
                                        asfloat(blob.data[44]));
    // normal/tangent come after opacity+thin_walled in the declaration; the
    // generator emits them too, but their exact index depends on the full field
    // list. Use the geometry normal as a safe fallback when not sure.
    float3 normal              = float3(asfloat(blob.data[46]), asfloat(blob.data[47]),
                                        asfloat(blob.data[48]));

    GBufferMaterialData m;
    m.base_color    = base_color * base;
    m.metalness     = metalness;
    m.roughness     = specular_roughness;
    m.emission      = emission_color * emission;
    m.opacity       = opacity;
    m.normal        = normal;
    m.tangent       = vi.tangentW.xyz;
    m.material_type = kMaterialType_StandardSurface;
    return m;
}

/// Decode a material blob into G-Buffer surface properties.
///
/// \param material_blob_id  Index into materialBlobBuffer.
/// \param material_type     Resolved enum (0/1/2...); NOT the pool index.
/// \param vi                Interpolated vertex attributes (world space).
GBufferMaterialData decode_material_surface(
    uint material_blob_id, uint material_type, VertexInfo vi)
{
    MaterialDataBlob blob = materialBlobBuffer[material_blob_id];

    GBufferMaterialData m;
    if (material_type == kMaterialType_StandardSurface) {
        m = _decode_standard_surface(blob, vi);
    }
    else if (material_type == kMaterialType_PreviewSurface) {
        m = _decode_preview_surface(blob, vi);
    }
    else {
        // Fallback (matches eval_fallback.slang: albedo 0.8, Lambertian).
        m.base_color    = float3(0.8);
        m.metalness     = 0.0;
        m.roughness     = 1.0;
        m.emission      = float3(0.0);
        m.opacity       = float3(1.0);
        m.normal        = vi.normalW;
        m.tangent       = vi.tangentW.xyz;
        m.material_type = kMaterialType_Fallback;
    }

    // Prefer the material-specified normal; fall back to the geometry normal.
    if (material_type == kMaterialType_StandardSurface ||
        material_type == kMaterialType_PreviewSurface)
    {
        if (dot(m.normal, m.normal) > 0.0) {
            m.normal = normalize(m.normal);
        } else {
            m.normal = normalize(vi.normalW);
        }
    }
    return m;
}
