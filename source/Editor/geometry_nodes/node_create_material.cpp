#ifdef GEOM_USD_EXTENSION

#include <filesystem>

#include "GCore/MaterialObject.h"
#include "GCore/Texture/TextureObject.h"
#include "geom_node_base.h"
#include "nodes/core/math/vec.hpp"
#include "spdlog/spdlog.h"
NODE_DEF_OPEN_SCOPE
NODE_DECLARATION_FUNCTION(create_material)
{
    // In-graph computed texture (storage deferred to USD-write time).
    // Typed as TextureHandle so existing texture producers (load_texture_2d,
    // noise, function textures) connect directly; carried internally as the
    // TextureObject aggregate (CPU pixels + optional GPU handle).
    b.add_input<TextureHandle>("Texture").optional(true);
    b.add_input<std::string>("Texture Name")
        .default_val("")  // file path fallback
        .optional(true);
    // glm::vec3 has no ValueTrait default — the code default below applies
    // when the socket is unwired.
    b.add_input<glm::vec3>("Base Color").optional(true);
    b.add_input<bool>("Alpha Cutout").default_val(false);
    b.add_input<float>("Opacity Threshold")
        .min(0.0f)
        .max(1.0f)
        .default_val(0.5f);
    b.add_input<float>("Roughness").min(0.0f).max(1.0f).default_val(0.5f);
    b.add_input<std::string>("Wrap Mode")
        .default_val("clamp");  // mirror | clamp | periodic | black
    b.add_output<MaterialObject>("Material");
}

NODE_EXECUTION_FUNCTION(create_material)
{
    // Optional object sockets: unconnected slots carry a NULL meta_any —
    // guard with has_input() before dereferencing.
    glm::vec3 base_color{ 0.5f, 0.5f, 0.5f };
    if (params.has_input("Base Color")) {
        entt::meta_any color_any =
            params.get_input<entt::meta_any>("Base Color");
        if (static_cast<bool>(color_any)) {
            if (auto* c = color_any.try_cast<glm::vec3>()) {
                base_color = *c;
            }
            else if (auto* v = color_any.try_cast<Ruzino::Vec3f>()) {
                // Python tuples arrive as the node system's Vec3f
                base_color = glm::vec3((*v)[0], (*v)[1], (*v)[2]);
            }
        }
    }
    TextureObject texture_object;
    if (params.has_input("Texture")) {
        entt::meta_any texture_any =
            params.get_input<entt::meta_any>("Texture");
        if (static_cast<bool>(texture_any)) {
            if (auto* handle = texture_any.try_cast<TextureHandle>()) {
                // Wrap the in-graph texture into the aggregate: CPU pixels
                // now, GPU handle slot available for renderer-side interop.
                texture_object.cpu = *handle;
            }
        }
    }
    auto texture = params.get_input<std::string>("Texture Name");
    auto alpha_cutout = params.get_input<bool>("Alpha Cutout");
    auto opacity_threshold = params.get_input<float>("Opacity Threshold");
    auto roughness = params.get_input<float>("Roughness");
    auto wrap_mode = params.get_input<std::string>("Wrap Mode");

    // Precedence: in-graph texture object > file path > flat color.
    const bool from_object = !texture_object.empty();
    const bool textured = from_object || !texture.empty();
    std::string resolved;
    if (!from_object && textured) {
        std::filesystem::path executable_path;

#ifdef _WIN32
        char p[MAX_PATH];
        GetModuleFileNameA(NULL, p, MAX_PATH);
        executable_path = std::filesystem::path(p).parent_path();
#else
        char p[PATH_MAX];
        ssize_t count = readlink("/proc/self/exe", p, PATH_MAX);
        if (count == -1) {
            throw std::runtime_error("Failed to get executable path.");
        }
        executable_path = std::filesystem::path(p).parent_path();
#endif

        // Expand the texture name to an absolute path (exe-relative like
        // set_texture), so saved graphs keep working across machines only
        // when the texture ships with the app; prefer absolute paths
        // otherwise.
        std::filesystem::path texture_path(texture);
        if (!texture_path.is_absolute())
            texture_path = executable_path / texture_path;
        texture_path = texture_path.lexically_normal();
        if (!std::filesystem::exists(texture_path)) {
            spdlog::warn(
                "[create_material] Texture file does not exist: {}",
                texture_path.string());
            return false;
        }
        if (std::filesystem::is_directory(texture_path)) {
            spdlog::warn(
                "[create_material] Path is a directory: {}",
                texture_path.string());
            return false;
        }
        resolved = texture_path.string();
    }

    MaterialObject material;
    material.textured = textured;
    material.base_color = base_color;
    material.texture_object = texture_object;
    if (textured && !from_object) {
        material.textures.push_back(resolved);
    }
    if (textured && from_object && !texture_object.has_cpu()) {
        spdlog::warn(
            "[create_material] GPU-only texture (no CPU side): USD write "
            "cannot materialize it and will fall back to Base Color");
    }
    material.alpha_cutout = alpha_cutout;
    material.opacity_threshold = opacity_threshold;
    material.roughness = roughness;
    material.wrap_mode = wrap_mode;

    params.set_output("Material", std::move(material));
    return true;
}

NODE_DECLARATION_UI(create_material);
NODE_DEF_CLOSE_SCOPE

#endif
