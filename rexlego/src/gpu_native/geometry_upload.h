#pragma once

#include <memory>
#include <plume_d3d12.h>

namespace legodimensions::gpu_native {

inline std::shared_ptr<plume::RenderBuffer> CopyGeometryToDevice(
    plume::RenderDevice* device, plume::RenderCommandList* commands,
    plume::RenderBuffer* source, uint32_t bytes, bool index) {
  if (!device || !commands || !source || !bytes) return {};
  const auto desc = index
      ? plume::RenderBufferDesc::IndexBuffer(bytes, plume::RenderHeapType::DEFAULT)
      : plume::RenderBufferDesc::VertexBuffer(bytes, plume::RenderHeapType::DEFAULT);
  auto destination = device->createBuffer(desc);
  if (!destination || !static_cast<plume::D3D12Buffer*>(destination.get())->d3d) return {};
  destination->setName(index ? "LEGO resident index buffer" : "LEGO resident vertex buffer");
  commands->barriers(plume::RenderBarrierStage::COPY,
      plume::RenderBufferBarrier(destination.get(), plume::RenderBufferAccess::WRITE));
  commands->copyBufferRegion(destination->at(0), source->at(0), bytes);
  commands->barriers(plume::RenderBarrierStage::GRAPHICS,
      plume::RenderBufferBarrier(destination.get(), plume::RenderBufferAccess::READ));
  return destination;
}

} // namespace legodimensions::gpu_native
