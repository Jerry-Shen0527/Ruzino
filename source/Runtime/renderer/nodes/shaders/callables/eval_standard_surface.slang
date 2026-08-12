import callable_data;
import Scene.MaterialParams;
import Scene.MaterialParamsBuffer;
import Scene.StandardSurface;
import utils.Spectrum;

// Standard Surface evaluation
[shader("callable")]
void eval_standard_surface(inout EvalCallableData data)
{
    // Read parameters from buffer
    MaterialParams params_blob =
        materialParamsBuffer[data.material_params_index];
    PackedStandardSurfaceMaterialParams pakced_params;

    // Copy data from buffer (assuming MaterialParams can be reinterpreted)
    // In practice, you might need proper unpacking
    pakced_params =
        reinterpret<PackedStandardSurfaceMaterialParams>(params_blob);

    StandardSurfaceMaterialParams params =
        unpackStandardSurfaceMaterialParams(pakced_params);

    VertexData vd;
    vd.normalWorld = data.vertexInfo.normalW;
    vd.tangentWorld = data.vertexInfo.tangentW.xyz;
    vd.positionWorld = data.vertexInfo.posW;

    // Evaluate the BSDF with light direction L
    surfaceshader bsdf_result = surfaceshader(float3(0.0), float3(0.0));
    NG_standard_surface_surfaceshader_100(
        vd,
        data.L,
        data.V,
        data.eta_flipped,
        params.base,
        params.base_color,
        params.diffuse_roughness,
        params.metalness,
        params.specular,
        params.specular_color,
        params.specular_roughness,
        params.specular_IOR,
        params.specular_anisotropy,
        params.specular_rotation,
        params.transmission,
        params.transmission_color,
        params.transmission_depth,
        params.transmission_scatter,
        params.transmission_scatter_anisotropy,
        params.transmission_dispersion,
        params.transmission_extra_roughness,
        params.subsurface,
        params.subsurface_color,
        params.subsurface_radius,
        params.subsurface_scale,
        params.subsurface_anisotropy,
        params.sheen,
        params.sheen_color,
        params.sheen_roughness,
        params.coat,
        params.coat_color,
        params.coat_roughness,
        params.coat_anisotropy,
        params.coat_rotation,
        params.coat_IOR,
        params.coat_normal,
        params.coat_affect_color,
        params.coat_affect_roughness,
        params.thin_film_thickness,
        params.thin_film_IOR,
        params.emission,
        params.emission_color,
        params.opacity,
        params.thin_walled != 0,
        params.normal,
        params.tangent,
        bsdf_result);

    data.color = Spectrum(bsdf_result.color);

    // bsdf_pdf(L) for MIS balance heuristic on the NEE path. Build the same
    // layered context the sampler uses, then evaluate its pdf at the NEE
    // direction L (transformed into the base-layer local frame).
    float evalIOR = params.specular_IOR;
    if (params.transmission > 0.0 && params.thin_walled == 0)
        if (data.eta_flipped > 0.0)
            evalIOR = 1.0 / evalIOR;
    LayeredBsdfContext lctx = build_layered_bsdf_context(
        data.V, params.normal, params.tangent, params.specular_roughness,
        params.specular_anisotropy, params.specular_rotation, evalIOR,
        params.coat, params.coat_normal, params.coat_roughness,
        params.coat_anisotropy, params.coat_rotation, params.coat_IOR,
        params.base, params.base_color, params.metalness, params.specular,
        params.transmission, params.transmission_color);
    float3 L_local = lctx.sf.toLocal(data.L);
    data.bsdfPdfForLightDir = _layered_pdf(lctx, L_local);

    // Emitted radiance Le = emission_color * emission. The params are already
    // unpacked (texture values resolved by the fetch callable), so this is a
    // direct multiply. The main loop accumulates this when a BSDF-sampled ray
    // hits an emissive mesh surface (BSDF-hit-emissive path).
    data.emissionLe = Spectrum(params.emission_color * params.emission);

    // Sample the BSDF for next direction
    data.sampledDir = sample_standard_surface(
        vd,
        data.V,
        params.base,
        params.base_color,
        params.diffuse_roughness,
        params.metalness,
        params.specular,
        params.specular_color,
        params.specular_roughness,
        params.specular_IOR,
        params.specular_anisotropy,
        params.specular_rotation,
        params.transmission,
        params.transmission_color,
        params.transmission_depth,
        params.transmission_scatter,
        params.transmission_scatter_anisotropy,
        params.transmission_dispersion,
        params.transmission_extra_roughness,
        params.subsurface,
        params.subsurface_color,
        params.subsurface_radius,
        params.subsurface_scale,
        params.subsurface_anisotropy,
        params.sheen,
        params.sheen_color,
        params.sheen_roughness,
        params.coat,
        params.coat_color,
        params.coat_roughness,
        params.coat_anisotropy,
        params.coat_rotation,
        params.coat_IOR,
        params.coat_normal,
        params.coat_affect_color,
        params.coat_affect_roughness,
        params.thin_film_thickness,
        params.thin_film_IOR,
        params.emission,
        params.emission_color,
        params.opacity,
        params.thin_walled != 0,
        params.normal,
        params.tangent,
        data.eta_flipped,
        data.seed,
        data.pdf);

    // Evaluate BSDF with sampled direction to get throughput (only once)
    surfaceshader weight_result = surfaceshader(float3(0.0), float3(0.0));
    NG_standard_surface_surfaceshader_100(
        vd,
        data.sampledDir,
        data.V,
        data.eta_flipped,
        params.base,
        params.base_color,
        params.diffuse_roughness,
        params.metalness,
        params.specular,
        params.specular_color,
        params.specular_roughness,
        params.specular_IOR,
        params.specular_anisotropy,
        params.specular_rotation,
        params.transmission,
        params.transmission_color,
        params.transmission_depth,
        params.transmission_scatter,
        params.transmission_scatter_anisotropy,
        params.transmission_dispersion,
        params.transmission_extra_roughness,
        params.subsurface,
        params.subsurface_color,
        params.subsurface_radius,
        params.subsurface_scale,
        params.subsurface_anisotropy,
        params.sheen,
        params.sheen_color,
        params.sheen_roughness,
        params.coat,
        params.coat_color,
        params.coat_roughness,
        params.coat_anisotropy,
        params.coat_rotation,
        params.coat_IOR,
        params.coat_normal,
        params.coat_affect_color,
        params.coat_affect_roughness,
        params.thin_film_thickness,
        params.thin_film_IOR,
        params.emission,
        params.emission_color,
        params.opacity,
        params.thin_walled != 0,
        params.normal,
        params.tangent,
        weight_result);

    // Throughput for the main loop's `current_throughput *= throughput / pdf`.
    // MaterialX's mx_*_bsdf functions return the PREMULTIPLIED BSDF response
    // f(L,V) * NdotL (the GGX denominator is 4*NdotV only — NdotL is folded
    // into the numerator; diffuse lobes multiply by NdotL explicitly).
    // So weight_result.color already IS f*NdotL, and we must NOT multiply
    // by NdotSampled again — that would double-count cos(theta) and darken
    // every standard_surface render (the bug that made mirror reflections
    // read ~23% too dark). This matches eval_preview_surface's convention
    // (see its comment at the equivalent position).
    data.throughput = Spectrum(weight_result.color);
}
