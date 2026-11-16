#include "gpu_native/temporal_scene_depth.h"
#include "gpu_native/device.h"
#include "gpu_native/textures.h"
#include <plume_render_interface.h>

namespace legodimensions::gpu_native {
TemporalSceneDepth CaptureTemporalSceneDepth(uint32_t depth_surface,
    uint32_t width, uint32_t height, uint64_t frame,
    const TemporalSceneDepth& reusable) {
  auto recording = HostDevice::LockRecording();
  const auto source = ResolveTextureResource(depth_surface);
  if (!source.texture || !source.depth || source.width != width || source.height != height ||
      source.descriptor_index == ~uint32_t{0}) return {};
  auto* device = HostDevice::Device();
  if (!device) return {};
  plume::RenderTextureDesc desc;
  desc.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  desc.width = width; desc.height = height;
  desc.depth = desc.mipLevels = desc.arraySize = 1;
  desc.multisampling.sampleCount = plume::RenderSampleCount::COUNT_1;
  desc.format = plume::RenderFormat::R32_FLOAT;
  desc.flags |= plume::RenderTextureFlag::RENDER_TARGET;
  desc.committed = true;
  // All copies and FSR reads are ordered on the same direct queue. Reuse
  // storage across frames, but publish the new frame only after a valid copy.
  std::shared_ptr<plume::RenderTexture> snapshot =
      reusable.width == width && reusable.height == height &&
      reusable.texture.get() != source.texture ? reusable.texture : nullptr;
  if (!snapshot) snapshot = device->createTexture(desc);
  if (!snapshot) return {};
  snapshot->setName("LEGO temporal scene depth snapshot");
  if (!HostDevice::ResolveHdrColor(source.texture, source.descriptor_index,
      snapshot.get(), width, height, 1.0f, ColorResolveDestination::kDepthFloat32, true)) {
    HostDevice::RetireResource(std::move(snapshot));
    return {};
  }
  return {std::move(snapshot), width, height, frame};
}
}  // namespace legodimensions::gpu_native
