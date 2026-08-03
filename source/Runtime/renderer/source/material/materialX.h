#pragma once
#include "MaterialXCore/Document.h"
#include "MaterialXGenShader/Library.h"
#include "bindlessContext.h"
#include "material.h"

namespace pxr {
class Hio_OpenEXRImage;
}

RUZINO_NAMESPACE_OPEN_SCOPE

class Shader;
using namespace pxr;

class Hio_StbImage;
class HD_RUZINO_API Hd_RUZINO_MaterialX : public Hd_RUZINO_Material {
   public:
    explicit Hd_RUZINO_MaterialX(SdfPath const& id);

    void Sync(
        HdSceneDelegate* sceneDelegate,
        HdRenderParam* renderParam,
        HdDirtyBits* dirtyBits) override;

    void ensure_shader_ready(const ShaderFactory& factory) override;

    // Upload material data to GPU after texture loading is complete
    void upload_material_data();

    // Tear down the process-global MaterialX state that persists across render
    // delegates: the shared_document (which every material's node graph gets
    // added to and is never cleared) and the nodedef lookup cache. Without this,
    // a second Hd_RUZINO_RenderDelegate built in the same process sees a
    // document still carrying the previous scene's material/shader nodes, so the
    // new scene's shader generation runs over stale state and the render comes
    // out black. `libraries` (the read-only stdlib) is intentionally preserved —
    // it's scene-independent — and shared_document is rebuilt from it. Called
    // from ~Hd_RUZINO_RenderDelegate so the next delegate starts clean.
    static void reset_shared_state();

   protected:
    void BuildGPUTextures(Hd_RUZINO_RenderParam* render_param);
    void CollectTextures(
        HdMaterialNetwork2Interface netInterface,
        HdMtlxTexturePrimvarData hdMtlxData);
    HdMaterialNetwork2Interface FetchMaterialNetwork(
        HdSceneDelegate* sceneDelegate,
        HdMaterialNetwork2& hdNetwork,
        SdfPath& materialPath,
        SdfPath& surfTerminalPath,
        HdMaterialNode2 const*& surfTerminal);

    std::string get_data_code;
    // Mapping from texture variable name to data location for texture IDs
    std::unordered_map<std::string, unsigned int> texture_id_locations;
    // Flag to track if material data needs to be uploaded to GPU
    bool material_data_dirty = false;

    // Network structure hash for detecting if only parameters changed
    size_t last_network_hash = 0;
    // Flag to track if we can do incremental parameter update instead of full
    // shader regeneration
    bool can_use_incremental_update = false;
    // Cached parameter mappings from last shader generation (for O(1) lookup)
    std::unordered_map<std::string, ParameterMapping> cached_parameter_mappings;

   private:
    void MtlxGenerateShader(
        MaterialX::ElementPtr mtlx_element,
        HdMaterialNetwork2Interface netInterface,
        HdMtlxTexturePrimvarData& hdMtlxData);

    static MaterialX::GenContextPtr shader_gen_context_;
    static MaterialX::DocumentPtr libraries;
    static MaterialX::DocumentPtr
        shared_document;  // Shared document for all materials (avoids
                          // copy/import)
    static std::once_flag shader_gen_initialized_;
    static std::mutex shadergen_mutex;
    static std::mutex document_mutex;  // Protects shared_document

    // Cache for NodeDef lookups to avoid expensive searches
    static std::unordered_map<std::string, MaterialX::NodeDefPtr>
        nodedef_cache_;
    static std::mutex nodedef_cache_mutex_;

    // Helper method to compute network structure hash
    size_t compute_network_hash(
        const HdMaterialNetwork2Interface& netInterface);

    // Helper method to update parameters incrementally without shader
    // regeneration
    void update_parameters_incremental(
        const HdMaterialNetwork2Interface& netInterface,
        HdMtlxTexturePrimvarData& hdMtlxData);
};

RUZINO_NAMESPACE_CLOSE_SCOPE
