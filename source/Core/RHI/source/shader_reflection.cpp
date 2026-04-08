#include "RHI/ShaderFactory/shader_reflection.hpp"

#include <spdlog/spdlog.h>

#include "RHI/ShaderFactory/shader.hpp"

RUZINO_NAMESPACE_OPEN_SCOPE

const nvrhi::BindingLayoutDescVector&
ShaderReflectionInfo::get_binding_layout_descs() const
{
    return binding_spaces;
}

unsigned ShaderReflectionInfo::get_binding_space(std::string_view path)
{
    std::string normalized_path = normalize_path(path);
    std::string_view base_name = get_base_name_view(normalized_path);

    // Try base name lookup
    auto it = binding_locations.find(std::string(base_name));
    if (it != binding_locations.end()) {
        return std::get<0>(it->second);
    }

    // spdlog::warn("Binding space not found: {} (base: {})", path, base_name);
    return -1;
}

unsigned ShaderReflectionInfo::get_binding_location(std::string_view path)
{
    std::string normalized_path = normalize_path(path);
    std::string_view base_name = get_base_name_view(normalized_path);

    // Try base name lookup
    auto it = binding_locations.find(std::string(base_name));
    if (it != binding_locations.end()) {
        return std::get<1>(it->second);
    }

    spdlog::error("Binding location not found: {} (base: {})", path, base_name);
    return -1;
}

nvrhi::ResourceType ShaderReflectionInfo::get_binding_type(
    std::string_view path)
{
    std::string normalized_path = normalize_path(path);
    std::string_view base_name = get_base_name_view(normalized_path);

    // Try base name lookup
    auto it = binding_locations.find(std::string(base_name));
    if (it != binding_locations.end()) {
        return binding_spaces[std::get<0>(it->second)]
            .bindings[std::get<1>(it->second)]
            .type;
    }

    spdlog::error("Binding type not found: {} (base: {})", path, base_name);
    return nvrhi::ResourceType::None;
}

unsigned ShaderReflectionInfo::get_binding_array_size(std::string_view path)
{
    std::string normalized_path = normalize_path(path);
    std::string_view base_name = get_base_name_view(normalized_path);

    // Try base name lookup
    auto it = binding_locations.find(std::string(base_name));
    if (it != binding_locations.end()) {
        auto space_id = std::get<0>(it->second);
        auto location_id = std::get<1>(it->second);
        return binding_spaces[space_id].bindings[location_id].size;
    }

    return 1;  // Default to 1 for unknown bindings
}

int ShaderReflectionInfo::parse_array_index(std::string_view path) const
{
    size_t bracket_pos = path.find('[');

    if (bracket_pos == std::string_view::npos) {
        return -1;  // Not an array access
    }

    size_t close_bracket = path.find(']', bracket_pos);
    if (close_bracket == std::string_view::npos) {
        return -1;  // Malformed array access
    }

    std::string_view index_str =
        path.substr(bracket_pos + 1, close_bracket - bracket_pos - 1);

    // Manual parsing to avoid string allocation
    int result = 0;
    for (char c : index_str) {
        if (c < '0' || c > '9') {
            return -1;  // Invalid character
        }
        result = result * 10 + (c - '0');
    }

    return result;
}

// Zero-copy version that returns string_view
std::string_view ShaderReflectionInfo::get_base_name_view(
    std::string_view path) const
{
    // Find the first occurrence of '[' to get the base name for arrays
    size_t bracket_pos = path.find('[');
    size_t dot_pos = path.find('.');

    if (bracket_pos != std::string_view::npos &&
        (dot_pos == std::string_view::npos || bracket_pos < dot_pos)) {
        // Array access found first
        return path.substr(0, bracket_pos);
    }
    else if (dot_pos != std::string_view::npos) {
        // Member access found
        return path.substr(0, dot_pos);
    }

    // No special characters found, return the whole path
    return path;
}

// Legacy string-returning version for compatibility
std::string ShaderReflectionInfo::get_base_name(std::string_view path) const
{
    return std::string(get_base_name_view(path));
}

bool ShaderReflectionInfo::has_binding(std::string_view path) const
{
    std::string normalized_path = normalize_path(path);
    std::string_view base_name = get_base_name_view(normalized_path);

    return binding_locations.find(std::string(base_name)) !=
           binding_locations.end();
}

std::string_view ShaderReflectionInfo::resolve_base_name(
    std::string_view path) const
{
    // Find the first occurrence of '[' to get the base name for arrays
    // For now, we prioritize array access over member access
    size_t bracket_pos = path.find('[');
    size_t dot_pos = path.find('.');

    if (bracket_pos != std::string_view::npos &&
        (dot_pos == std::string_view::npos || bracket_pos < dot_pos)) {
        // Array access found first
        return path.substr(0, bracket_pos);
    }
    else if (dot_pos != std::string_view::npos) {
        // Member access found
        return path.substr(0, dot_pos);
    }

    // No special characters found, return the whole path
    return path;
}

std::string_view ShaderReflectionInfo::extract_array_indices(
    std::string_view path) const
{
    // Extract all array indices and member accesses for future use
    size_t pos = path.find_first_of("[.");
    if (pos == std::string_view::npos) {
        return "";
    }
    return path.substr(pos);
}

bool ShaderReflectionInfo::is_array_access(std::string_view path) const
{
    return path.find('[') != std::string_view::npos;
}

std::string ShaderReflectionInfo::normalize_path(std::string_view path) const
{
    // Handle various path formats and normalize them
    std::string normalized(path);

    // Replace consecutive dots with single dot
    size_t pos = 0;
    while ((pos = normalized.find("..", pos)) != std::string::npos) {
        normalized.replace(pos, 2, ".");
        pos += 1;
    }

    // Remove leading/trailing dots
    if (!normalized.empty() && normalized.front() == '.') {
        normalized.erase(0, 1);
    }
    if (!normalized.empty() && normalized.back() == '.') {
        normalized.pop_back();
    }

    return normalized;
}

ShaderReflectionInfo ShaderReflectionInfo::operator+(
    const ShaderReflectionInfo& other) const
{
    ShaderReflectionInfo result;
    result.binding_spaces = binding_spaces;

    auto larger_size =
        std::max(binding_spaces.size(), other.binding_spaces.size());

    result.binding_spaces.resize(larger_size);

    auto r_size = other.binding_spaces.size();
    for (int i = 0; i < r_size; ++i) {
        auto& r_space = other.binding_spaces[i];
        auto& l_space = result.binding_spaces[i];

        l_space.visibility = l_space.visibility | r_space.visibility;
    }

    for (const auto& [name, location] : other.binding_locations) {
        auto r_space_id = std::get<0>(location);
        auto r_location_id = std::get<1>(location);

        nvrhi::BindingLayoutItem r_binding_item =
            other.binding_spaces[r_space_id].bindings[r_location_id];

        // search in the first binding layout
        auto l_space_id = r_space_id;
        auto& l_space = result.binding_spaces[r_space_id];

        auto pos = std::find(
            l_space.bindings.begin(), l_space.bindings.end(), r_binding_item);

        if (pos == l_space.bindings.end()) {
            l_space.bindings.push_back(r_binding_item);
            unsigned new_l_location =
                static_cast<unsigned int>(l_space.bindings.size() - 1);
            result.binding_locations[name] =
                std::make_tuple(l_space_id, new_l_location);
        }
        else {
            result.binding_locations[name] = std::make_tuple(
                l_space_id,
                static_cast<unsigned int>(pos - l_space.bindings.begin()));
        }
    }

    return result;
}

ShaderReflectionInfo& ShaderReflectionInfo::operator+=(
    const ShaderReflectionInfo& other)
{
    *this = *this + other;
    return *this;
}

std::ostream& operator<<(std::ostream& os, const ShaderReflectionInfo& info)
{
    // print binding layout using binding locations
    for (const std::pair<const std::string, std::tuple<unsigned, unsigned>>&
             binding_location : info.binding_locations) {
        os << binding_location.first << " : ";
        auto space_id = std::get<0>(binding_location.second);
        auto location_id = std::get<1>(binding_location.second);

        os << "space: " << space_id << ", location: " << location_id << "; "
           << std::endl;
    }
    return os;
}

RUZINO_NAMESPACE_CLOSE_SCOPE
