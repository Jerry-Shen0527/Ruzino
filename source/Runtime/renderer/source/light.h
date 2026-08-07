#pragma once
#include "DescriptorTableManager.h"
#include "api.h"
#include "internal/memory/DeviceMemoryPool.hpp"
#include "nvrhi/nvrhi.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/garch/glApi.h"
#include "pxr/imaging/hd/light.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/hio/image.h"
#include "pxr/pxr.h"
#include "pxr/usd/sdf/assetPath.h"
// SceneTypes (MeshDesc, GeometryInstanceData, GeometryType) -- shared host/
// device header, included by renderTLAS.h and mesh.h the same way.
#include "../nodes/shaders/Scene/SceneTypes.slang"

RUZINO_NAMESPACE_OPEN_SCOPE

using namespace pxr;
// Forward declarations
struct LightData;
class Hd_RUZINO_RenderParam;
// Base light class
class HD_RUZINO_API Hd_RUZINO_Light : public HdLight {
   public:
    explicit Hd_RUZINO_Light(const SdfPath& id, const TfToken& lightType)
        : HdLight(id),
          _lightType(lightType)
    {
    }

    virtual ~Hd_RUZINO_Light() = default;

    void Sync(
        HdSceneDelegate* sceneDelegate,
        HdRenderParam* renderParam,
        HdDirtyBits* dirtyBits) override;
    HdDirtyBits GetInitialDirtyBitsMask() const override;

    VtValue Get(TfToken const& token) const;

    [[nodiscard]] TfToken GetLightType() const
    {
        return _lightType;
    }

    [[nodiscard]] typename DeviceMemoryPool<LightData>::MemoryHandle
    GetLightBuffer() const
    {
        return light_buffer;
    }

    void Finalize(HdRenderParam* renderParam) override;

   protected:
    // Stores the internal light type of this light.
    TfToken _lightType;
    // Cached states.
    TfHashMap<TfToken, VtValue, TfToken::HashFunctor> _params;
    // GPU buffer for light data
    typename DeviceMemoryPool<LightData>::MemoryHandle light_buffer;
};

// Simple light (directional, point light)
class HD_RUZINO_API Hd_RUZINO_Simple_Light : public Hd_RUZINO_Light {
   public:
    Hd_RUZINO_Simple_Light(const SdfPath& id, const TfToken& lightType)
        : Hd_RUZINO_Light(id, lightType)
    {
    }

    void Sync(
        HdSceneDelegate* sceneDelegate,
        HdRenderParam* renderParam,
        HdDirtyBits* dirtyBits) override;
};

// Distant light (sun light)
class HD_RUZINO_API Hd_RUZINO_Distant_Light : public Hd_RUZINO_Light {
   public:
    Hd_RUZINO_Distant_Light(const SdfPath& id, const TfToken& lightType)
        : Hd_RUZINO_Light(id, lightType)
    {
    }

    void Sync(
        HdSceneDelegate* sceneDelegate,
        HdRenderParam* renderParam,
        HdDirtyBits* dirtyBits) override;

    // Accessors
    GfVec3f GetDirection() const
    {
        return _direction;
    }
    float GetAngle() const
    {
        return _angle;
    }

   private:
    GfVec3f _direction;
    float _angle = 0.53f;  // Default sun angle
};

// Sphere light
class HD_RUZINO_API Hd_RUZINO_Sphere_Light : public Hd_RUZINO_Light {
   public:
    Hd_RUZINO_Sphere_Light(const SdfPath& id, const TfToken& lightType)
        : Hd_RUZINO_Light(id, lightType)
    {
    }

    void Sync(
        HdSceneDelegate* sceneDelegate,
        HdRenderParam* renderParam,
        HdDirtyBits* dirtyBits) override;

    // Accessor
    float GetRadius() const
    {
        return _radius;
    }

   private:
    float _radius = 1.0f;
};

// Rectangle light
class HD_RUZINO_API Hd_RUZINO_Rect_Light : public Hd_RUZINO_Light {
   public:
    Hd_RUZINO_Rect_Light(const SdfPath& id, const TfToken& lightType)
        : Hd_RUZINO_Light(id, lightType)
    {
    }

    void Sync(
        HdSceneDelegate* sceneDelegate,
        HdRenderParam* renderParam,
        HdDirtyBits* dirtyBits) override;

    // Accessors
    float GetWidth() const
    {
        return _width;
    }
    float GetHeight() const
    {
        return _height;
    }

   private:
    float _width = 1.0f;
    float _height = 1.0f;

    // --- Intersectable light geometry (BSDF-sampled rays can hit the light) ---
    // A 2-triangle quad in world space (positions only; no normals/tangents --
    // the light's Le comes from lightBuffer, not from a material). Stored as a
    // single interleaved buffer: 4 positions (RGB32_FLOAT) + 6 indices
    // (R32_UINT), mirroring the mesh BLAS layout so it can use the standard
    // triangle hit group.
    nvrhi::BufferHandle light_vertex_buffer;
    nvrhi::rt::AccelStructHandle light_blas;
    DescriptorHandle light_descriptor_handle;
    CommandListHandle light_command_list;
    // Pool slots. These are rebuilt only when geometry actually changes (width/
    // height/transform), not every dirty tick.
    DeviceMemoryPool<MeshDesc>::MemoryHandle light_mesh_desc_buffer;
    DeviceMemoryPool<GeometryInstanceData>::MemoryHandle light_instance_buffer;
    DeviceMemoryPool<nvrhi::rt::InstanceDesc>::MemoryHandle light_rt_instance_buffer;

    void BuildLightGeometry(
        Hd_RUZINO_RenderParam* render_param,
        float3 posW,
        float3 tangent,
        float3 bitangent);
};

// Disk light
class HD_RUZINO_API Hd_RUZINO_Disk_Light : public Hd_RUZINO_Light {
   public:
    Hd_RUZINO_Disk_Light(const SdfPath& id, const TfToken& lightType)
        : Hd_RUZINO_Light(id, lightType)
    {
    }

    void Sync(
        HdSceneDelegate* sceneDelegate,
        HdRenderParam* renderParam,
        HdDirtyBits* dirtyBits) override;

    // Accessor
    float GetRadius() const
    {
        return _radius;
    }

   private:
    float _radius = 1.0f;
};

// Cylinder light
class HD_RUZINO_API Hd_RUZINO_Cylinder_Light : public Hd_RUZINO_Light {
   public:
    Hd_RUZINO_Cylinder_Light(const SdfPath& id, const TfToken& lightType)
        : Hd_RUZINO_Light(id, lightType)
    {
    }

    void Sync(
        HdSceneDelegate* sceneDelegate,
        HdRenderParam* renderParam,
        HdDirtyBits* dirtyBits) override;

    // Accessors
    float GetRadius() const
    {
        return _radius;
    }
    float GetLength() const
    {
        return _length;
    }

   private:
    float _radius = 1.0f;
    float _length = 1.0f;
};

class HD_RUZINO_API Hd_RUZINO_Dome_Light : public Hd_RUZINO_Light {
   public:
    struct HD_RUZINO_API InputDescriptor {
        HioImageSharedPtr image = nullptr;

        TfToken wrapS;
        TfToken wrapT;

        TfToken uv_primvar_name;

        VtValue value;

        GLuint glTexture = 0;
        TfToken input_name;

        nvrhi::TextureHandle texture;
        DescriptorHandle descriptor;
    };

    Hd_RUZINO_Dome_Light(const SdfPath& id, const TfToken& lightType)
        : Hd_RUZINO_Light(id, lightType)
    {
    }

    void _PrepareDomeLight(SdfPath const& id, HdSceneDelegate* scene_delegate);
    void Sync(
        HdSceneDelegate* sceneDelegate,
        HdRenderParam* renderParam,
        HdDirtyBits* dirtyBits) override;

    void Finalize(HdRenderParam* renderParam) override;

    // Accessor for shader path
    [[nodiscard]] const std::string& GetShaderPath() const
    {
        return shader_path;
    }
    [[nodiscard]] bool HasValidShader() const
    {
        return has_valid_shader;
    }

   private:
    pxr::SdfAssetPath textureFileName;
    GfVec3f radiance;

    InputDescriptor env_texture;

    // Path to custom callable shader for miss shader
    std::string shader_path;
    bool has_valid_shader =
        false;  // True only if shader_path points to a valid file
};

RUZINO_NAMESPACE_CLOSE_SCOPE
