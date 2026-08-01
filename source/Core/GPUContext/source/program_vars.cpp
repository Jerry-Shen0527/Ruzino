#include "GPUContext/program_vars.hpp"

#include <nvrhi/nvrhi.h>

#include <cstdlib>
#include <iostream>

#include "RHI/ResourceManager/resource_allocator.hpp"

RUZINO_NAMESPACE_OPEN_SCOPE

// ProgramVarsProxy implementation
ProgramVarsProxy::ProgramVarsProxy(
    ProgramVars* parent,
    const std::string& path,
    int array_index)
    : parent_(parent),
      path_(path),
      binding_id_(),
      array_index_(array_index)
{
}

ProgramVarsProxy::ProgramVarsProxy(
    ProgramVars* parent,
    BindingID binding_id,
    int array_index)
    : parent_(parent),
      path_(),
      binding_id_(binding_id),
      array_index_(array_index)
{
}

ProgramVarsProxy ProgramVarsProxy::operator[](const std::string& name)
{
    // If we have a valid binding_id, use it for faster access
    if (binding_id_.is_valid()) {
        // This is an error case - can't do member access on a resolved binding
        // Fall back to string-based path
        return ProgramVarsProxy(parent_, build_path(name));
    }
    return ProgramVarsProxy(parent_, build_path(name));
}

ProgramVarsProxy ProgramVarsProxy::operator[](int index)
{
    // For array access with a valid binding_id, keep the ID
    if (binding_id_.is_valid()) {
        return ProgramVarsProxy(parent_, binding_id_, index);
    }
    // For array access, keep the base path and store the index separately
    return ProgramVarsProxy(parent_, path_, index);
}

ProgramVarsProxy& ProgramVarsProxy::operator=(nvrhi::IResource* resource)
{
    // Get the binding location for this proxy
    std::tuple<unsigned, unsigned> location;
    if (binding_id_.is_valid()) {
        location =
            parent_->get_binding_location_fast(binding_id_, array_index_);
        parent_->get_resource_direct(binding_id_, array_index_) = resource;
    }
    else {
        location = parent_->get_binding_location(path_, array_index_);
        parent_->get_resource_direct(path_, array_index_) = resource;
    }

    auto [binding_space_id, binding_set_location] = location;
    if (binding_space_id != static_cast<unsigned>(-1)) {
        // When assigning a resource directly (not via BindingSetItem),
        // automatically set range/subresources to defaults
        auto& binding_item =
            parent_->binding_spaces[binding_space_id][binding_set_location];

        if (dynamic_cast<nvrhi::IBuffer*>(resource)) {
            binding_item.range = nvrhi::EntireBuffer;
        }
        else if (dynamic_cast<nvrhi::ITexture*>(resource)) {
            binding_item.subresources = nvrhi::AllSubresources;
        }
    }

    return *this;
}

ProgramVarsProxy& ProgramVarsProxy::operator=(const nvrhi::BindingSetItem& item)
{
    // Get the binding location for this proxy
    std::tuple<unsigned, unsigned> location;
    if (binding_id_.is_valid()) {
        location =
            parent_->get_binding_location_fast(binding_id_, array_index_);
    }
    else {
        location = parent_->get_binding_location(path_, array_index_);
    }

    auto [binding_space_id, binding_set_location] = location;
    if (binding_space_id == static_cast<unsigned>(-1)) {
        return *this;  // Invalid binding
    }

    // Selectively copy fields from the input item
    // DO NOT overwrite slot, type, and arrayElement which were set from
    // reflection
    auto& target =
        parent_->binding_spaces[binding_space_id][binding_set_location];
    target.resourceHandle = item.resourceHandle;
    target.format = item.format;
    target.dimension = item.dimension;

    // Copy union field based on resource type
    // subresources and range are in the same union, so only copy the relevant
    // one
    if (item.type == nvrhi::ResourceType::Texture_SRV ||
        item.type == nvrhi::ResourceType::Texture_UAV) {
        target.subresources = item.subresources;
    }
    else if (
        item.type == nvrhi::ResourceType::TypedBuffer_SRV ||
        item.type == nvrhi::ResourceType::TypedBuffer_UAV ||
        item.type == nvrhi::ResourceType::StructuredBuffer_SRV ||
        item.type == nvrhi::ResourceType::StructuredBuffer_UAV ||
        item.type == nvrhi::ResourceType::RawBuffer_SRV ||
        item.type == nvrhi::ResourceType::RawBuffer_UAV ||
        item.type == nvrhi::ResourceType::ConstantBuffer) {
        target.range = item.range;
    }
    // Preserve: slot, type, arrayElement (set by get_binding_location)

    return *this;
}

ProgramVarsProxy::operator nvrhi::IResource*&()
{
    if (binding_id_.is_valid()) {
        return parent_->get_resource_direct(binding_id_, array_index_);
    }
    return parent_->get_resource_direct(path_, array_index_);
}

std::string ProgramVarsProxy::build_path(const std::string& name) const
{
    if (path_.empty()) {
        return name;
    }
    // Handle both member access and nested structures
    return path_ + "." + name;
}

// ProgramVars implementation
ProgramVars::ProgramVars(ResourceAllocator& r) : resource_allocator_(r)
{
}

ProgramVars::~ProgramVars()
{
    for (int i = 0; i < binding_sets_solid.size(); ++i) {
        resource_allocator_.destroy(binding_sets_solid[i]);
    }
    for (int i = 0; i < binding_layouts.size(); ++i) {
        resource_allocator_.destroy(binding_layouts[i]);
    }
    if (nvapi_ext_buffer_) {
        resource_allocator_.destroy(nvapi_ext_buffer_);
    }
}

void ProgramVars::finish_setting_vars()
{
    // Auto-create NVAPI extension buffer if needed
    if (!programs.empty() && programs[0]->get_desc().hlslExtensionsUAV >= 0) {
        // Check if g_NvidiaExt binding exists in reflection
        if (final_reflection_info.has_binding("g_NvidiaExt")) {
            // Destroy old buffer if it exists
            if (nvapi_ext_buffer_) {
                resource_allocator_.destroy(nvapi_ext_buffer_);
            }

            // Create minimal UAV buffer for NVAPI SER
            nvrhi::BufferDesc nvapi_desc;
            nvapi_desc.byteSize = sizeof(int);  // Minimal placeholder
            nvapi_desc.structStride = 0;
            nvapi_desc.canHaveUAVs = true;
            nvapi_desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            nvapi_desc.keepInitialState = true;
            nvapi_desc.debugName = "g_NvidiaExt_auto";
            nvapi_desc.format = nvrhi::Format::R32_SINT;
            nvapi_desc.structStride = sizeof(int);
            nvapi_ext_buffer_ = resource_allocator_.create(nvapi_desc);

            // Automatically bind it using operator[] to trigger proper
            // initialization
            (*this)["g_NvidiaExt"] = nvapi_ext_buffer_;
        }
    }

    for (int i = 0; i < binding_sets_solid.size(); ++i) {
        resource_allocator_.destroy(binding_sets_solid[i]);
    }
    binding_sets_solid.resize(0);
    binding_sets_solid.resize(binding_spaces.size());

    for (int i = 0; i < binding_spaces.size(); ++i) {
        if (!descriptor_tables[i]) {
            BindingSetDesc desc{};
            desc.bindings = binding_spaces[i];

            binding_sets_solid[i] =
                resource_allocator_.create(desc, binding_layouts[i].Get());
        }
    }

    // When RZ_RHI_VALIDATION is set, dump the reflected binding map alongside
    // which slots actually received a resource. A slot that is reflected
    // (declared by the shader) but whose resourceHandle is null is exactly
    // what nvrhi reports as "Bindings declared in the layout are not present
    // in the binding set: tN" — this dump names the culprit.
    if (std::getenv("RZ_RHI_VALIDATION")) {
        std::cerr << "[program-vars] reflected bindings:\n"
                  << final_reflection_info;
        std::cerr << "[program-vars] binding fill status (null = MISSING):\n";
        for (size_t space = 0; space < binding_spaces.size(); ++space) {
            const auto& items = binding_spaces[space];
            for (size_t loc = 0; loc < items.size(); ++loc) {
                const auto& item = items[loc];
                std::cerr << "  space=" << space << " slot=t" << item.slot
                          << " type=" << static_cast<int>(item.type)
                          << " resource="
                          << (item.resourceHandle ? "bound" : "NULL")
                          << (descriptor_tables[space]
                                  ? " (descriptor-table space)"
                                  : "")
                          << "\n";
            }
        }
        std::cerr << std::flush;
    }
}

// This is based on reflection
unsigned ProgramVars::get_binding_space(std::string_view name)
{
    return final_reflection_info.get_binding_space(name);
}

// This is based on reflection
unsigned ProgramVars::get_binding_id(std::string_view name)
{
    auto binding_space = get_binding_space(name);
    if (binding_space == -1) {
        return -1;
    }

    auto binding_location = final_reflection_info.get_binding_location(name);
    if (binding_location == -1) {
        return -1;
    }

    auto slot = final_reflection_info.get_binding_layout_descs()[binding_space]
                    .bindings[binding_location]
                    .slot;

    return slot;
}

// This is based on reflection
nvrhi::ResourceType ProgramVars::get_binding_type(std::string_view name)
{
    return final_reflection_info.get_binding_type(name);
}

// O(1) binding ID resolution with caching
BindingID ProgramVars::resolve_binding_id(std::string_view name)
{
    // Get base name without array indices
    std::string_view base_name = final_reflection_info.get_base_name_view(name);

    // Check cache first - need to convert to string for lookup
    std::string base_name_str(base_name);
    auto cache_it = base_name_to_id_cache.find(base_name_str);
    if (cache_it != base_name_to_id_cache.end()) {
        return cache_it->second;
    }

    // Resolve and cache
    unsigned space_id = get_binding_space(base_name);
    if (space_id == -1) {
        return BindingID();  // Invalid
    }

    unsigned location = final_reflection_info.get_binding_location(base_name);
    if (location == -1) {
        return BindingID();  // Invalid
    }

    BindingID result(space_id, location);
    base_name_to_id_cache.emplace(std::move(base_name_str), result);
    return result;
}

// Fast path using pre-resolved BindingID
std::tuple<unsigned, unsigned> ProgramVars::get_binding_location_fast(
    BindingID binding_id,
    int array_index)
{
    if (!binding_id.is_valid()) {
        return std::make_tuple(-1, -1);
    }

    auto [space_id, layout_location] = binding_id.as_tuple();

    // Build cache key
    std::string cache_key =
        std::to_string(space_id) + ":" + std::to_string(layout_location);
    if (array_index >= 0) {
        cache_key += "[" + std::to_string(array_index) + "]";
    }

    // Check if we've already created a binding location
    auto path_it = path_to_binding_location.find(cache_key);
    if (path_it != path_to_binding_location.end()) {
        return path_it->second;
    }

    // Ensure space exists
    if (binding_spaces.size() <= space_id) {
        binding_spaces.resize(space_id + 1);
    }
    if (descriptor_tables.size() <= space_id) {
        descriptor_tables.resize(space_id + 1);
    }

    auto& binding_space = binding_spaces[space_id];
    auto& binding_layout = get_binding_layout()[space_id];
    auto& layout_items = binding_layout->getDesc()->bindings;

    // Use the layout_location directly
    if (layout_location >= layout_items.size()) {
        return std::make_tuple(-1, -1);
    }

    const auto& layout_item = layout_items[layout_location];

    // Validate array index
    if (array_index >= 0 &&
        static_cast<unsigned>(array_index) >= layout_item.size) {
        assert(false && "Array index out of bounds");
        return std::make_tuple(-1, -1);
    }

    // Create new binding set item
    unsigned binding_set_location = binding_space.size();
    binding_space.resize(binding_set_location + 1);

    nvrhi::BindingSetItem& item = binding_space[binding_set_location];
    item.resourceHandle = nullptr;
    item.slot = layout_item.slot;
    item.type = layout_item.type;
    item.format = nvrhi::Format::UNKNOWN;
    item.dimension = nvrhi::TextureDimension::Unknown;
    item.unused = 0;
    item.unused2 = 0;
    item.subresources = nvrhi::AllSubresources;
    item.arrayElement =
        array_index >= 0 ? static_cast<uint32_t>(array_index) : 0;

    auto result = std::make_tuple(space_id, binding_set_location);
    path_to_binding_location[cache_key] = result;
    return result;
}

// This is where it is within the binding set
std::tuple<unsigned, unsigned> ProgramVars::get_binding_location(
    std::string_view name,
    int array_index)
{
    // Build cache key - use string for heterogeneous lookup
    std::string cache_key;
    if (array_index >= 0) {
        cache_key = std::string(name) + "[" + std::to_string(array_index) + "]";
    }
    else {
        cache_key = std::string(name);
    }

    // Check if we've already created a binding location for this exact path
    auto path_it = path_to_binding_location.find(cache_key);
    if (path_it != path_to_binding_location.end()) {
        return path_it->second;
    }

    // Get the base name
    std::string_view base_name = final_reflection_info.get_base_name_view(name);

    // If array_index is -1, try parsing from the name string
    if (array_index < 0) {
        array_index = final_reflection_info.parse_array_index(name);
    }

    unsigned binding_space_id = get_binding_space(base_name);

    if (binding_space_id == -1) {
        return std::make_tuple(-1, -1);
    }

    if (binding_spaces.size() <= binding_space_id) {
        binding_spaces.resize(binding_space_id + 1);
    }
    if (descriptor_tables.size() <= binding_space_id) {
        descriptor_tables.resize(binding_space_id + 1);
    }

    auto& binding_space = binding_spaces[binding_space_id];

    auto& binding_layout = get_binding_layout()[binding_space_id];
    auto& layout_items = binding_layout->getDesc()->bindings;

    // Find the layout item for the base binding
    auto pos = std::find_if(
        layout_items.begin(),
        layout_items.end(),
        [&base_name, this](const nvrhi::BindingLayoutItem& binding) {
            return binding.slot == get_binding_id(base_name) &&
                   binding.type == get_binding_type(base_name);
        });

    assert(pos != layout_items.end());

    // Get array size to validate array index
    unsigned array_size =
        final_reflection_info.get_binding_array_size(base_name);
    if (array_index >= 0 && static_cast<unsigned>(array_index) >= array_size) {
        assert(false && "Array index out of bounds");
        return std::make_tuple(-1, -1);
    }

    // Create a new BindingSetItem for this specific array element (or single
    // binding)
    unsigned binding_set_location = binding_space.size();
    binding_space.resize(binding_set_location + 1);

    nvrhi::BindingSetItem& item = binding_space[binding_set_location];

    // Initialize all fields properly (default constructor doesn't initialize
    // for performance)
    item.resourceHandle = nullptr;
    item.slot = get_binding_id(base_name);
    item.type = get_binding_type(base_name);
    item.format = nvrhi::Format::UNKNOWN;
    item.dimension = nvrhi::TextureDimension::Unknown;
    item.unused = 0;
    item.unused2 = 0;
    item.subresources = nvrhi::AllSubresources;

    // Set the array element if this is an array access
    if (array_index >= 0) {
        item.arrayElement = static_cast<uint32_t>(array_index);
    }
    else {
        item.arrayElement = 0;
    }

    // Cache this path -> binding location mapping
    auto result = std::make_tuple(binding_space_id, binding_set_location);
    path_to_binding_location[cache_key] = result;

    return result;
}

static nvrhi::IResource* placeholder;

ProgramVarsProxy ProgramVars::operator[](std::string_view name)
{
    // Try to resolve to BindingID for faster future access
    BindingID binding_id = resolve_binding_id(name);
    if (binding_id.is_valid()) {
        return ProgramVarsProxy(this, binding_id);
    }
    // Fall back to string-based path
    return ProgramVarsProxy(this, std::string(name));
}

nvrhi::IResource*& ProgramVars::get_resource_direct(
    std::string_view name,
    int array_index)
{
    auto [binding_space_id, binding_set_location] =
        get_binding_location(name, array_index);

    if (binding_space_id == -1) {
        return placeholder;
    }

    return binding_spaces[binding_space_id][binding_set_location]
        .resourceHandle;
}

// Fast path: O(1) access using pre-resolved BindingID
nvrhi::IResource*& ProgramVars::get_resource_direct(
    BindingID binding_id,
    int array_index)
{
    auto [binding_space_id, binding_set_location] =
        get_binding_location_fast(binding_id, array_index);

    if (binding_space_id == -1) {
        return placeholder;
    }

    return binding_spaces[binding_space_id][binding_set_location]
        .resourceHandle;
}

void ProgramVars::set_descriptor_table(
    const std::string& name,
    nvrhi::IDescriptorTable* table,
    BindingLayoutHandle layout_handle)
{
    auto [binding_space_id, binding_set_location] = get_binding_location(name);
    if (binding_space_id == -1) {
        return;
    }
    descriptor_tables[binding_space_id] = table;

    if (binding_layouts[binding_space_id]) {
        resource_allocator_.destroy(binding_layouts[binding_space_id]);
        binding_layouts[binding_space_id] = layout_handle;
    }
}

void ProgramVars::set_binding(
    const std::string& name,
    nvrhi::ITexture* resource,
    const nvrhi::TextureSubresourceSet& subset)
{
    auto [binding_space_id, binding_set_location] = get_binding_location(name);
    if (binding_space_id == -1) {
        return;
    }
    auto& binding_set = binding_spaces[binding_space_id][binding_set_location];

    binding_set.resourceHandle = resource;
    binding_set.subresources = subset;
    if (subset.baseArraySlice != 0 || subset.numArraySlices != 1) {
        binding_set.dimension = nvrhi::TextureDimension::Texture2DArray;
    }
}

nvrhi::BindingSetVector ProgramVars::get_binding_sets() const
{
    nvrhi::BindingSetVector result;

    result.resize(binding_sets_solid.size());
    for (int i = 0; i < binding_sets_solid.size(); ++i) {
        if (binding_sets_solid[i]) {
            result[i] = binding_sets_solid[i].Get();
        }
        if (descriptor_tables[i]) {
            result[i] = descriptor_tables[i];
        }
    }
    return result;
}

nvrhi::BindingLayoutVector& ProgramVars::get_binding_layout()
{
    if (binding_layouts.empty()) {
        auto binding_layout_descs =
            final_reflection_info.get_binding_layout_descs();
        for (int i = 0; i < binding_layout_descs.size(); ++i) {
            auto binding_layout =
                resource_allocator_.create(binding_layout_descs[i]);
            binding_layouts.push_back(binding_layout);
        }
    }
    return binding_layouts;
}

std::vector<IProgram*> ProgramVars::get_programs() const
{
    return programs;
}

RUZINO_NAMESPACE_CLOSE_SCOPE
