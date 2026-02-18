#ifdef GEOM_USD_EXTENSION

#include <filesystem>

#include "GCore/Components/MaterialComponent.h"
#include "geom_node_base.h"
#include "spdlog/spdlog.h"
NODE_DEF_OPEN_SCOPE
NODE_DECLARATION_FUNCTION(set_texture)
{
    b.add_input<Geometry>("Geometry");
    b.add_input<std::string>("Texture Name").default_val("");
    b.add_output<Geometry>("Geometry");
}

NODE_EXECUTION_FUNCTION(set_texture)
{
    auto texture = params.get_input<std::string>("Texture Name");

    auto geometry = params.get_input<Geometry>("Geometry");
    auto material = geometry.get_component<MaterialComponent>();

    std::filesystem::path executable_path;

#ifdef _WIN32
    char p[MAX_PATH];
    GetModuleFileNameA(NULL, p, MAX_PATH);
    executable_path = std::filesystem::path(p).parent_path();
#else
    char p[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", p, PATH_MAX);
    if (count != -1) {
        p[count] = '\0';
        executable_path = std::filesystem::path(p).parent_path();
    }
    else {
        throw std::runtime_error("Failed to get executable path.");
    }
#endif

    if (texture.empty()) {
        spdlog::warn("Path cannot be empty!");
        return false;
    }  // no path input
    // expand the texture name to abs path
    std::filesystem::path texture_path(texture);
    if (!texture_path.is_absolute())
        texture_path = executable_path / texture_path;
    spdlog::info("Exec path {}", executable_path.string());
    texture_path = texture_path.lexically_normal();
    spdlog::info("Normalized texture path: {}", texture_path.string());
    if (!std::filesystem::exists(texture_path)) {
        spdlog::warn("File not exists!");
        return false;
    }
    if (std::filesystem::is_directory(texture_path)) {
        spdlog::warn("This is a directory!");
        return false;
    }

    texture = texture_path.string();
    spdlog::info("Texture file: {}", texture.c_str());

    if (!material) {
        material = std::make_shared<MaterialComponent>(&geometry);
    }
    material->textures.clear();

    material->textures.push_back(texture);
    geometry.attach_component(material);

    params.set_output("Geometry", std::move(geometry));
    return true;
}

NODE_DECLARATION_UI(set_texture);
NODE_DEF_CLOSE_SCOPE

#endif
