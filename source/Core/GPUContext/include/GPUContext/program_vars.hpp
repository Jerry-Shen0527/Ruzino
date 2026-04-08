#pragma once
#include <string_view>
#include <unordered_map>

#include "RHI/ResourceManager/resource_allocator.hpp"
#include "api.h"
#include "nvrhi/nvrhi.h"

RUZINO_NAMESPACE_OPEN_SCOPE

using BindingSetItemArray = std::vector<nvrhi::BindingSetItem>;

class ProgramVars;

// Lightweight ID-based binding location for O(1) access
struct BindingID {
    uint32_t space_id : 16;     // Binding space index
    uint32_t location_id : 16;  // Location within the space

    constexpr BindingID() : space_id(0xFFFF), location_id(0xFFFF)
    {
    }
    constexpr BindingID(unsigned space, unsigned loc)
        : space_id(space),
          location_id(loc)
    {
    }

    constexpr bool is_valid() const
    {
        return space_id != 0xFFFF;
    }
    constexpr std::tuple<unsigned, unsigned> as_tuple() const
    {
        return std::make_tuple(space_id, location_id);
    }
};

class GPUCONTEXT_API ProgramVarsProxy {
   public:
    ProgramVarsProxy(
        ProgramVars* parent,
        const std::string& path,
        int array_index = -1);
    ProgramVarsProxy(
        ProgramVars* parent,
        BindingID binding_id,
        int array_index = -1);

    // Support nested access with string
    ProgramVarsProxy operator[](const std::string& name);

    // Support array access with integer
    ProgramVarsProxy operator[](int index);

    // Assignment operator for direct resource assignment
    ProgramVarsProxy& operator=(nvrhi::IResource* resource);

    ProgramVarsProxy& operator=(const nvrhi::BindingSetItem& item);

    // Implicit conversion to nvrhi::IResource*&
    operator nvrhi::IResource*&();

   private:
    ProgramVars* parent_;
    std::string path_;      // Base path without array index (only used for
                            // string-based access)
    BindingID binding_id_;  // Pre-resolved binding ID for O(1) access
    int array_index_;       // -1 means not an array access

    std::string build_path(const std::string& name) const;
};

class GPUCONTEXT_API ProgramVars {
    friend class ProgramVarsProxy;

   public:
    ProgramVars(ResourceAllocator& r);

    template<typename... Args>
    ProgramVars(
        ResourceAllocator& r,
        const ProgramHandle& program,
        Args&&... args);

    ~ProgramVars();
    void finish_setting_vars();

    // Single operator[] using string_view - accepts const char*, std::string,
    // and std::string_view
    ProgramVarsProxy operator[](std::string_view name);

    nvrhi::IResource*& get_resource(std::string_view name)
    {
        return get_resource_direct(name);
    }

    // Fast path: pre-resolved binding ID
    nvrhi::IResource*& get_resource(BindingID binding_id, int array_index = -1)
    {
        return get_resource_direct(binding_id, array_index);
    }

    // Get binding ID for caching (call once, reuse many times)
    BindingID resolve_binding_id(std::string_view name);

    void set_descriptor_table(
        const std::string& name,
        nvrhi::IDescriptorTable* table,
        BindingLayoutHandle layout_handle);
    // This is for setting extra settings
    void set_binding(
        const std::string& name,
        nvrhi::ITexture* resource,
        const nvrhi::TextureSubresourceSet& subset = {});
    nvrhi::BindingSetVector get_binding_sets() const;
    nvrhi::BindingLayoutVector& get_binding_layout();
    std::vector<IProgram*> get_programs() const;

   private:
    nvrhi::BindingLayoutVector binding_layouts;

    nvrhi::static_vector<BindingSetItemArray, nvrhi::c_MaxBindingLayouts>
        binding_spaces;

    nvrhi::static_vector<nvrhi::BindingSetHandle, nvrhi::c_MaxBindingLayouts>
        binding_sets_solid;

    nvrhi::static_vector<nvrhi::IDescriptorTable*, nvrhi::c_MaxBindingLayouts>
        descriptor_tables;
    ResourceAllocator& resource_allocator_;
    std::vector<IProgram*> programs;

    // Automatically created NVAPI extension buffer (for SER support)
    nvrhi::BufferHandle nvapi_ext_buffer_;

    // Map from full path (including array indices) to binding location in
    // binding_spaces This allows us to have multiple BindingSetItems for the
    // same base binding with different arrayElements
    std::unordered_map<std::string, std::tuple<unsigned, unsigned>>
        path_to_binding_location;

    // Cache of base name -> BindingID for O(1) repeated lookups
    std::unordered_map<std::string, BindingID> base_name_to_id_cache;

    unsigned get_binding_space(std::string_view name);
    unsigned get_binding_id(std::string_view name);

    nvrhi::ResourceType get_binding_type(std::string_view name);
    std::tuple<unsigned, unsigned> get_binding_location(
        std::string_view name,
        int array_index = -1);
    std::tuple<unsigned, unsigned> get_binding_location_fast(
        BindingID binding_id,
        int array_index = -1);

    nvrhi::IResource*& get_resource_direct(
        std::string_view name,
        int array_index = -1);
    nvrhi::IResource*& get_resource_direct(
        BindingID binding_id,
        int array_index = -1);

    ShaderReflectionInfo final_reflection_info;
};

template<typename... Args>
ProgramVars::ProgramVars(
    ResourceAllocator& r,
    const ProgramHandle& program,
    Args&&... args)
    : ProgramVars(r, std::forward<Args>(args)...)
{
    programs.push_back(program.Get());
    final_reflection_info += program.Get()->get_reflection_info();
}

RUZINO_NAMESPACE_CLOSE_SCOPE