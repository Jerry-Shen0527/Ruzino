
__exported import stdlib.genslang.mx_surfaceshader;
__exported import VertexData;

import pbrlib.genslang.mx_roughness_anisotropy;

import pbrlib.genslang.mx_sheen_bsdf;

import stdlib.genslang.mx_luminance_color3;

import stdlib.genslang.mx_rotate_vector3;

import pbrlib.genslang.mx_artistic_ior;

import pbrlib.genslang.mx_uniform_edf;

import pbrlib.genslang.mx_multiply_edf_color3;

import pbrlib.genslang.mx_dielectric_bsdf;

import pbrlib.genslang.mx_conductor_bsdf;

import pbrlib.genslang.mx_translucent_bsdf;

import pbrlib.genslang.mx_subsurface_bsdf;

import pbrlib.genslang.mx_oren_nayar_diffuse_bsdf;

import utils.Math.ShadingFrame;

void NG_convert_float_color3(VertexData vd, float in, out float3 out1)
{
    float3 combine_out = float3(in, in, in);
    out1 = combine_out;
}

import pbrlib.genslang.mx_mix_bsdf;

import pbrlib.genslang.mx_generalized_schlick_edf;

import pbrlib.genslang.mx_mix_edf;

import pbrlib.genslang.mx_layer_bsdf;

import pbrlib.genslang.mx_multiply_bsdf_color3;

bool is_close(float3 a, float3 b)
{
    const float EPSILON = 1e-5;
    return all(abs(a - b) < float3(EPSILON, EPSILON, EPSILON));
}

void NG_standard_surface_surfaceshader_100(
    VertexData vd,
    float3 L,
    float3 V,
    uint eta_flipped,
    float base,
    float3 base_color,
    float diffuse_roughness,
    float metalness,
    float specular,
    float3 specular_color,
    float specular_roughness,
    float specular_IOR,
    float specular_anisotropy,
    float specular_rotation,
    float transmission,
    float3 transmission_color,
    float transmission_depth,
    float3 transmission_scatter,
    float transmission_scatter_anisotropy,
    float transmission_dispersion,
    float transmission_extra_roughness,
    float subsurface,
    float3 subsurface_color,
    float3 subsurface_radius,
    float subsurface_scale,
    float subsurface_anisotropy,
    float sheen,
    float3 sheen_color,
    float sheen_roughness,
    float coat,
    float3 coat_color,
    float coat_roughness,
    float coat_anisotropy,
    float coat_rotation,
    float coat_IOR,
    float3 coat_normal,
    float coat_affect_color,
    float coat_affect_roughness,
    float thin_film_thickness,
    float thin_film_IOR,
    float emission,
    float3 emission_color,
    float3 opacity,
    bool thin_walled,
    float3 normal,
    float3 tangent,
    out surfaceshader out1)
{
    // The straight-through direction L = -V is a real transmission sample for
    // thin_walled shells. Let it fall through to the full BSDF eval (which
    // applies transmission_color tint) instead of short-circuiting to white.
    if (!thin_walled && is_close(normalize(L), normalize(-V))) {
        // For near-perfect mirror reflection, use Fresnel-weighted response
        // instead of plain white (which is wrong for colored conductors).
        if (metalness > 1e-5) {
            float3 metal_reflectivity_out = base_color * base;
            float3 metal_edgecolor_out = specular_color * specular;
            float3 artistic_ior_ior_fast = float3(0.0);
            float3 artistic_ior_extinction_fast = float3(0.0);
            mx_artistic_ior(
                metal_reflectivity_out,
                metal_edgecolor_out,
                artistic_ior_ior_fast,
                artistic_ior_extinction_fast);
            float3 F = mx_fresnel_conductor(
                1.0, artistic_ior_ior_fast, artistic_ior_extinction_fast);
            out1 = surfaceshader(F, float3(0.0));
        }
        else {
            out1 = surfaceshader(float3(1.0f), float3(1.0));
        }
        return;
    }
    // NOTE: the hard cutout (opacity.r<0.25 → fully transparent) was removed
    // because it overlapped with the path-tracing main loop's stochastic
    // pass-through. Rays reaching this BSDF eval have already been judged to
    // hit an existing surface. The final `color *= surfaceOpacity` multiply
    // is also removed below for the same reason (double attenuation).
    // thin_walled mirrors the sample-side gate: keep IOR un-flipped so the
    // thin-shell treats both faces symmetrically (no Snell medium flip).
    if (transmission > 0.0 && !thin_walled)
        if (eta_flipped > 0.0)
            specular_IOR = 1.0 / specular_IOR;
    float2 coat_roughness_vector_out = float2(0.0);
    mx_roughness_anisotropy(
        coat_roughness, coat_anisotropy, coat_roughness_vector_out);
    const float coat_tangent_rotate_degree_in2_tmp = 360.000000;
    float coat_tangent_rotate_degree_out =
        coat_rotation * coat_tangent_rotate_degree_in2_tmp;
    float3 metal_reflectivity_out = base_color * base;
    float3 metal_edgecolor_out = specular_color * specular;
    float coat_affect_roughness_multiply1_out = coat_affect_roughness * coat;
    const float tangent_rotate_degree_in2_tmp = 360.000000;
    float tangent_rotate_degree_out =
        specular_rotation * tangent_rotate_degree_in2_tmp;
    float transmission_roughness_add_out =
        specular_roughness + transmission_extra_roughness;
    const float subsurface_color_nonnegative_in2_tmp = 0.000000;
    float3 subsurface_color_nonnegative_out =
        max(subsurface_color, subsurface_color_nonnegative_in2_tmp);
    const float coat_clamped_low_tmp = 0.000000;
    const float coat_clamped_high_tmp = 1.000000;
    float coat_clamped_out =
        clamp(coat, coat_clamped_low_tmp, coat_clamped_high_tmp);
    float3 subsurface_radius_scaled_out = subsurface_radius * subsurface_scale;
    float subsurface_selector_out = float(thin_walled);
    const float base_color_nonnegative_in2_tmp = 0.000000;
    float3 base_color_nonnegative_out =
        max(base_color, base_color_nonnegative_in2_tmp);
    const float3 coat_attenuation_bg_tmp = float3(1.000000, 1.000000, 1.000000);
    float3 coat_attenuation_out =
        lerp(coat_attenuation_bg_tmp, coat_color, coat);
    const float one_minus_coat_ior_in1_tmp = 1.000000;
    float one_minus_coat_ior_out = one_minus_coat_ior_in1_tmp - coat_IOR;
    const float one_plus_coat_ior_in1_tmp = 1.000000;
    float one_plus_coat_ior_out = one_plus_coat_ior_in1_tmp + coat_IOR;
    float3 emission_weight_out = emission_color * emission;
    float3 opacity_luminance_out = float3(0.0);
    mx_luminance_color3(
        opacity, float3(0.272229, 0.674082, 0.053689), opacity_luminance_out);
    float3 coat_tangent_rotate_out = float3(0.0);
    mx_rotate_vector3(
        tangent,
        coat_tangent_rotate_degree_out,
        coat_normal,
        coat_tangent_rotate_out);
    float3 artistic_ior_ior = float3(0.0);
    float3 artistic_ior_extinction = float3(0.0);
    mx_artistic_ior(
        metal_reflectivity_out,
        metal_edgecolor_out,
        artistic_ior_ior,
        artistic_ior_extinction);
    float coat_affect_roughness_multiply2_out =
        coat_affect_roughness_multiply1_out * coat_roughness;
    float3 tangent_rotate_out = float3(0.0);
    mx_rotate_vector3(
        tangent, tangent_rotate_degree_out, normal, tangent_rotate_out);
    const float transmission_roughness_clamped_low_tmp = 0.000000;
    const float transmission_roughness_clamped_high_tmp = 1.000000;
    float transmission_roughness_clamped_out = clamp(
        transmission_roughness_add_out,
        transmission_roughness_clamped_low_tmp,
        transmission_roughness_clamped_high_tmp);
    float coat_gamma_multiply_out = coat_clamped_out * coat_affect_color;
    float coat_ior_to_F0_sqrt_out =
        one_minus_coat_ior_out / one_plus_coat_ior_out;
    const int opacity_luminance_float_index_tmp = 0;
    float opacity_luminance_float_out =
        opacity_luminance_out[opacity_luminance_float_index_tmp];
    float3 coat_tangent_rotate_normalize_out =
        normalize(coat_tangent_rotate_out);
    const float coat_affected_roughness_fg_tmp = 1.000000;
    float coat_affected_roughness_out = lerp(
        specular_roughness,
        coat_affected_roughness_fg_tmp,
        coat_affect_roughness_multiply2_out);
    float3 tangent_rotate_normalize_out = normalize(tangent_rotate_out);
    const float coat_affected_transmission_roughness_fg_tmp = 1.000000;
    float coat_affected_transmission_roughness_out = lerp(
        transmission_roughness_clamped_out,
        coat_affected_transmission_roughness_fg_tmp,
        coat_affect_roughness_multiply2_out);
    const float coat_gamma_in2_tmp = 1.000000;
    float coat_gamma_out = coat_gamma_multiply_out + coat_gamma_in2_tmp;
    float coat_ior_to_F0_out =
        coat_ior_to_F0_sqrt_out * coat_ior_to_F0_sqrt_out;
    const float coat_tangent_value2_tmp = 0.000000;
    float3 coat_tangent_out = (coat_anisotropy > coat_tangent_value2_tmp)
                                  ? coat_tangent_rotate_normalize_out
                                  : tangent;
    float2 main_roughness_out = float2(0.0);
    mx_roughness_anisotropy(
        coat_affected_roughness_out, specular_anisotropy, main_roughness_out);
    const float main_tangent_value2_tmp = 0.000000;
    float3 main_tangent_out = (specular_anisotropy > main_tangent_value2_tmp)
                                  ? tangent_rotate_normalize_out
                                  : tangent;
    float2 transmission_roughness_out = float2(0.0);
    mx_roughness_anisotropy(
        coat_affected_transmission_roughness_out,
        specular_anisotropy,
        transmission_roughness_out);
    float3 coat_affected_subsurface_color_out =
        pow(subsurface_color_nonnegative_out, float3(coat_gamma_out));
    float3 coat_affected_diffuse_color_out =
        pow(base_color_nonnegative_out, float3(coat_gamma_out));
    const float one_minus_coat_ior_to_F0_in1_tmp = 1.000000;
    float one_minus_coat_ior_to_F0_out =
        one_minus_coat_ior_to_F0_in1_tmp - coat_ior_to_F0_out;
    float3 emission_color0_out = float3(0.0);
    NG_convert_float_color3(
        vd, one_minus_coat_ior_to_F0_out, emission_color0_out);
    surfaceshader shader_constructor_out =
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

            // Skip coat layer if weight is negligible
            BSDF coat_bsdf_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            if (coat > 1e-5) {
                mx_dielectric_bsdf(
                    closureData,
                    coat,
                    float3(1.000000, 1.000000, 1.000000),
                    coat_IOR,
                    coat_roughness_vector_out,
                    0.000000,
                    1.500000,
                    coat_normal,
                    coat_tangent_out,
                    0,
                    0,
                    coat_bsdf_out);
            }

            BSDF metal_bsdf_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            if (metalness > 1e-5) {
                mx_conductor_bsdf(
                    closureData,
                    1.000000,
                    artistic_ior_ior,
                    artistic_ior_extinction,
                    main_roughness_out,
                    thin_film_thickness,
                    thin_film_IOR,
                    normal,
                    main_tangent_out,
                    0,
                    metal_bsdf_out);
            }
            BSDF specular_bsdf_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            if (specular > 1e-5 && metalness < 0.99999) {
                mx_dielectric_bsdf(
                    closureData,
                    specular,
                    specular_color,
                    specular_IOR,
                    main_roughness_out,
                    thin_film_thickness,
                    thin_film_IOR,
                    normal,
                    main_tangent_out,
                    0,
                    0,
                    specular_bsdf_out);
            }
            BSDF transmission_bsdf_out =
                BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_dielectric_bsdf(
                closureData,
                1.000000,
                transmission_color,
                specular_IOR,
                transmission_roughness_out,
                0.000000,
                1.500000,
                normal,
                main_tangent_out,
                0,
                1,
                transmission_bsdf_out);
            BSDF sheen_bsdf_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_sheen_bsdf(
                closureData,
                sheen,
                sheen_color,
                sheen_roughness,
                normal,
                0,
                sheen_bsdf_out);
            BSDF translucent_bsdf_out =
                BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_translucent_bsdf(
                closureData,
                1.000000,
                coat_affected_subsurface_color_out,
                normal,
                translucent_bsdf_out);
            BSDF subsurface_bsdf_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_subsurface_bsdf(
                closureData,
                1.000000,
                coat_affected_subsurface_color_out,
                subsurface_radius_scaled_out,
                subsurface_anisotropy,
                normal,
                subsurface_bsdf_out);
            BSDF selected_subsurface_bsdf_out =
                BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_mix_bsdf(
                closureData,
                translucent_bsdf_out,
                subsurface_bsdf_out,
                subsurface_selector_out,
                selected_subsurface_bsdf_out);
            BSDF diffuse_bsdf_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_oren_nayar_diffuse_bsdf(
                closureData,
                base,
                coat_affected_diffuse_color_out,
                diffuse_roughness,
                normal,
                false,
                diffuse_bsdf_out);
            BSDF subsurface_mix_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_mix_bsdf(
                closureData,
                selected_subsurface_bsdf_out,
                diffuse_bsdf_out,
                subsurface,
                subsurface_mix_out);
            BSDF sheen_layer_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_layer_bsdf(
                closureData,
                sheen_bsdf_out,
                subsurface_mix_out,
                sheen_layer_out);
            BSDF transmission_mix_out =
                BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_mix_bsdf(
                closureData,
                transmission_bsdf_out,
                sheen_layer_out,
                transmission,
                transmission_mix_out);
            BSDF specular_layer_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_layer_bsdf(
                closureData,
                specular_bsdf_out,
                transmission_mix_out,
                specular_layer_out);
            BSDF metalness_mix_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_mix_bsdf(
                closureData,
                metal_bsdf_out,
                specular_layer_out,
                metalness,
                metalness_mix_out);
            BSDF thin_film_layer_attenuated_out =
                BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_multiply_bsdf_color3(
                closureData,
                metalness_mix_out,
                coat_attenuation_out,
                thin_film_layer_attenuated_out);
            BSDF coat_layer_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_layer_bsdf(
                closureData,
                coat_bsdf_out,
                thin_film_layer_attenuated_out,
                coat_layer_out);

            // Accumulate the light's contribution
            shader_constructor_out.color += coat_layer_out.response;
        }

        // Calculate the BSDF transmission for viewing direction
        // Skip transmission calculation if transmission weight is negligible
        if (transmission > 1e-5) {
            ClosureData closureData =
                ClosureData(CLOSURE_TYPE_TRANSMISSION, L, V, N, P, occlusion);
            BSDF coat_bsdf_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            if (coat > 1e-5) {
                mx_dielectric_bsdf(
                    closureData,
                    coat,
                    float3(1.000000, 1.000000, 1.000000),
                    coat_IOR,
                    coat_roughness_vector_out,
                    0.000000,
                    1.500000,
                    coat_normal,
                    coat_tangent_out,
                    0,
                    0,
                    coat_bsdf_out);
            }
            BSDF metal_bsdf_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            if (metalness > 1e-5) {
                mx_conductor_bsdf(
                    closureData,
                    1.000000,
                    artistic_ior_ior,
                    artistic_ior_extinction,
                    main_roughness_out,
                    thin_film_thickness,
                    thin_film_IOR,
                    normal,
                    main_tangent_out,
                    0,
                    metal_bsdf_out);
            }
            BSDF specular_bsdf_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            if (specular > 1e-5 && metalness < 0.99999) {
                mx_dielectric_bsdf(
                    closureData,
                    specular,
                    specular_color,
                    specular_IOR,
                    main_roughness_out,
                    thin_film_thickness,
                    thin_film_IOR,
                    normal,
                    main_tangent_out,
                    0,
                    0,
                    specular_bsdf_out);
            }
            BSDF transmission_bsdf_out =
                BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_dielectric_bsdf(
                closureData,
                1.000000,
                transmission_color,
                specular_IOR,
                transmission_roughness_out,
                0.000000,
                1.500000,
                normal,
                main_tangent_out,
                0,
                1,
                transmission_bsdf_out);
            BSDF sheen_bsdf_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_sheen_bsdf(
                closureData,
                sheen,
                sheen_color,
                sheen_roughness,
                normal,
                0,
                sheen_bsdf_out);
            BSDF translucent_bsdf_out =
                BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_translucent_bsdf(
                closureData,
                1.000000,
                coat_affected_subsurface_color_out,
                normal,
                translucent_bsdf_out);
            BSDF subsurface_bsdf_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_subsurface_bsdf(
                closureData,
                1.000000,
                coat_affected_subsurface_color_out,
                subsurface_radius_scaled_out,
                subsurface_anisotropy,
                normal,
                subsurface_bsdf_out);
            BSDF selected_subsurface_bsdf_out =
                BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_mix_bsdf(
                closureData,
                translucent_bsdf_out,
                subsurface_bsdf_out,
                subsurface_selector_out,
                selected_subsurface_bsdf_out);
            BSDF diffuse_bsdf_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_oren_nayar_diffuse_bsdf(
                closureData,
                base,
                coat_affected_diffuse_color_out,
                diffuse_roughness,
                normal,
                false,
                diffuse_bsdf_out);
            BSDF subsurface_mix_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_mix_bsdf(
                closureData,
                selected_subsurface_bsdf_out,
                diffuse_bsdf_out,
                subsurface,
                subsurface_mix_out);
            BSDF sheen_layer_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_layer_bsdf(
                closureData,
                sheen_bsdf_out,
                subsurface_mix_out,
                sheen_layer_out);
            BSDF transmission_mix_out =
                BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_mix_bsdf(
                closureData,
                transmission_bsdf_out,
                sheen_layer_out,
                transmission,
                transmission_mix_out);
            BSDF specular_layer_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_layer_bsdf(
                closureData,
                specular_bsdf_out,
                transmission_mix_out,
                specular_layer_out);
            BSDF metalness_mix_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_mix_bsdf(
                closureData,
                metal_bsdf_out,
                specular_layer_out,
                metalness,
                metalness_mix_out);
            BSDF thin_film_layer_attenuated_out =
                BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_multiply_bsdf_color3(
                closureData,
                metalness_mix_out,
                coat_attenuation_out,
                thin_film_layer_attenuated_out);
            BSDF coat_layer_out = BSDF(float3(0.0), float3(1.0), 0.0, 0.0);
            mx_layer_bsdf(
                closureData,
                coat_bsdf_out,
                thin_film_layer_attenuated_out,
                coat_layer_out);
            shader_constructor_out.color += coat_layer_out.response;
        }  // End transmission calculation

        // The former `color *= surfaceOpacity` multiply is removed: it
        // double-attenuated energy the path-tracing main loop already handles
        // stochastically. surfaceOpacity (luminance of the float3 opacity) is
        // now unused here; opacity presence is resolved before BSDF eval.
    }

    out1 = shader_constructor_out;
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

// Exact Fresnel reflectance
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

// Context holding the frame + weight setup shared between sampling and pdf
// evaluation of the layered standard-surface BSDF. Built once per shading
// point; sample_standard_surface fills it implicitly via locals, the pdf
// helper consumes it. Keeping it in a struct avoids a ~15-arg helper signature
// and guarantees the pdf math stays identical to the sampling distribution.
struct LayeredBsdfContext {
    ShadingFrame sf;
    ShadingFrame coat_sf;
    float3 V_local;
    float3 V_coat_local;
    float NdotV;
    float coat_NdotV;
    float2 alpha;
    float2 coat_alpha;
    float eta;
    float coat_sample_weight;
    float base_sample_weight;
    float metal_weight;
    float nonmetal_weight;
    float diffuse_weight;
    float reflection_weight;
    float transmission_weight;
    float total_nonmetal_weight;
};

// Evaluate the layered BSDF sampling pdf for a query direction L_local (in the
// base-layer local frame). This is the "Calculate MIS PDF" block factored out
// of sample_standard_surface so the eval callable can also obtain bsdf_pdf(L)
// for the NEE direction. Returns 0 for directions no layer can generate.
float _layered_pdf(LayeredBsdfContext ctx, float3 L_local)
{
    float coat_pdf = 0.0;
    float base_pdf = 0.0;

    // Coat PDF (computed in coat's coordinate frame)
    float3 L_world = ctx.sf.fromLocal(L_local);
    float3 L_coat_local = ctx.coat_sf.toLocal(L_world);
    if (L_coat_local.z > -M_FLOAT_EPS) {
        float3 H_coat = normalize(ctx.V_coat_local + L_coat_local);
        float VdotH_coat = max(dot(ctx.V_coat_local, H_coat), M_FLOAT_EPS);
        float D = mx_ggx_NDF(H_coat, ctx.coat_alpha);
        float G1 = mx_ggx_smith_G1_aniso(
            ctx.coat_NdotV, ctx.V_coat_local.x, ctx.V_coat_local.y,
            ctx.coat_alpha.x, ctx.coat_alpha.y);
        float vndf_pdf = D * G1 * VdotH_coat / ctx.coat_NdotV;
        coat_pdf = vndf_pdf / (4.0 * VdotH_coat);
    }

    float metal_pdf = 0.0;
    float nonmetal_pdf = 0.0;

    // Metal path PDF (reflection only)
    if (L_local.z > -M_FLOAT_EPS) {
        float3 H = normalize(ctx.V_local + L_local);
        float VdotH = max(dot(ctx.V_local, H), M_FLOAT_EPS);
        float D = mx_ggx_NDF(H, ctx.alpha);
        float G1 = mx_ggx_smith_G1_aniso(
            ctx.NdotV, ctx.V_local.x, ctx.V_local.y, ctx.alpha.x, ctx.alpha.y);
        float vndf_pdf = D * G1 * VdotH / ctx.NdotV;
        metal_pdf = vndf_pdf / (4.0 * VdotH);
    }

    if (ctx.total_nonmetal_weight > M_FLOAT_EPS) {
        float diffuse_pdf = 0.0;
        float reflection_pdf = 0.0;
        float transmission_pdf = 0.0;

        if (L_local.z > -M_FLOAT_EPS) {
            diffuse_pdf = max(L_local.z, 0.0) * M_1_PI;
            float3 H = normalize(ctx.V_local + L_local);
            float VdotH = max(dot(ctx.V_local, H), M_FLOAT_EPS);
            float D = mx_ggx_NDF(H, ctx.alpha);
            float G1 = mx_ggx_smith_G1_aniso(
                ctx.NdotV, ctx.V_local.x, ctx.V_local.y, ctx.alpha.x,
                ctx.alpha.y);
            float vndf_pdf = D * G1 * VdotH / ctx.NdotV;
            reflection_pdf = vndf_pdf / (4.0 * VdotH);
        }
        else if (L_local.z < -M_FLOAT_EPS) {
            float3 H = normalize(ctx.V_local + L_local * ctx.eta);
            if (H.z < 0.0) H = -H;
            float VdotH = max(dot(ctx.V_local, H), M_FLOAT_EPS);
            float LdotH = max(abs(dot(L_local, H)), M_FLOAT_EPS);
            float D = mx_ggx_NDF(H, ctx.alpha);
            float G1 = mx_ggx_smith_G1_aniso(
                ctx.NdotV, ctx.V_local.x, ctx.V_local.y, ctx.alpha.x,
                ctx.alpha.y);
            float vndf_pdf = D * G1 * VdotH / ctx.NdotV;
            float denom = VdotH + LdotH / ctx.eta;
            float jacobian = (ctx.eta * ctx.eta * LdotH) / (denom * denom);
            transmission_pdf = vndf_pdf * jacobian / (4.0 * VdotH);
        }

        nonmetal_pdf = diffuse_pdf * ctx.diffuse_weight +
                       reflection_pdf * ctx.reflection_weight +
                       transmission_pdf * ctx.transmission_weight;
    }

    base_pdf = metal_pdf * ctx.metal_weight + nonmetal_pdf * ctx.nonmetal_weight;

    float pdf = coat_pdf * ctx.coat_sample_weight +
                base_pdf * ctx.base_sample_weight;
    return max(pdf, M_FLOAT_EPS);
}

// Build the layered-BSDF context (frames + weights) shared by sampling and
// pdf evaluation. Factored out of sample_standard_surface so the eval callable
// can obtain the identical context for bsdf_pdf(L). Adjusts specular_IOR for
// solid-transmission eta flipping (caller passes the already-flipped value).
LayeredBsdfContext build_layered_bsdf_context(
    float3 V,
    float3 normal,
    float3 tangent,
    float specular_roughness,
    float specular_anisotropy,
    float specular_rotation,
    float specular_IOR,
    float coat,
    float3 coat_normal,
    float coat_roughness,
    float coat_anisotropy,
    float coat_rotation,
    float coat_IOR,
    float base,
    float3 base_color,
    float metalness,
    float specular,
    float transmission,
    float3 transmission_color)
{
    float3 shading_normal = normal;
    float3 shading_tangent = tangent;
    if (dot(V, normal) < 0.0) {
        shading_normal = -normal;
        shading_tangent = -tangent;
    }

    float3 coat_shading_normal = coat_normal;
    float3 coat_shading_tangent = tangent;
    if (dot(V, coat_normal) < 0.0) {
        coat_shading_normal = -coat_normal;
        coat_shading_tangent = -tangent;
    }

    float3 rotated_tangent;
    mx_rotate_vector3(
        shading_tangent, specular_rotation * 360.0, shading_normal,
        rotated_tangent);
    bool valid;
    ShadingFrame sf = ShadingFrame.createSafe(
        shading_normal, float4(rotated_tangent, 1.0), valid);
    float3 V_local = sf.toLocal(V);
    float NdotV = clamp(V_local.z, M_FLOAT_EPS, 1.0);

    float2 alpha;
    mx_roughness_anisotropy(specular_roughness, specular_anisotropy, alpha);

    float3 coat_rotated_tangent;
    mx_rotate_vector3(
        coat_shading_tangent, coat_rotation * 360.0, coat_shading_normal,
        coat_rotated_tangent);
    coat_rotated_tangent = normalize(coat_rotated_tangent);
    bool coat_valid;
    ShadingFrame coat_sf = ShadingFrame.createSafe(
        coat_shading_normal, float4(coat_rotated_tangent, 1.0), coat_valid);
    float3 V_coat_local = coat_sf.toLocal(V);
    float coat_NdotV = clamp(V_coat_local.z, M_FLOAT_EPS, 1.0);

    float coat_weight = clamp(coat, 0.0, 1.0);
    float2 coat_alpha;
    mx_roughness_anisotropy(coat_roughness, coat_anisotropy, coat_alpha);
    float coat_fresnel = fresnel_dielectric(coat_NdotV, coat_IOR);

    float eta = specular_IOR;
    float base_fresnel = fresnel_dielectric(NdotV, eta);

    float coat_reflection_prob = coat_weight * coat_fresnel;
    float coat_sample_weight = coat_reflection_prob;
    float base_sample_weight = 1.0 - coat_sample_weight;

    float metal_weight = metalness;
    float nonmetal_weight = 1.0 - metalness;
    float diffuse_weight = base * (1.0 - base_fresnel) * luminance(base_color) *
                           (1 - transmission);
    float reflection_weight = base_fresnel * specular;
    float transmission_weight =
        (1.0 - base_fresnel) * transmission * luminance(transmission_color);

    float total_nonmetal_weight =
        diffuse_weight + reflection_weight + transmission_weight;
    if (total_nonmetal_weight > M_FLOAT_EPS) {
        diffuse_weight /= total_nonmetal_weight;
        reflection_weight /= total_nonmetal_weight;
        transmission_weight /= total_nonmetal_weight;
    }

    LayeredBsdfContext ctx;
    ctx.sf = sf;
    ctx.coat_sf = coat_sf;
    ctx.V_local = V_local;
    ctx.V_coat_local = V_coat_local;
    ctx.NdotV = NdotV;
    ctx.coat_NdotV = coat_NdotV;
    ctx.alpha = alpha;
    ctx.coat_alpha = coat_alpha;
    ctx.eta = eta;
    ctx.coat_sample_weight = coat_sample_weight;
    ctx.base_sample_weight = base_sample_weight;
    ctx.metal_weight = metal_weight;
    ctx.nonmetal_weight = nonmetal_weight;
    ctx.diffuse_weight = diffuse_weight;
    ctx.reflection_weight = reflection_weight;
    ctx.transmission_weight = transmission_weight;
    ctx.total_nonmetal_weight = total_nonmetal_weight;
    return ctx;
}

// Sample diffuse component using cosine hemisphere sampling
float3 sample_diffuse_lobe(float2 u, out float pdf)
{
    float3 L = sample_cosine_hemisphere_concentric(u, pdf);
    return L;
}

// Sample specular reflection using GGX distribution
float3
sample_specular_reflection(float2 u, float3 V, float2 alpha, out float pdf)
{
    // Sample microfacet normal using GGX VNDF distribution
    // VNDF already ensures H is in the correct hemisphere for V
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

// Sample transmission using GGX distribution and Snell's law
float3
sample_transmission(float2 u, float3 V, float2 alpha, float eta, out float pdf)
{
    // Sample microfacet normal using GGX VNDF distribution
    float3 H = mx_ggx_importance_sample_VNDF(u, V, alpha);

    // For transmission, ensure half vector points toward the incident side
    // This is necessary because refraction uses a different half-vector
    // convention
    if (dot(V, H) < 0.0) {
        H = -H;
    }

    float VdotH = max(dot(V, H), M_FLOAT_EPS);
    float NdotV = max(V.z, M_FLOAT_EPS);

    // Check for total internal reflection
    float discriminant = 1.0 - (1.0 - VdotH * VdotH) / (eta * eta);
    if (discriminant < 0.0) {
        pdf = M_FLOAT_EPS;
        return reflect(-V, H);  // Return reflection direction if TIR occurs
    }

    // Compute refracted direction using Snell's law
    float sqrt_discriminant = sqrt(discriminant);
    float3 L = -V / eta + H * (VdotH / eta - sqrt_discriminant);

    // Check if transmission is valid (below surface)
    if (L.z >= 0.0) {
        pdf = 0.0;
        return float3(0.0);
    }

    float LdotH = max(abs(dot(L, H)), M_FLOAT_EPS);

    // Compute PDF using GGX VNDF for transmission
    float D = mx_ggx_NDF(H, alpha);
    float G1 = mx_ggx_smith_G1_aniso(NdotV, V.x, V.y, alpha.x, alpha.y);
    float vndf_pdf = D * G1 * VdotH / NdotV;

    // Transform to transmission direction with Jacobian
    float denom = VdotH + LdotH / eta;
    float jacobian = (eta * eta * LdotH) / (denom * denom);
    pdf = vndf_pdf * jacobian;

    return L;
}

float3 sample_standard_surface(
    VertexData vd,
    float3 V,
    float base,
    float3 base_color,
    float diffuse_roughness,
    float metalness,
    float specular,
    float3 specular_color,
    float specular_roughness,
    float specular_IOR,
    float specular_anisotropy,
    float specular_rotation,
    float transmission,
    float3 transmission_color,
    float transmission_depth,
    float3 transmission_scatter,
    float transmission_scatter_anisotropy,
    float transmission_dispersion,
    float transmission_extra_roughness,
    float subsurface,
    float3 subsurface_color,
    float3 subsurface_radius,
    float subsurface_scale,
    float subsurface_anisotropy,
    float sheen,
    float3 sheen_color,
    float sheen_roughness,
    float coat,
    float3 coat_color,
    float coat_roughness,
    float coat_anisotropy,
    float coat_rotation,
    float coat_IOR,
    float3 coat_normal,
    float coat_affect_color,
    float coat_affect_roughness,
    float thin_film_thickness,
    float thin_film_IOR,
    float emission,
    float3 emission_color,
    float3 opacity,
    bool thin_walled,
    float3 normal,
    float3 tangent,
    uint eta_flipped,  // 0 for normal, 1 for flipped
    inout uint seed,
    out float pdf)
{
    // thin_walled: single-sided open shell — no Snell refraction. Keep IOR
    // un-flipped so both faces of the infinitesimally thin wall are treated
    // symmetrically (the ray enters and exits the medium at the same point).
    // Solid (thin_walled=false) keeps the eta_flipped logic for paired
    // refraction across a closed volume.
    if (transmission > 0.0 && !thin_walled)
        if (eta_flipped > 0.0)
            specular_IOR = 1.0 / specular_IOR;

    // The hard cutout (opacity.r<0.25 → return -V) was removed: it overlapped
    // with the path-tracing main loop's stochastic pass-through. Rays reaching
    // here hit an existing surface, so sample the BSDF normally.
    // Build the shared layered-BSDF context (frames + weights) once; the pdf
    // helper _layered_pdf consumes the identical ctx, so sampling and
    // bsdf_pdf(L) stay consistent.
    LayeredBsdfContext ctx = build_layered_bsdf_context(
        V, normal, tangent, specular_roughness, specular_anisotropy,
        specular_rotation, specular_IOR, coat, coat_normal, coat_roughness,
        coat_anisotropy, coat_rotation, coat_IOR, base, base_color, metalness,
        specular, transmission, transmission_color);
    // Local aliases so the sampling block below reads unchanged.
    ShadingFrame sf = ctx.sf;
    ShadingFrame coat_sf = ctx.coat_sf;
    float3 V_local = ctx.V_local;
    float3 V_coat_local = ctx.V_coat_local;
    float2 alpha = ctx.alpha;
    float2 coat_alpha = ctx.coat_alpha;
    float eta = ctx.eta;
    float coat_sample_weight = ctx.coat_sample_weight;
    float base_sample_weight = ctx.base_sample_weight;
    float metal_weight = ctx.metal_weight;
    float nonmetal_weight = ctx.nonmetal_weight;
    float diffuse_weight = ctx.diffuse_weight;
    float reflection_weight = ctx.reflection_weight;

    // Sample component: coat vs base
    float component_sample = random_float(seed);
    float3 L_local;
    bool sampled_coat = (component_sample < coat_sample_weight);

    if (sampled_coat) {
        // Sample coat layer in its own coordinate frame
        float2 u = random_float2(seed);
        float3 L_coat_local =
            sample_specular_reflection(u, V_coat_local, coat_alpha, pdf);

        if (pdf <= 0.0) {
            pdf = 0.0;
            return float3(0.0);
        }

        // Transform from coat frame to world, then to base frame
        float3 L_world = coat_sf.fromLocal(L_coat_local);
        L_local = sf.toLocal(L_world);
    }
    else {
        // Sample base layer
        float base_sample =
            (component_sample - coat_sample_weight) / base_sample_weight;
        bool sampled_metal = (base_sample < metal_weight);

        if (sampled_metal) {
            // Sample metal reflection
            float2 u = random_float2(seed);
            L_local = sample_specular_reflection(u, V_local, alpha, pdf);

            if (pdf <= 0.0) {
                pdf = 0.0;
                return float3(0.0);
            }
        }
        else {
            // Sample non-metal component
            float nonmetal_sample =
                (base_sample - metal_weight) / nonmetal_weight;

            if (nonmetal_sample < diffuse_weight) {
                // Sample diffuse
                float2 u = random_float2(seed);
                L_local = sample_diffuse_lobe(u, pdf);
            }
            else if (nonmetal_sample < diffuse_weight + reflection_weight) {
                // Sample non-metal reflection
                float2 u = random_float2(seed);
                L_local = sample_specular_reflection(u, V_local, alpha, pdf);
            }
            else if (thin_walled) {
                // Thin-shell straight-through transmission (no Snell bend).
                // The infinitely-thin wall enters & exits the medium at the
                // same point, so the net direction is unchanged: L = -V.
                // The eval side still applies Fresnel + transmission_color
                // tint via mx_dielectric_bsdf, giving the colored-but-flat
                // look of e.g. a soap bubble or leaf. pdf is near-Dirac; the
                // throughput comes from the eval-side BSDF response.
                L_local = -V_local;
                pdf = 1.0;
            }
            else {
                // Solid: refract via Snell's law (paired entry/exit faces)
                float2 u = random_float2(seed);
                L_local = sample_transmission(u, V_local, alpha, eta, pdf);
            }

            if (pdf <= 0.0) {
                pdf = 0.0;
                return float3(0.0);
            }
        }
    }

    // Calculate MIS PDF for the sampled direction via the shared helper (same
    // math as the inline block that used to live here, now also reused by the
    // eval callable to get bsdf_pdf(L) for the NEE direction).
    pdf = _layered_pdf(ctx, L_local);

    // Transform to world space
    return sf.fromLocal(L_local);
}

