import callable_data;
import Scene.MaterialParams;
import Scene.MaterialParamsBuffer;
import Scene.UsdPreviewSurface;
import utils.Spectrum;

// Preview Surface evaluation
[shader("callable")]
void eval_preview_surface(inout EvalCallableData data)
{
    // Read parameters from buffer
    MaterialParams params_blob =
        materialParamsBuffer[data.material_params_index];
    PreviewSurfaceMaterialParams params;

    // Reinterpret buffer data as PreviewSurfaceMaterialParams
    params = reinterpret<PreviewSurfaceMaterialParams>(params_blob);

    // Clamp roughness to avoid numerical issues with perfect mirrors
    const float MIN_ROUGHNESS = 0.001;
    params.roughness = max(params.roughness, MIN_ROUGHNESS);
    params.clearcoatRoughness = max(params.clearcoatRoughness, MIN_ROUGHNESS);

    // params.opacity = 1.0f;

    VertexData vd;
    vd.normalWorld = data.vertexInfo.normalW;
    vd.tangentWorld = data.vertexInfo.tangentW.xyz;
    vd.bitangentWorld =
        cross(data.vertexInfo.normalW, data.vertexInfo.tangentW.xyz) *
        data.vertexInfo.tangentW.w;
    vd.positionWorld = data.vertexInfo.posW;

    // Evaluate the BSDF with light direction L
    surfaceshader bsdf_result = surfaceshader(float3(0.0), float3(0.0));
    IMP_UsdPreviewSurface_surfaceshader(
        vd,
        data.L,
        data.V,
        data.eta_flipped,
        params.diffuseColor,
        params.emissiveColor,
        params.useSpecularWorkflow,
        params.specularColor,
        params.metallic,
        params.roughness,
        params.clearcoat,
        params.clearcoatRoughness,
        params.opacity,
        params.opacityMode,
        params.opacityThreshold,
        params.ior,
        params.normal,
        params.displacement,
        params.occlusion,
        bsdf_result);

    data.color = Spectrum(bsdf_result.color);

    // bsdf_pdf(L) for MIS balance heuristic on the NEE path.
    data.bsdfPdfForLightDir = eval_preview_surface_pdf(
        vd, data.V, data.L, data.eta_flipped, params.diffuseColor,
        params.metallic, params.roughness, params.ior);

    // Sample the BSDF for next direction
    data.sampledDir = sample_preview_surface(
        vd,
        data.V,
        params.diffuseColor,
        params.emissiveColor,
        params.useSpecularWorkflow,
        params.specularColor,
        params.metallic,
        params.roughness,
        params.clearcoat,
        params.clearcoatRoughness,
        params.opacity,
        params.opacityMode,
        params.opacityThreshold,
        params.ior,
        params.normal,
        params.displacement,
        params.occlusion,
        data.eta_flipped,
        data.seed,
        data.pdf);

    // Evaluate BSDF again with sampled direction to get throughput
    surfaceshader weight_result = surfaceshader(float3(0.0), float3(0.0));
    IMP_UsdPreviewSurface_surfaceshader(
        vd,
        data.sampledDir,
        data.V,
        data.eta_flipped,
        params.diffuseColor,
        params.emissiveColor,
        params.useSpecularWorkflow,
        params.specularColor,
        params.metallic,
        params.roughness,
        params.clearcoat,
        params.clearcoatRoughness,
        params.opacity,
        params.opacityMode,
        params.opacityThreshold,
        params.ior,
        params.normal,
        params.displacement,
        params.occlusion,
        weight_result);

    // MaterialX BSDF response already includes NdotL (response = f * NdotL),
    // so this is the correct throughput for the main loop's
    // current_throughput *= throughput / pdf update. Do NOT multiply NdotL
    // again -- that would double-count cos(theta) and darken the image.
    data.throughput = Spectrum(weight_result.color);
}
