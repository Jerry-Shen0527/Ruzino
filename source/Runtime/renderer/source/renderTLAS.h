#pragma once
#include "api.h"
#include "geometries/mesh.h"
#include "nvrhi/nvrhi.h"

// SceneTypes
#include "../nodes/shaders/shaders/Scene/BindlessMaterial.slang"
#include "../nodes/shaders/shaders/Scene/Lights/LightData.slang"
#include "../nodes/shaders/shaders/Scene/SceneTypes.slang"
#include "internal/memory/DeviceMemoryPool.hpp"

RUZINO_NAMESPACE_OPEN_SCOPE

class HD_RUZINO_API Hd_RUZINO_RenderInstanceCollection {
   public:
    explicit Hd_RUZINO_RenderInstanceCollection();
    ~Hd_RUZINO_RenderInstanceCollection();

    nvrhi::rt::IAccelStruct* get_tlas();
    DescriptorTableManager* get_buffer_descriptor_table() const
    {
        return bindlessData.bufferDescriptorTableManager.get();
    }
    DescriptorTableManager* get_texture_descriptor_table() const
    {
        return bindlessData.textureDescriptorTableManager.get();
    }

    // DeviceMemoryPool<unsigned> index_pool;
    // DeviceMemoryPool<float> vertex_pool;
    DeviceMemoryPool<GeometryInstanceData> instance_pool;
    DeviceMemoryPool<nvrhi::rt::InstanceDesc> rt_instance_pool;
    DeviceMemoryPool<MeshDesc> mesh_pool;
    DeviceMemoryPool<VolumeDesc> volume_pool;
    DeviceMemoryPool<MaterialDataBlob> material_pool;
    DeviceMemoryPool<MaterialHeader> material_header_pool;
    DeviceMemoryPool<nvrhi::DrawIndirectArguments> draw_indirect_pool;
    DeviceMemoryPool<LightData> light_pool;
    CommandListHandle command_list;

    struct BindlessData {
        BindlessData();
        std::shared_ptr<DescriptorTableManager> bufferDescriptorTableManager;
        std::shared_ptr<DescriptorTableManager> textureDescriptorTableManager;

        nvrhi::BindingLayoutHandle bufferBindlessLayout;
        nvrhi::BindingLayoutHandle textureBindlessLayout;
    };
    BindlessData bindlessData;

    void set_require_rebuild_tlas()
    {
        require_rebuild_tlas = true;
    }

    bool get_require_rebuild_tlas() const
    {
        return require_rebuild_tlas;
    }

    // Track material changes
    uint32_t material_version = 0;
    void mark_materials_dirty()
    {
        material_version++;
    }
    uint32_t get_material_version() const
    {
        return material_version;
    }

    // Track geometry/buffer changes
    uint32_t geometry_version = 0;
    void mark_geometry_dirty()
    {
        geometry_version++;
    }
    uint32_t get_geometry_version() const
    {
        return geometry_version;
    }

    // track light changes
    uint32_t light_version = 0;
    void mark_lights_dirty()
    {
        light_version++;
    }
    uint32_t get_light_version() const
    {
        return light_version;
    }

   private:
    nvrhi::rt::AccelStructHandle TLAS;

    bool require_rebuild_tlas = true;
    void rebuild_tlas();
};

RUZINO_NAMESPACE_CLOSE_SCOPE
