#include "renderTLAS.h"

#include "RHI/rhi.hpp"

RUZINO_NAMESPACE_OPEN_SCOPE

Hd_RUZINO_RenderInstanceCollection::Hd_RUZINO_RenderInstanceCollection()
    : rt_instance_pool(
          BufferDesc{}.setIsAccelStructBuildInput(true).setCanHaveRawViews(
              true)),
      draw_indirect_pool(BufferDesc{}.setIsDrawIndirectArgs(true))
{
    nvrhi::rt::AccelStructDesc tlasDesc;
    tlasDesc.isTopLevel = true;
    tlasDesc.topLevelMaxInstances = 1024 * 1024;
    TLAS = RHI::get_device()->createAccelStruct(tlasDesc);
}

Hd_RUZINO_RenderInstanceCollection::~Hd_RUZINO_RenderInstanceCollection()
{
}

nvrhi::rt::IAccelStruct* Hd_RUZINO_RenderInstanceCollection::get_tlas()
{
    if (rt_instance_pool.compress()) {
        require_rebuild_tlas = true;
    }
    if (require_rebuild_tlas) {
        rebuild_tlas();
        require_rebuild_tlas = false;
    }

    return TLAS;
}

Hd_RUZINO_RenderInstanceCollection::BindlessData::BindlessData()
{
    auto device = RHI::get_device();
    nvrhi::BindlessLayoutDesc buffer_layout_desc;
    buffer_layout_desc.visibility = nvrhi::ShaderType::All;
    buffer_layout_desc.maxCapacity = 8 * 1024;
    buffer_layout_desc.addRegisterSpace(
        nvrhi::BindingLayoutItem::RawBuffer_SRV(1));
    bufferBindlessLayout = device->createBindlessLayout(buffer_layout_desc);
    bufferDescriptorTableManager =
        std::make_shared<DescriptorTableManager>(device, bufferBindlessLayout);

    nvrhi::BindlessLayoutDesc texture_layout_desc;
    texture_layout_desc.visibility = nvrhi::ShaderType::All;
    texture_layout_desc.maxCapacity = 8 * 1024;
    texture_layout_desc.addRegisterSpace(
        nvrhi::BindingLayoutItem::Texture_SRV(2));
    textureBindlessLayout = device->createBindlessLayout(texture_layout_desc);

    textureDescriptorTableManager =
        std::make_shared<DescriptorTableManager>(device, textureBindlessLayout);
}

void Hd_RUZINO_RenderInstanceCollection::rebuild_tlas()
{
    auto nvrhi_device = RHI::get_device();

    if (!command_list)
        command_list = nvrhi_device->createCommandList();
    command_list->open();
    command_list->beginMarker("TLAS Update");
    command_list->buildTopLevelAccelStructFromBuffer(
        TLAS,
        rt_instance_pool.get_device_buffer(),
        0,
        rt_instance_pool.count());
    command_list->endMarker();
    command_list->close();
    nvrhi_device->executeCommandList(command_list);
    nvrhi_device->waitForIdle();
}

RUZINO_NAMESPACE_CLOSE_SCOPE
