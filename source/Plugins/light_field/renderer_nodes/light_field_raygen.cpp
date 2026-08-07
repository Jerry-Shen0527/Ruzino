#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>

#include "GPUContext/compute_context.hpp"
#include "shaders/utils/ray.slang"

// Use the actual OpenUSD internal namespace
using namespace pxr;

// Comparison operators for GfVec2f
inline bool operator<(const GfVec2f& lhs, const GfVec2f& rhs)
{
    if (lhs[0] != rhs[0])
        return lhs[0] < rhs[0];
    return lhs[1] < rhs[1];
}

inline bool operator>(const GfVec2f& lhs, const GfVec2f& rhs)
{
    return rhs < lhs;
}

// Comparison operators for GfVec3f
inline bool operator<(const GfVec3f& lhs, const GfVec3f& rhs)
{
    if (lhs[0] != rhs[0])
        return lhs[0] < rhs[0];
    if (lhs[1] != rhs[1])
        return lhs[1] < rhs[1];
    return lhs[2] < rhs[2];
}

inline bool operator>(const GfVec3f& lhs, const GfVec3f& rhs)
{
    return rhs < lhs;
}

#include <algorithm>

#include "nodes/core/socket_trait.inl"
template<>
struct ValueTrait<pxr::GfVec3f> {
    static constexpr bool has_min = true;
    static constexpr bool has_max = true;
    static constexpr bool has_default = true;
};
template<>
struct ValueTrait<pxr::GfVec2f> {
    static constexpr bool has_min = true;
    static constexpr bool has_max = true;
    static constexpr bool has_default = true;
};

#include <pxr/base/gf/vec2f.h>

#include "hd_RUZINO/render_node_base.h"
#include "nodes/core/def/node_def.hpp"

NODE_DEF_OPEN_SCOPE
NODE_DECLARATION_FUNCTION(light_field_raygen)
{
    // Function content omitted	b.add_input<nvrhi::TextureHandle>("random
    // seeds");
    b.add_input<nvrhi::TextureHandle>("random seeds");
    b.add_input<float>("Display Screen Z").min(-50).max(50).default_val(-20);
    b.add_input<float>("View Distance")
        .min(0.1f)
        .max(100.0f)
        .default_val(20.0f);
    b.add_input<float>("Focal Distance").min(6.f).max(12.0f).default_val(8.0f);
    b.add_input<float>("Pupil Diameter").min(0.1f).max(10.0f).default_val(2.0f);
    b.add_input<float>("Lens to Display").min(6).max(10).default_val(9);
    b.add_input<int>("View Count").min(1).max(32).default_val(9);
    b.add_input<pxr::GfVec2f>("Lens scaling")
        .default_val({ 1, 1 })
        .min(pxr::GfVec2f(0.1f, 0.1f))
        .max(pxr::GfVec2f(1.0f, 1.0f));
    b.add_input<pxr::GfVec2f>("View Range")
        .default_val(pxr::GfVec2f(-10.0f, 10.0f))
        .min(pxr::GfVec2f(-50.0f, -50.0f))
        .max(pxr::GfVec2f(50.0f, 50.0f));
    b.add_input<pxr::GfVec2f>("Display Screen Size")
        .default_val(pxr::GfVec2f(20.0f, 20.0f))
        .min(pxr::GfVec2f(0.1f, 0.1f))
        .max(pxr::GfVec2f(100.0f, 100.0f));

    b.add_output<nvrhi::BufferHandle>("Pixel Target");
    b.add_output<nvrhi::BufferHandle>("Rays");
}

NODE_EXECUTION_FUNCTION(light_field_raygen)
{
    auto image_size = get_size(params);

    auto display_screen_z = params.get_input<float>("Display Screen Z");
    auto view_distance = params.get_input<float>("View Distance");
    auto display_screen_size =
        params.get_input<pxr::GfVec2f>("Display Screen Size");

    // Prepare the shader using reflection
    ProgramDesc cs_program_desc;
    cs_program_desc.shaderType = nvrhi::ShaderType::Compute;
    cs_program_desc
        .set_path(
            LIGHT_FIELD_SHADERS_DIR
            "/shaders/light_field_init_closest_lens.slang")
        .set_entry_name("main");

    ProgramHandle cs_program = resource_allocator.create(cs_program_desc);
    MARK_DESTROY_NVRHI_RESOURCE(cs_program);
    CHECK_PROGRAM_ERROR(cs_program);

    auto ray_buffer = create_buffer<RayInfo>(
        params, image_size[0] * image_size[1], false, true);

    auto pixel_target_buffer = create_buffer<GfVec2i>(
        params, image_size[0] * image_size[1], false, true);

    auto random_seeds = params.get_input<nvrhi::TextureHandle>("random seeds");
    // Create constants buffer
    struct Constants {
        uint32_t LaunchDimensions[2];
        float displaySize[2];
        float displayZ;
        float focalLength;
        float pupilDiameter;
        float viewDistance;
        uint32_t viewCount;
        float viewRange[2];
    };
    Constants constants;
    constants.LaunchDimensions[0] = image_size[0];
    constants.LaunchDimensions[1] = image_size[1];
    constants.displaySize[0] = display_screen_size[0];
    constants.displaySize[1] = display_screen_size[1];
    constants.displayZ = display_screen_z;
    constants.focalLength = params.get_input<float>("Focal Distance");
    constants.pupilDiameter = params.get_input<float>("Pupil Diameter");
    constants.viewDistance = view_distance;
    constants.viewCount = params.get_input<int>("View Count");
    auto view_range = params.get_input<pxr::GfVec2f>("View Range");
    constants.viewRange[0] = view_range[0];
    constants.viewRange[1] = view_range[1];

    auto constants_buffer = create_constant_buffer(params, constants);
    MARK_DESTROY_NVRHI_RESOURCE(constants_buffer);

    // Create lens positions buffer (example positions - should be provided as
    // input)
    std::vector<GfVec3f> lens_positions_data;

    // Create 26 lenses arranged horizontally with vertical offset pattern
    int total_lenses = 26;
    float total_horizontal_spacing = display_screen_size[0];
    float lens_spacing =
        total_horizontal_spacing / 26.0f;  // Divide into 27 intervals

    // Start position: left edge + half spacing
    float start_x = -display_screen_size[0] / 2.0f + lens_spacing * 0.5f;

    // Vertical spacing for the offset pattern
    float vertical_spacing =
        display_screen_size[1] / 4.0f;  // Divide into 5 intervals
    float start_y = -display_screen_size[1] / 2.0f + vertical_spacing * 0.5f;

    auto lens_scaling = params.get_input<pxr::GfVec2f>("Lens scaling");
    auto lens_to_display = params.get_input<float>("Lens to Display");

    for (int i = 0; i < total_lenses; i++) {
        float x = start_x + i * lens_spacing;
        float y = start_y + (i % 4) * vertical_spacing;

        lens_positions_data.push_back(GfVec3f(
            x * lens_scaling[0],
            y * lens_scaling[1],
            display_screen_z + lens_to_display));
    }

    auto lens_positions_buffer =
        create_buffer<GfVec3f>(params, lens_positions_data.size(), false, true);
    MARK_DESTROY_NVRHI_RESOURCE(lens_positions_buffer);

    ProgramVars program_vars(resource_allocator, cs_program);
    program_vars["rays"] = ray_buffer;
    program_vars["random_seeds"] = random_seeds;
    program_vars["pixel_targets"] = pixel_target_buffer;
    program_vars["lens_positions"] = lens_positions_buffer;
    program_vars["constants"] = constants_buffer;

    program_vars.finish_setting_vars();

    ComputeContext context(resource_allocator, program_vars);
    context.finish_setting_pso();

    context.begin();
    context.write_buffer(
        lens_positions_buffer,
        lens_positions_data.data(),
        lens_positions_data.size() * sizeof(GfVec3f));
    // context.uav_barrier(lens_positions_buffer);
    context.dispatch({}, program_vars, image_size[0], 8, image_size[1], 8);
    context.finish();

    params.set_output("Rays", ray_buffer);
    params.set_output("Pixel Target", pixel_target_buffer);
    return true;
}

NODE_DECLARATION_UI(light_field_raygen);
NODE_DEF_CLOSE_SCOPE
