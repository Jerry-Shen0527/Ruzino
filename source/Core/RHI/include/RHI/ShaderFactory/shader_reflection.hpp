#pragma once

#include "RHI/ShaderFactory/vector_map.hpp"
#include "RHI/api.h"
#include "RHI/internal/nvrhi_patch.hpp"

RUZINO_NAMESPACE_OPEN_SCOPE
class RHI_API ShaderReflectionInfo {
   public:
    [[nodiscard]] const nvrhi::BindingLayoutDescVector&
    get_binding_layout_descs() const;

    // Main interface for nested path resolution (now using string_view for
    // zero-copy)
    unsigned get_binding_space(std::string_view path);
    unsigned get_binding_location(std::string_view path);
    nvrhi::ResourceType get_binding_type(std::string_view path);

    // Get the array size for a binding (returns 1 for non-array bindings)
    unsigned get_binding_array_size(std::string_view path);

    // Parse array index from path like "samplers[3]", returns -1 if not an
    // array access
    int parse_array_index(std::string_view path) const;

    // Get base name without array indices (e.g., "samplers[3]" -> "samplers")
    // Returns a string_view into the original string for zero-copy
    std::string_view get_base_name_view(std::string_view path) const;

    // Legacy string-returning version (for compatibility)
    std::string get_base_name(std::string_view path) const;

    // Check if a binding exists
    bool has_binding(std::string_view path) const;

    ShaderReflectionInfo operator+(const ShaderReflectionInfo& other) const;
    ShaderReflectionInfo& operator+=(const ShaderReflectionInfo& other);

   private:
    nvrhi::BindingLayoutDescVector binding_spaces;
    VectorMap<std::string, std::tuple<unsigned, unsigned>> binding_locations;

    // Helper methods for path parsing (using string_view for zero-copy)
    std::string_view resolve_base_name(std::string_view path) const;
    std::string_view extract_array_indices(std::string_view path) const;
    bool is_array_access(std::string_view path) const;
    std::string normalize_path(std::string_view path) const;

    friend class ShaderFactory;
    friend RHI_API std::ostream& operator<<(
        std::ostream& os,
        const ShaderReflectionInfo& info);
};

RHI_API std::ostream& operator<<(
    std::ostream& os,
    const ShaderReflectionInfo& info);
RUZINO_NAMESPACE_CLOSE_SCOPE
