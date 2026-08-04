
import stdlib.genslang.mx_surfaceshader;

__exported import stdlib.genslang.mx_surfaceshader;
__exported import VertexData;
import utils.Math.ShadingFrame;

import pbrlib.genslang.mx_roughness_anisotropy;

import pbrlib.genslang.mx_artistic_ior;

import pbrlib.genslang.mx_uniform_edf;

import stdlib.genslang.mx_normalmap;

void NG_switch_floatI(
    VertexData vd,
    float in1,
    float in2,
    float in3,
    float in4,
    float in5,
    float in6,
    float in7,
    float in8,
    float in9,
    float in10,
    int which,
    out float out1)
{
    const int ifgreater_10_value1_tmp = 10;
    const float ifgreater_10_in2_tmp = 0.000000;
    float ifgreater_10_out =
        (ifgreater_10_value1_tmp > which) ? in10 : ifgreater_10_in2_tmp;
    const int ifgreater_9_value1_tmp = 9;
    float ifgreater_9_out =
        (ifgreater_9_value1_tmp > which) ? in9 : ifgreater_10_out;
    const int ifgreater_8_value1_tmp = 8;
    float ifgreater_8_out =
        (ifgreater_8_value1_tmp > which) ? in8 : ifgreater_9_out;
    const int ifgreater_7_value1_tmp = 7;
    float ifgreater_7_out =
        (ifgreater_7_value1_tmp > which) ? in7 : ifgreater_8_out;
    const int ifgreater_6_value1_tmp = 6;
    float ifgreater_6_out =
        (ifgreater_6_value1_tmp > which) ? in6 : ifgreater_7_out;
    const int ifgreater_5_value1_tmp = 5;
    float ifgreater_5_out =
        (ifgreater_5_value1_tmp > which) ? in5 : ifgreater_6_out;
    const int ifgreater_4_value1_tmp = 4;
    float ifgreater_4_out =
        (ifgreater_4_value1_tmp > which) ? in4 : ifgreater_5_out;
    const int ifgreater_3_value1_tmp = 3;
    float ifgreater_3_out =
        (ifgreater_3_value1_tmp > which) ? in3 : ifgreater_4_out;
    const int ifgreater_2_value1_tmp = 2;
    float ifgreater_2_out =
        (ifgreater_2_value1_tmp > which) ? in2 : ifgreater_3_out;
    const int ifgreater_1_value1_tmp = 1;
    float ifgreater_1_out =
        (ifgreater_1_value1_tmp > which) ? in1 : ifgreater_2_out;
    out1 = ifgreater_1_out;
}

void NG_convert_float_color3(VertexData vd, float in, out float3 out1)
{
    float3 combine_out = float3(in, in, in);
    out1 = combine_out;
}

import pbrlib.genslang.mx_generalized_schlick_bsdf;

import pbrlib.genslang.mx_oren_nayar_diffuse_bsdf;

import pbrlib.genslang.mx_dielectric_bsdf;

import pbrlib.genslang.mx_conductor_bsdf;

import pbrlib.genslang.mx_mix_bsdf;

import pbrlib.genslang.mx_layer_bsdf;

bool is_close(float3 a, float3 b)
{
    const float EPSILON = 1e-5;
    return all(abs(a - b) < float3(EPSILON, EPSILON, EPSILON));
}

void IMP_UsdPreviewSurface_surfaceshader(
    VertexData vd,
    float3 L,
    float3 V,
    uint eta_flipped,
    float3 diffuseColor,
    float3 emissiveColor,
    int useSpecularWorkflow,
    float3 specularColor,
    float metallic,
    float roughness,
    float clearcoat,
    float clearcoatRoughness,
    float opacity,
    int opacityMode,
    float opacityThreshold,
    float ior,
    float3 normal,
    float displacement,
    float occlusion,
    out surfaceshader out1)
{
    if (is_close(normalize(L), normalize(-V))) {
        out1 = surfaceshader(float3(1.0f), float3(1.0));
        return;
    }
    // NOTE: the hard cutout (opacity<0.25 → fully transparent) was removed
    // because it overlapped with the path-tracing main loop's stochastic
    // pass-through (path_tracing.slang: random > opacity → ray continues).
    // Rays reaching this BSDF eval have already been judged to hit an existing
    // surface, so we must not zero them out again here. The opacity→
    // transmission_mix mapping below is KEPT: that is the surface's own
    // refraction model (PreviewSurface has no transmission param, so opacity
    // doubles as the dielectric mix weight), distinct from geometric presence.

    float3 geomprop_Tworld_out1 = normalize(vd.tangentWorld);
    const float one_minus_ior_in1_tmp = 1.000000;
    float one_minus_ior_out = one_minus_ior_in1_tmp - ior;
    const float one_plus_ior_in1_tmp = 1.000000;
    float one_plus_ior_out = one_plus_ior_in1_tmp + ior;
    float2 coat_roughness_out = float2(0.0);
    mx_roughness_anisotropy(clearcoatRoughness, 0.000000, coat_roughness_out);
    float3 geomprop_Nworld_out1 = normalize(vd.normalWorld);
    float3 geomprop_Bworld_out1 = normalize(vd.bitangentWorld);
    const float scale_normal_in2_tmp = 0.500000;
    float3 scale_normal_out = normal * scale_normal_in2_tmp;
    float2 specular_roughness_out = float2(0.0);
    mx_roughness_anisotropy(roughness, 0.000000, specular_roughness_out);
    const float inverse_metalness_in1_tmp = 1.000000;
    float inverse_metalness_out = inverse_metalness_in1_tmp - metallic;
    float use_specular_workflow_float_out = float(useSpecularWorkflow);
    const float transmission_mix_amount_value2_tmp = 0.000000;
    const float transmission_mix_amount_in1_tmp = 1.000000;
    float transmission_mix_amount_out =
        (opacityThreshold > transmission_mix_amount_value2_tmp)
            ? transmission_mix_amount_in1_tmp
            : opacity;
    float3 artistic_ior_ior = float3(0.0);
    float3 artistic_ior_extinction = float3(0.0);
    mx_artistic_ior(
        diffuseColor, diffuseColor, artistic_ior_ior, artistic_ior_extinction);
    const float3 specular_color_metallic_bg_tmp =
        float3(1.000000, 1.000000, 1.000000);
    float3 specular_color_metallic_out =
        lerp(specular_color_metallic_bg_tmp, diffuseColor, metallic);
    const float cutout_opacity_in1_tmp = 1.000000;
    const float cutout_opacity_in2_tmp = 0.000000;
    float cutout_opacity_out = (opacity >= opacityThreshold)
                                   ? cutout_opacity_in1_tmp
                                   : cutout_opacity_in2_tmp;
    float R_out = one_minus_ior_out / one_plus_ior_out;
    const float bias_normal_in2_tmp = 0.500000;
    float3 bias_normal_out = scale_normal_out + bias_normal_in2_tmp;
    const float diffuse_bsdf_weight_fg_tmp = 1.000000;
    float diffuse_bsdf_weight_out = lerp(
        inverse_metalness_out,
        diffuse_bsdf_weight_fg_tmp,
        use_specular_workflow_float_out);
    const float opacity_presence_value2_tmp = 0.000000;
    const float opacity_presence_in1_tmp = 0.000000;
    float opacity_presence_out = (opacity == opacity_presence_value2_tmp)
                                     ? opacity_presence_in1_tmp
                                     : cutout_opacity_out;
    float R_sq_out = R_out * R_out;
    float3 surface_normal_out = float3(0.0);
    mx_normalmap_float(
        bias_normal_out,
        1.000000,
        geomprop_Nworld_out1,
        geomprop_Tworld_out1,
        geomprop_Bworld_out1,
        surface_normal_out);
    float opacity_switch_out = 0.0;
    NG_switch_floatI(
        vd,
        cutout_opacity_out,
        opacity_presence_out,
        0.000000,
        0.000000,
        0.000000,
        0.000000,
        0.000000,
        0.000000,
        0.000000,
        0.000000,
        opacityMode,
        opacity_switch_out);
    float3 coat_F0_out = float3(0.0);
    NG_convert_float_color3(vd, R_sq_out, coat_F0_out);
    float3 specular_color_metallic_R_sq_out =
        specular_color_metallic_out * R_sq_out;
    float3 F0_out = lerp(
        specular_color_metallic_R_sq_out,
        specular_color_metallic_out,
        metallic);
    surfaceshader surface_constructor_out =
        surfaceshader(float3(0.0), float3(0.0));
    {
        float3 N = normalize(vd.normalWorld);
        float3 P = vd.positionWorld;
        float occlusion = 1.0;

        // Shadow occlusion

        // Calculate the BSDF response for this light source
        {
            ClosureData closureData =
                ClosureData(CLOSURE_TYPE_REFLECTION, L, V, N, P, occlusion);
            BSDF coat_dielectric_bsdf_out =
                BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_generalized_schlick_bsdf(
                closureData,
                clearcoat,
                coat_F0_out,
                float3(1.000000, 1.000000, 1.000000),
                float3(1.000000, 1.000000, 1.000000),
                5.000000,
                coat_roughness_out,
                0.000000,
                1.500000,
                surface_normal_out,
                geomprop_Tworld_out1,
                0,
                0,
                coat_dielectric_bsdf_out);
            BSDF specular_bsdf1_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_generalized_schlick_bsdf(
                closureData,
                1.000000,
                specularColor,
                float3(1.000000, 1.000000, 1.000000),
                float3(1.000000, 1.000000, 1.000000),
                5.000000,
                specular_roughness_out,
                0.000000,
                1.500000,
                surface_normal_out,
                geomprop_Tworld_out1,
                0,
                0,
                specular_bsdf1_out);
            BSDF diffuse_bsdf_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_oren_nayar_diffuse_bsdf(
                closureData,
                diffuse_bsdf_weight_out,
                diffuseColor,
                0.000000,
                surface_normal_out,
                false,
                diffuse_bsdf_out);
            BSDF transmission_bsdf_out =
                BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_dielectric_bsdf(
                closureData,
                1.000000,
                float3(1.000000, 1.000000, 1.000000),
                ior,
                float2(0.000000, 0.000000),
                0.000000,
                1.500000,
                surface_normal_out,
                geomprop_Tworld_out1,
                0,
                1,
                transmission_bsdf_out);
            BSDF transmission_mix_out =
                BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_mix_bsdf(
                closureData,
                diffuse_bsdf_out,
                transmission_bsdf_out,
                transmission_mix_amount_out,
                transmission_mix_out);
            BSDF specular_workflow_bsdf_out =
                BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_layer_bsdf(
                closureData,
                specular_bsdf1_out,
                transmission_mix_out,
                specular_workflow_bsdf_out);
            BSDF metalness_metal_bsdf_out =
                BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_conductor_bsdf(
                closureData,
                1.000000,
                artistic_ior_ior,
                artistic_ior_extinction,
                specular_roughness_out,
                0.000000,
                1.500000,
                surface_normal_out,
                geomprop_Tworld_out1,
                0,
                metalness_metal_bsdf_out);
            BSDF specular_bsdf2_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_generalized_schlick_bsdf(
                closureData,
                1.000000,
                F0_out,
                float3(1.000000, 1.000000, 1.000000),
                specular_color_metallic_out,
                5.000000,
                specular_roughness_out,
                0.000000,
                1.500000,
                surface_normal_out,
                geomprop_Tworld_out1,
                0,
                0,
                specular_bsdf2_out);
            BSDF metalness_specular_bsdf_out =
                BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_layer_bsdf(
                closureData,
                specular_bsdf2_out,
                transmission_mix_out,
                metalness_specular_bsdf_out);
            BSDF metalness_workflow_bsdf_out =
                BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_mix_bsdf(
                closureData,
                metalness_metal_bsdf_out,
                metalness_specular_bsdf_out,
                metallic,
                metalness_workflow_bsdf_out);
            BSDF workflow_selector_bsdf_out =
                BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_mix_bsdf(
                closureData,
                specular_workflow_bsdf_out,
                metalness_workflow_bsdf_out,
                use_specular_workflow_float_out,
                workflow_selector_bsdf_out);
            BSDF coat_bsdf_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_layer_bsdf(
                closureData,
                coat_dielectric_bsdf_out,
                workflow_selector_bsdf_out,
                coat_bsdf_out);

            // Accumulate the light's contribution
            surface_constructor_out.color += coat_bsdf_out.response;
        }

        // // Calculate the BSDF transmission for viewing direction
        // {
        //     ClosureData closureData =
        //         ClosureData(CLOSURE_TYPE_TRANSMISSION, L, V, N, P, occlusion);
        //     BSDF coat_dielectric_bsdf_out =
        //         BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
        //     mx_generalized_schlick_bsdf(
        //         closureData,
        //         clearcoat,
        //         coat_F0_out,
        //         float3(1.000000, 1.000000, 1.000000),
        //         float3(1.000000, 1.000000, 1.000000),
        //         5.000000,
        //         coat_roughness_out,
        //         0.000000,
        //         1.500000,
        //         surface_normal_out,
        //         geomprop_Tworld_out1,
        //         0,
        //         0,
        //         coat_dielectric_bsdf_out);
        //     BSDF specular_bsdf1_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
        //     mx_generalized_schlick_bsdf(
        //         closureData,
        //         1.000000,
        //         specularColor,
        //         float3(1.000000, 1.000000, 1.000000),
        //         float3(1.000000, 1.000000, 1.000000),
        //         5.000000,
        //         specular_roughness_out,
        //         0.000000,
        //         1.500000,
        //         surface_normal_out,
        //         geomprop_Tworld_out1,
        //         0,
        //         0,
        //         specular_bsdf1_out);
        //     BSDF diffuse_bsdf_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
        //     mx_oren_nayar_diffuse_bsdf(
        //         closureData,
        //         diffuse_bsdf_weight_out,
        //         diffuseColor,
        //         0.000000,
        //         surface_normal_out,
        //         false,
        //         diffuse_bsdf_out);
        //     BSDF transmission_bsdf_out =
        //         BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
        //     mx_dielectric_bsdf(
        //         closureData,
        //         1.000000,
        //         float3(1.000000, 1.000000, 1.000000),
        //         ior,
        //         float2(0.000000, 0.000000),
        //         0.000000,
        //         1.500000,
        //         surface_normal_out,
        //         geomprop_Tworld_out1,
        //         0,
        //         1,
        //         transmission_bsdf_out);
        //     BSDF transmission_mix_out =
        //         BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
        //     mx_mix_bsdf(
        //         closureData,
        //         diffuse_bsdf_out,
        //         transmission_bsdf_out,
        //         transmission_mix_amount_out,
        //         transmission_mix_out);
        //     BSDF specular_workflow_bsdf_out =
        //         BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
        //     mx_layer_bsdf(
        //         closureData,
        //         specular_bsdf1_out,
        //         transmission_mix_out,
        //         specular_workflow_bsdf_out);
        //     BSDF metalness_metal_bsdf_out =
        //         BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
        //     mx_conductor_bsdf(
        //         closureData,
        //         1.000000,
        //         artistic_ior_ior,
        //         artistic_ior_extinction,
        //         specular_roughness_out,
        //         0.000000,
        //         1.500000,
        //         surface_normal_out,
        //         geomprop_Tworld_out1,
        //         0,
        //         metalness_metal_bsdf_out);
        //     BSDF specular_bsdf2_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
        //     mx_generalized_schlick_bsdf(
        //         closureData,
        //         1.000000,
        //         F0_out,
        //         float3(1.000000, 1.000000, 1.000000),
        //         specular_color_metallic_out,
        //         5.000000,
        //         specular_roughness_out,
        //         0.000000,
        //         1.500000,
        //         surface_normal_out,
        //         geomprop_Tworld_out1,
        //         0,
        //         0,
        //         specular_bsdf2_out);
        //     BSDF metalness_specular_bsdf_out =
        //         BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
        //     mx_layer_bsdf(
        //         closureData,
        //         specular_bsdf2_out,
        //         transmission_mix_out,
        //         metalness_specular_bsdf_out);
        //     BSDF metalness_workflow_bsdf_out =
        //         BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
        //     mx_mix_bsdf(
        //         closureData,
        //         metalness_metal_bsdf_out,
        //         metalness_specular_bsdf_out,
        //         metallic,
        //         metalness_workflow_bsdf_out);
        //     BSDF workflow_selector_bsdf_out =
        //         BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
        //     mx_mix_bsdf(
        //         closureData,
        //         specular_workflow_bsdf_out,
        //         metalness_workflow_bsdf_out,
        //         use_specular_workflow_float_out,
        //         workflow_selector_bsdf_out);
        //     BSDF coat_bsdf_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
        //     mx_layer_bsdf(
        //         closureData,
        //         coat_dielectric_bsdf_out,
        //         workflow_selector_bsdf_out,
        //         coat_bsdf_out);
        //     surface_constructor_out.color += coat_bsdf_out.response;
        // }

        // Apply emissive only. The former `color *= surfaceOpacity` multiply
        // was removed: it double-attenuated energy that the path-tracing main
        // loop already handles stochastically (rays that should pass through
        // an opacity<1 surface never reach this BSDF eval). The opacity→
        // transmission_mix weight above is the surface's refraction model and
        // is intentionally kept.
        {
            surface_constructor_out.color += emissiveColor;
        }
    }

    out1 = surface_constructor_out;
}

import pbrlib.genslang.mx_roughness_anisotropy;
import pbrlib.genslang.lib.mx_microfacet;
import pbrlib.genslang.lib.mx_microfacet_specular;
import pbrlib.genslang.lib.mx_microfacet_diffuse;
import utils.Math.MathHelpers;
#include "utils/Math/MathConstants.slangh"
#include "utils/random.slangh"

// Calculate luminance of a color
float luminance(float3 color)
{
    return dot(color, float3(0.212671, 0.715160, 0.072169));
}

// Exact Fresnel reflectance for USD Preview Surface
float fresnel_dielectric(float cosTheta, float eta)
{
    float sinThetaT2 = (1.0 - cosTheta * cosTheta) / (eta * eta);

    // Check for total internal reflection
    if (sinThetaT2 >= 1.0) {
        return 1.0;
    }

    float cosThetaT = sqrt(1.0 - sinThetaT2);

    float Rs = (cosTheta - eta * cosThetaT) / (cosTheta + eta * cosThetaT);
    float Rp = (eta * cosTheta - cosThetaT) / (eta * cosTheta + cosThetaT);

    return 0.5 * (Rs * Rs + Rp * Rp);
}

// Sample diffuse component using cosine hemisphere sampling
float3 sample_diffuse_lobe(float2 u, out float pdf)
{
    float3 L = sample_cosine_hemisphere_concentric(u, pdf);
    return L;
}

// Sample specular reflection using GGX distribution
float3
sample_specular_reflection(float2 u, float3 V, float roughness, out float pdf)
{
    float2 alpha = float2(roughness * roughness, roughness * roughness);

    // Sample microfacet normal using GGX VNDF distribution
    float3 H = mx_ggx_importance_sample_VNDF(u, V, alpha);

    // Reflect view direction around microfacet normal
    float3 L = reflect(-V, H);

    // Check if the reflection is valid (above surface)
    // Use a small epsilon to tolerate numerical errors, especially for grazing
    // angles
    if (L.z <= -M_FLOAT_EPS) {
        pdf = 0.0;
        return float3(0.0);
    }

    // Compute PDF using correct VNDF formulation
    float NdotH = max(H.z, M_FLOAT_EPS);
    float VdotH = max(dot(V, H), M_FLOAT_EPS);
    float NdotV = max(V.z, M_FLOAT_EPS);

    // VNDF PDF: D(H) * G1(V) * max(0, V·H) / NdotV
    float D = mx_ggx_NDF(H, alpha);
    float G1 = mx_ggx_smith_G1_aniso(NdotV, V.x, V.y, alpha.x, alpha.y);
    float vndf_pdf = D * G1 * VdotH / NdotV;

    // Transform to reflection direction: PDF_L = PDF_H / (4 * V·H)
    pdf = vndf_pdf / (4.0 * VdotH);

    return L;
}

float3 sample_preview_surface(
    VertexData vd,
    float3 V,
    float3 diffuseColor,
    float3 emissiveColor,
    int useSpecularWorkflow,
    float3 specularColor,
    float metallic,
    float roughness,
    float clearcoat,
    float clearcoatRoughness,
    float opacity,
    int opacityMode,
    float opacityThreshold,
    float ior,
    float3 normal,
    float displacement,
    float occlusion,
    uint eta_flipped,
    inout uint seed,
    out float pdf)
{
    // The hard cutout (opacity<0.25 → return -V) was removed: it overlapped
    // with the path-tracing main loop's stochastic pass-through. Rays reaching
    // here hit an existing surface, so sample the BSDF normally.
    // Create shading frame
    bool valid;
    ShadingFrame sf =
        ShadingFrame.createSafe(vd.normalWorld, float4(1, 0, 0, 1), valid);

    // Transform view direction to local space
    float3 V_local = sf.toLocal(V);
    float NdotV = clamp(V_local.z, M_FLOAT_EPS, 1.0);

    // Compute material properties
    float eta = eta_flipped != 0 ? (1.0 / ior) : ior;
    float fresnel = fresnel_dielectric(NdotV, eta);

    // Determine workflow
    bool isSpecularWorkflow = false;

    // Compute final diffuse and specular colors based on workflow
    float3 final_diffuse_color;
    float3 final_specular_color;
    float final_metallic;

    if (isSpecularWorkflow) {
        final_diffuse_color = diffuseColor;
        final_specular_color = specularColor;
        final_metallic = 0.0;  // Specular workflow doesn't use metallic
    }
    else {
        // Metallic workflow
        final_metallic = metallic;
        final_diffuse_color = diffuseColor * (1.0 - metallic);
        final_specular_color = lerp(float3(0.04), diffuseColor, metallic);
    }

    // Metal path weights
    float metal_weight = final_metallic;

    // Non-metal path weights
    float nonmetal_weight = 1.0 - final_metallic;
    float diffuse_weight = (1.0 - fresnel) * luminance(final_diffuse_color);
    float reflection_weight = fresnel;

    float total_nonmetal_weight = diffuse_weight + reflection_weight;
    if (total_nonmetal_weight > M_FLOAT_EPS) {
        diffuse_weight /= total_nonmetal_weight;
        reflection_weight /= total_nonmetal_weight;
    }

    // Sample component based on first level branching
    float component_sample = random_float(seed);
    float3 L_local;
    bool sampled_metal = (component_sample < metal_weight);

    if (sampled_metal) {
        // Sample metal reflection
        float2 u = random_float2(seed);
        L_local = sample_specular_reflection(u, V_local, roughness, pdf);

        if (pdf <= 0.0) {
            pdf = 0.0;
            return float3(0.0);
        }
    }
    else {
        // Sample non-metal component
        float nonmetal_sample =
            (component_sample - metal_weight) / nonmetal_weight;

        if (nonmetal_sample < diffuse_weight) {
            // Sample diffuse
            float2 u = random_float2(seed);
            L_local = sample_diffuse_lobe(u, pdf);
        }
        else {
            // Sample non-metal reflection
            float2 u = random_float2(seed);
            L_local = sample_specular_reflection(u, V_local, roughness, pdf);
        }

        if (pdf <= 0.0) {
            pdf = 0.0;
            return float3(0.0);
        }
    }

    // Calculate MIS PDF: both paths need to compute their probability for the
    // sampled direction
    float metal_pdf = 0.0;
    float nonmetal_pdf = 0.0;

    // Metal path PDF (only has reflection component)
    // Use consistent threshold with sampling: allow directions very close to
    // horizontal
    if (L_local.z > -M_FLOAT_EPS) {
        float3 H = normalize(V_local + L_local);
        float NdotH = max(H.z, M_FLOAT_EPS);
        float VdotH = max(dot(V_local, H), M_FLOAT_EPS);

        float2 alpha = float2(roughness * roughness, roughness * roughness);
        float D = mx_ggx_NDF(H, alpha);
        float G1 = mx_ggx_smith_G1_aniso(
            NdotV, V_local.x, V_local.y, alpha.x, alpha.y);
        float vndf_pdf = D * G1 * VdotH / NdotV;
        metal_pdf = vndf_pdf / (4.0 * VdotH);
    }

    // Non-metal path PDF (compute diffuse and reflection components)
    if (total_nonmetal_weight > M_FLOAT_EPS && L_local.z > -M_FLOAT_EPS) {
        // Diffuse PDF (only valid for directions above surface)
        float diffuse_pdf = max(L_local.z, 0.0) * M_1_PI;

        // Reflection PDF
        float3 H = normalize(V_local + L_local);
        float NdotH = max(H.z, M_FLOAT_EPS);
        float VdotH = max(dot(V_local, H), M_FLOAT_EPS);

        float2 alpha = float2(roughness * roughness, roughness * roughness);
        float D = mx_ggx_NDF(H, alpha);
        float G1 = mx_ggx_smith_G1_aniso(
            NdotV, V_local.x, V_local.y, alpha.x, alpha.y);
        float vndf_pdf = D * G1 * VdotH / NdotV;
        float reflection_pdf = vndf_pdf / (4.0 * VdotH);

        nonmetal_pdf =
            diffuse_pdf * diffuse_weight + reflection_pdf * reflection_weight;
    }

    // Final MIS PDF combining both paths
    pdf = metal_pdf * metal_weight + nonmetal_pdf * nonmetal_weight;
    pdf = max(pdf, M_FLOAT_EPS);

    // Transform to world space
    return sf.fromLocal(L_local);
}
