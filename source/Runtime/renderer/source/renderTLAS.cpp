#include "renderTLAS.h"

#include "RHI/rhi.hpp"

RUZINO_NAMESPACE_OPEN_SCOPE

Hd_RUZINO_RenderInstanceCollection::Hd_RUZINO_RenderInstanceCollection()
    : rt_instance_pool(
          BufferDesc{}.setIsAccelStructBuildInput(true).setCanHaveRawViews(
              true))
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
    // rt_instance_pool is a stable-slot arena now: entries are never moved
    // (nothing may relocate data that rt_instance.instanceID /
    // GeometryInstanceData references point at, and instancer.slang writes
    // rows in place on the GPU). Contiguity for the TLAS build input is
    // produced by gathering live rows into a staging buffer below, NOT by
    // compressing the pool.
    if (rt_instance_pool.take_pending_changes()) {
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
    // Gather the live instance descs into a dense staging array. The
    // upload and the AS build are recorded on the SAME command list, so
    // same-queue ordering makes the data visible to the build without any
    // cross-queue fence (the pool's Copy-queue write_data would race the
    // build here — same family as the black-frame race documented in
    // test_raster_mutation_consistency).
    auto gathered = rt_instance_pool.gather_active();
    const size_t count = gathered.size();

    auto nvrhi_device = RHI::get_device();
    if (!command_list)
        command_list = nvrhi_device->createCommandList();

    if (count > 0) {
        const size_t needed = count * sizeof(nvrhi::rt::InstanceDesc);
        if (!tlas_instance_staging ||
            tlas_instance_staging->getDesc().byteSize < needed) {
            nvrhi::BufferDesc staging_desc;
            staging_desc.structStride = sizeof(nvrhi::rt::InstanceDesc);
            staging_desc.byteSize =
                std::max<size_t>(needed, 64 * sizeof(nvrhi::rt::InstanceDesc));
            staging_desc.setIsAccelStructBuildInput(true);
            staging_desc.setCanHaveRawViews(true);
            staging_desc.keepInitialState = true;
            staging_desc.debugName = "TLASInstanceStaging";
            tlas_instance_staging = nvrhi_device->createBuffer(staging_desc);
        }

        command_list->open();
        command_list->writeBuffer(
            tlas_instance_staging, gathered.data(), needed, 0);
        command_list->beginMarker("TLAS Update");
        command_list->buildTopLevelAccelStructFromBuffer(
            TLAS, tlas_instance_staging, 0, static_cast<uint32_t>(count));
        command_list->endMarker();
        command_list->close();
    }
    else {
        // Empty scene: build with an empty input so the TLAS exists and
        // rays miss cleanly.
        command_list->open();
        command_list->beginMarker("TLAS Update (empty)");
        command_list->buildTopLevelAccelStructFromBuffer(TLAS, nullptr, 0, 0);
        command_list->endMarker();
        command_list->close();
    }

    nvrhi_device->executeCommandList(command_list);
    nvrhi_device->waitForIdle();
}

RUZINO_NAMESPACE_CLOSE_SCOPE
