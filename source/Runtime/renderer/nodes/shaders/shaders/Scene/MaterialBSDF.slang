// MaterialBSDF.slang
//
// Full per-material BSDF evaluation for the raster deferred-lighting pass
// (deferred_direct_lighting.cs.slang). This is the raster analogue of the path
// tracer's eval_standard_surface / eval_preview_surface / eval_fallback DXR
// callables — but as plain Slang functions importable from a compute shader,
// since a raster/compute pipeline cannot CallShader.
//
// Kept SEPARATE from MaterialDecode.slang (the lightweight G-Buffer decode used
// by rasterize.ps.slang) because importing the full standard_surface /
// UsdPreviewSurface BSDF libraries into the rasterize PIXEL shader, together
// with the VS's bindless-buffer import, triggered a D3D12 root-signature
// "register range overlaps" error in the merged VS+PS reflection. The deferred
// CS is a single stage (no VS+PS merge), so it can safely pull in everything.
//
// Honors the plan: "根据不同的 material id 调用不同的函数" — evaluate_material_bsdf
// dispatches to NG_standard_surface / IMP_UsdPreviewSurface / Lambertian based
// on material_type.
//
// Returns the PURE BSDF value f(L,V) (NO NdotL), matching eval_* conventions;
// the caller multiplies NdotL itself:
//     contribution = light.intensity * NdotL * f(L,V) / pdf

import Scene.VertexInfo;
import Scene.VertexData;
import Scene.MaterialParams;
// MaterialEvaluation declares materialBlobBuffer (t3) + MaterialDataBlob
// directly (no bindless texture table). Do NOT import Scene.BindlessMaterial
// alongside it — redeclaration of those types would be ambiguous.
import Scene.MaterialEvaluation;  // GBufferMaterialData, decode_material_surface, enums, materialBlobBuffer
import Scene.StandardSurface;     // NG_standard_surface_surfaceshader_100
import Scene.UsdPreviewSurface;   // IMP_UsdPreviewSurface_surfaceshader
import stdlib.genslang.mx_surfaceshader;  // surfaceshader struct

#include "utils/Math/MathConstants.slangh"

/// Evaluate the material BSDF value f(L, V) for direct lighting.
///
/// \param m                Decoded surface properties (from decode_material_surface).
/// \param material_blob_id Index into materialBlobBuffer — needed because the
///                         G-Buffer stores only a subset of the params; the
///                         BSDF needs the full set (re-decoded from the blob).
/// \param vi               Interpolated vertex attributes (world space).
/// \param L                Direction TOWARD the light (normalized, world space).
/// \param V                Direction TOWARD the viewer (normalized, world space).
float3 evaluate_material_bsdf(
    GBufferMaterialData m,
    uint material_blob_id,
    VertexInfo vi,
    float3 L,
    float3 V)
{
    // Build the VertexData the MaterialX BSDF functions expect.
    VertexData vd;
    vd.normalWorld    = normalize(vi.normalW);
    vd.tangentWorld   = normalize(vi.tangentW.xyz);
    vd.bitangentWorld = normalize(cross(vd.normalWorld, vd.tangentWorld)) *
                        vi.tangentW.w;
    vd.positionWorld  = vi.posW;

    const uint eta_flipped = 0u;
    const float PI = 3.14159265359;

    if (m.material_type == kMaterialType_StandardSurface) {
        MaterialDataBlob blob = materialBlobBuffer[material_blob_id];
        PackedStandardSurfaceMaterialParams packed =
            reinterpret<PackedStandardSurfaceMaterialParams>(blob);
        StandardSurfaceMaterialParams params =
            unpackStandardSurfaceMaterialParams(packed);

        surfaceshader bsdf_result = surfaceshader(float3(0.0), float3(0.0));
        NG_standard_surface_surfaceshader_100(
            vd, L, V, eta_flipped,
            params.base, params.base_color, params.diffuse_roughness,
            params.metalness, params.specular, params.specular_color,
            params.specular_roughness, params.specular_IOR,
            params.specular_anisotropy, params.specular_rotation,
            params.transmission, params.transmission_color,
            params.transmission_depth, params.transmission_scatter,
            params.transmission_scatter_anisotropy,
            params.transmission_dispersion, params.transmission_extra_roughness,
            params.subsurface, params.subsurface_color,
            params.subsurface_radius, params.subsurface_scale,
            params.subsurface_anisotropy, params.sheen, params.sheen_color,
            params.sheen_roughness, params.coat, params.coat_color,
            params.coat_roughness, params.coat_anisotropy,
            params.coat_rotation, params.coat_IOR, params.coat_normal,
            params.coat_affect_color, params.coat_affect_roughness,
            params.thin_film_thickness, params.thin_film_IOR,
            params.emission, params.emission_color, params.opacity,
            params.thin_walled != 0u, params.normal, params.tangent,
            bsdf_result);
        return bsdf_result.color;
    }

    if (m.material_type == kMaterialType_PreviewSurface) {
        MaterialDataBlob blob = materialBlobBuffer[material_blob_id];
        PreviewSurfaceMaterialParams params =
            reinterpret<PreviewSurfaceMaterialParams>(blob);

        surfaceshader bsdf_result = surfaceshader(float3(0.0), float3(0.0));
        IMP_UsdPreviewSurface_surfaceshader(
            vd, L, V, eta_flipped, params.diffuseColor, params.emissiveColor,
            params.useSpecularWorkflow, params.specularColor, params.metallic,
            params.roughness, params.clearcoat, params.clearcoatRoughness,
            params.opacity, params.opacityMode, params.opacityThreshold,
            params.ior, params.normal, params.displacement, params.occlusion,
            bsdf_result);
        return bsdf_result.color;
    }

    // Fallback: Lambertian, albedo 0.8 (matches eval_fallback.slang).
    float3 albedo = float3(0.8);
    return albedo / PI;
}
