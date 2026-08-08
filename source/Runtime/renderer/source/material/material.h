#pragma once

#include "GPUContext/program_vars.hpp"
#include "api.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/hd/materialNetwork2Interface.h"
#include "pxr/imaging/hdMtlx/hdMtlx.h"
#include "pxr/imaging/hio/image.h"
#include "renderParam.h"

namespace pxr {
class Hio_OpenEXRImage;

}

RUZINO_NAMESPACE_OPEN_SCOPE
class Shader;
using namespace pxr;

class Hio_StbImage;
class HD_RUZINO_API Hd_RUZINO_Material : public HdMaterial {
   public:
    explicit Hd_RUZINO_Material(SdfPath const& id);

    HdDirtyBits GetInitialDirtyBitsMask() const override;

    void Finalize(HdRenderParam* renderParam) override;
    void Sync(
        HdSceneDelegate* sceneDelegate,
        HdRenderParam* renderParam,
        HdDirtyBits* dirtyBits) override;
    void ensure_material_data_handle(Hd_RUZINO_RenderParam* render_param);

    virtual void ensure_shader_ready(const ShaderFactory& factory);

    // Get shader generation counter to track changes
    uint32_t get_shader_generation() const
    {
        return shader_generation;
    }

    unsigned GetMaterialLocation() const;

    std::string GetShader(const ShaderFactory& factory);

    std::string GetMaterialName() const;

    // Upload material data to GPU (override in subclasses if needed)
    virtual void upload_material_data()
    {
    }

    virtual void update_data_loader(
        DescriptorIndex descriptor_index,
        const std::string& texture_name)
    {
    }

    // Accessor for shader path (for custom callable shaders)
    [[nodiscard]] const std::string& GetShaderPath() const
    {
        return shader_path;
    }
    [[nodiscard]] bool HasValidShader() const
    {
        return has_valid_shader;
    }

    /// Returns true if this material emits light (emission > 0 in
    /// standard_surface, or emissiveColor > 0 in UsdPreviewSurface). Used by
    /// LightCollection to decide whether a mesh instance's triangles should be
    /// registered as emissive for NEE sampling. Base implementation returns
    /// false; MaterialX subclass inspects cached_parameter_mappings.
    virtual bool isEmissive() const
    {
        return false;
    }

    /// Returns the emission radiance color (RGB) and scalar strength if the
    /// material is emissive. For standard_surface: emission_color * emission.
    /// For UsdPreviewSurface: emissiveColor. Returns (0,0,0) if not emissive.
    /// Used by LightCollection to compute per-triangle flux for BVH importance
    /// sampling (CPU-side estimate, no texture integration).
    virtual GfVec3f getEmissionRadiance() const
    {
        return GfVec3f(0.0f);
    }

    /// Returns the bindless texture descriptor index for the emission texture,
    /// or 0xffffffff if emission is a constant (no texture). For
    /// standard_surface this is the texture connected to emission_color; for
    /// UsdPreviewSurface, to emissiveColor. Used by LightCollection to let the
    /// NEE sampler query textured emission radiance at the sampled point.
    virtual uint32_t getEmissionTextureIndex() const
    {
        return 0xffffffffu;
    }

   protected:
    HdMaterialNetwork2 surfaceNetwork;

    std::string eval_shader_source;
    std::string material_name;
    std::string final_shader_source;

    bool shader_ready = false;

    std::unordered_map<std::string, std::string> texturePaths;
    ProgramHandle final_program;

    struct TextureResource {
        std::string filePath;
        HioImageSharedPtr image;
        nvrhi::TextureHandle texture;
        DescriptorHandle descriptor;
        bool isSRGB =
            false;  // Track whether this texture should use sRGB format
    };

    std::unordered_map<std::string, TextureResource> textureResources;

    HdMaterialNetwork2Interface FetchNetInterface(
        HdSceneDelegate* sceneDelegate,
        HdMaterialNetwork2& hdNetwork,
        SdfPath& materialPath);

    DeviceMemoryPool<MaterialDataBlob>::MemoryHandle material_data_handle;
    DeviceMemoryPool<MaterialHeader>::MemoryHandle material_header_handle;
    MaterialDataBlob material_data;

    std::string slang_source_code_main;
    static std::string slang_source_code_template;
    static std::string eval_source_code_fallback;
    static std::mutex texture_mutex;
    static std::mutex material_data_handle_mutex;

    uint32_t shader_generation = 0;  // Incremented when shader is regenerated

    // Path to custom callable shader file
    std::string shader_path;
    bool has_valid_shader =
        false;  // True only if shader_path points to a valid file
};

RUZINO_NAMESPACE_CLOSE_SCOPE
