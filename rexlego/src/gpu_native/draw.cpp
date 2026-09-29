#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>
#include <rex/graphics/xenos.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

#include "gpu_native/buffers.h"
#include "gpu_native/d3d.h"
#include "gpu_native/device.h"
#include "gpu_native/shaders.h"
#include "gpu_native/state.h"
#include "gpu_native/textures.h"
#include "gpu_native/vertex_declarations.h"

namespace legodimensions::gpu_native {
namespace {

struct SharedConstants {
  u32 texture_2d[16]{};
  u32 texture_3d[16]{};
  u32 texture_cube[16]{};
  u32 samplers[16]{};
  u32 booleans[8]{};
  u32 swapped_texcoords = 0;
  float half_pixel_x = 0.0f;
  float half_pixel_y = 0.0f;
  float alpha_threshold = 0.0f;
  u32 swapped_normals = 0;
  u32 swapped_binormals = 0;
  u32 swapped_tangents = 0;
  u32 swapped_blend_weights = 0;
  u32 swapped_positions = 0;
  u32 sint_texcoords = 0;
  float shadow_pcf_scale = 1.0f;
  u32 padding = 0;
  float blit_half_pixel_x = 0.0f;
  float blit_half_pixel_y = 0.0f;
  u32 padding_2[2]{};
};
static_assert(sizeof(SharedConstants) == 352);

struct PipelineKey {
  u32 vertex_shader = 0;
  u32 pixel_shader = 0;
  u32 declaration = 0;
  u32 render_target_format = 0;
  u32 depth_format = 0;
  u32 topology = 0;
  u32 spec_constants = 0;
  std::array<u32, kNativeVertexStreams> strides{};
  bool operator==(const PipelineKey&) const = default;
};

struct PipelineKeyHash {
  size_t operator()(const PipelineKey& key) const {
    size_t hash = 1469598103934665603ull;
    const auto* bytes = reinterpret_cast<const u8*>(&key);
    for (size_t i = 0; i < sizeof(key); ++i) {
      hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
    return hash;
  }
};

std::mutex g_pipeline_mutex;
std::unordered_map<PipelineKey, std::unique_ptr<plume::RenderPipeline>,
                   PipelineKeyHash>
    g_pipelines;
u32 g_unsupported_draw_logs = 0;

bool MapTopology(u32 primitive_type, plume::RenderPrimitiveTopology& result) {
  using rex::graphics::xenos::PrimitiveType;
  switch (static_cast<PrimitiveType>(primitive_type)) {
    case PrimitiveType::kPointList:
      result = plume::RenderPrimitiveTopology::POINT_LIST;
      return true;
    case PrimitiveType::kLineList:
      result = plume::RenderPrimitiveTopology::LINE_LIST;
      return true;
    case PrimitiveType::kLineStrip:
      result = plume::RenderPrimitiveTopology::LINE_STRIP;
      return true;
    case PrimitiveType::kTriangleList:
      result = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
      return true;
    case PrimitiveType::kTriangleStrip:
      result = plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
      return true;
    default:
      return false;
  }
}

void CopyBigEndianDwords(void* destination, const void* source, u32 size) {
  auto* dst = static_cast<u32*>(destination);
  const auto* src = static_cast<const be_u32*>(source);
  for (u32 i = 0; i < size / sizeof(u32); ++i) {
    dst[i] = src[i];
  }
}

std::shared_ptr<plume::RenderBuffer> BindConstants(
    plume::RenderCommandList* commands, const D3DDevice* device,
    const DrawBindings& bindings, const VertexDeclarationView& declaration) {
  constexpr u32 kConstantsSize = 0x1000;
  constexpr u32 kSharedOffset = kConstantsSize * 2;
  auto* host = HostDevice::Device();
  if (!host) {
    return {};
  }
  auto upload = std::shared_ptr<plume::RenderBuffer>(
      host->createBuffer(plume::RenderBufferDesc::UploadBuffer(
                             kSharedOffset + sizeof(SharedConstants)))
          .release());
  if (!upload) {
    return {};
  }
  auto* mapped = static_cast<u8*>(upload->map());
  if (!mapped) {
    return {};
  }
  const auto* bytes = reinterpret_cast<const u8*>(device);
  CopyBigEndianDwords(mapped, bytes + 0x700, kConstantsSize);
  CopyBigEndianDwords(mapped + kConstantsSize, bytes + 0x1700,
                      kConstantsSize);

  SharedConstants shared;
  std::fill(std::begin(shared.texture_3d), std::end(shared.texture_3d), 1u);
  std::fill(std::begin(shared.texture_cube), std::end(shared.texture_cube), 2u);
  const auto* bools = reinterpret_cast<const be_u32*>(bytes + 0x2700);
  for (u32 i = 0; i < 8; ++i) {
    shared.booleans[i] = bools[i];
  }
  shared.swapped_texcoords = declaration.swapped_texcoords;
  shared.swapped_normals = declaration.swapped_normals;
  shared.swapped_binormals = declaration.swapped_binormals;
  shared.swapped_tangents = declaration.swapped_tangents;
  shared.swapped_blend_weights = declaration.swapped_blend_weights;
  shared.swapped_positions = declaration.swapped_positions;
  shared.sint_texcoords = declaration.sint_texcoords;
  for (u32 i = 0; i < kNativeTextureSlots; ++i) {
    const auto texture = ResolveTextureResource(bindings.textures[i]);
    if (!texture.texture || texture.descriptor_index == ~u32{0}) {
      continue;
    }
    UploadTextureResource(bindings.textures[i], commands);
    if (texture.d3d_type ==
        static_cast<u32>(D3DResourceType::kVolumeTexture)) {
      shared.texture_3d[i] = texture.descriptor_index;
    } else if (texture.d3d_type ==
               static_cast<u32>(D3DResourceType::kCubeTexture)) {
      shared.texture_cube[i] = texture.descriptor_index;
    } else {
      shared.texture_2d[i] = texture.descriptor_index;
    }
  }
  std::memcpy(mapped + kSharedOffset, &shared, sizeof(shared));
  upload->unmap();
  commands->setGraphicsRootDescriptor(upload->at(0), 0);
  commands->setGraphicsRootDescriptor(upload->at(kConstantsSize), 1);
  commands->setGraphicsRootDescriptor(upload->at(kSharedOffset), 2);
  return upload;
}

plume::RenderPipeline* GetPipeline(
    const PipelineKey& key, const VertexDeclarationView& declaration,
    plume::RenderPrimitiveTopology topology, plume::RenderFormat color_format,
    plume::RenderFormat depth_format) {
  std::lock_guard lock(g_pipeline_mutex);
  if (const auto it = g_pipelines.find(key); it != g_pipelines.end()) {
    return it->second.get();
  }
  auto* vs = ResolveBoundShader(ShaderStage::kVertex, key.spec_constants);
  auto* ps = ResolveBoundShader(ShaderStage::kPixel, key.spec_constants);
  if (!vs || (key.pixel_shader && !ps)) {
    return nullptr;
  }
  std::array<plume::RenderInputSlot, kNativeVertexStreams> slots;
  for (u32 i = 0; i < slots.size(); ++i) {
    slots[i] = plume::RenderInputSlot(i, key.strides[i] ? key.strides[i] : 16);
  }
  plume::RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = HostDevice::PipelineLayout();
  desc.vertexShader = vs;
  desc.pixelShader = ps;
  desc.depthFunction = plume::RenderComparisonFunction::LESS_EQUAL;
  desc.depthEnabled = depth_format != plume::RenderFormat::UNKNOWN;
  desc.depthWriteEnabled = desc.depthEnabled;
  desc.depthClipEnabled = true;
  desc.primitiveTopology = topology;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.fillMode = plume::RenderFillMode::SOLID;
  desc.renderTargetCount = color_format == plume::RenderFormat::UNKNOWN ? 0 : 1;
  desc.renderTargetFormat[0] = color_format;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.depthTargetFormat = depth_format;
  desc.inputElements = declaration.elements;
  desc.inputElementsCount = declaration.element_count;
  desc.inputSlots = slots.data();
  desc.inputSlotsCount = static_cast<u32>(slots.size());
  auto pipeline = HostDevice::Device()->createGraphicsPipeline(desc);
  if (!pipeline) {
    return nullptr;
  }
  auto* result = pipeline.get();
  g_pipelines.emplace(key, std::move(pipeline));
  return result;
}

bool DispatchDraw(D3DDevice* device, u32 primitive_type, bool indexed,
                  u32 start, u32 count, i32 base_vertex) {
  if (!device || !count) {
    return false;
  }
  plume::RenderPrimitiveTopology topology;
  if (!MapTopology(primitive_type, topology)) {
    if (g_unsupported_draw_logs++ < 20) {
      REXLOG_WARN("Native GPU: skipped unsupported primitive type {}",
                  primitive_type);
    }
    return false;
  }
  const DrawBindings bindings = SnapshotDrawBindings();
  const auto declaration = ResolveVertexDeclaration(bindings.vertex_declaration);
  const auto color = ResolveTextureResource(bindings.render_targets[0]);
  const auto depth = ResolveTextureResource(bindings.depth_stencil);
  auto* framebuffer = ResolveFramebuffer(bindings.render_targets[0],
                                         bindings.depth_stencil);
  if (!declaration.supported || !declaration.elements || !framebuffer ||
      (!color.texture && !depth.texture)) {
    if (g_unsupported_draw_logs++ < 20) {
      REXLOG_WARN("Native GPU: skipped draw with incomplete bindings "
                  "(decl=0x{:08X}, rt=0x{:08X}, ds=0x{:08X})",
                  bindings.vertex_declaration, bindings.render_targets[0],
                  bindings.depth_stencil);
    }
    return false;
  }
  auto* commands = HostDevice::BeginFrameCommands();
  if (!commands) {
    return false;
  }

  std::array<plume::RenderTextureBarrier, 2> texture_barriers;
  u32 barrier_count = 0;
  if (color.texture) {
    texture_barriers[barrier_count++] = plume::RenderTextureBarrier(
        color.texture, plume::RenderTextureLayout::COLOR_WRITE);
  }
  if (depth.texture) {
    texture_barriers[barrier_count++] = plume::RenderTextureBarrier(
        depth.texture, plume::RenderTextureLayout::DEPTH_WRITE);
  }
  commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                     texture_barriers.data(), barrier_count);
  commands->setFramebuffer(framebuffer);

  PipelineKey key;
  key.vertex_shader = BoundShaderAddress(ShaderStage::kVertex);
  key.pixel_shader = BoundShaderAddress(ShaderStage::kPixel);
  key.declaration = bindings.vertex_declaration;
  key.render_target_format = static_cast<u32>(color.format);
  key.depth_format = static_cast<u32>(depth.format);
  key.topology = static_cast<u32>(topology);
  key.spec_constants = declaration.has_r11g11b10_normal ? 1u : 0u;
  for (u32 i = 0; i < kNativeVertexStreams; ++i) {
    key.strides[i] = bindings.vertex_streams[i].stride;
  }
  auto* pipeline = GetPipeline(key, declaration, topology, color.format,
                               depth.format);
  if (!pipeline) {
    return false;
  }

  commands->setGraphicsPipelineLayout(HostDevice::PipelineLayout());
  commands->setGraphicsDescriptorSet(HostDevice::TextureDescriptorSet(), 0);
  commands->setGraphicsDescriptorSet(HostDevice::TextureDescriptorSet(), 1);
  commands->setGraphicsDescriptorSet(HostDevice::TextureDescriptorSet(), 2);
  commands->setGraphicsDescriptorSet(HostDevice::SamplerDescriptorSet(), 3);
  auto constants = BindConstants(commands, device, bindings, declaration);
  if (!constants) {
    return false;
  }
  commands->setPipeline(pipeline);

  const float viewport_width = static_cast<u32>(device->viewport.width);
  const float viewport_height = static_cast<u32>(device->viewport.height);
  commands->setViewports(plume::RenderViewport(
      static_cast<u32>(device->viewport.x), static_cast<u32>(device->viewport.y),
      viewport_width > 0 ? viewport_width : float(color.width),
      viewport_height > 0 ? viewport_height : float(color.height),
      static_cast<float>(device->viewport.min_z),
      static_cast<float>(device->viewport.max_z)));
  const i32 scissor_right = static_cast<i32>(device->scissor.right);
  const i32 scissor_bottom = static_cast<i32>(device->scissor.bottom);
  commands->setScissors(plume::RenderRect(
      static_cast<i32>(device->scissor.left),
      static_cast<i32>(device->scissor.top),
      scissor_right > 0 ? scissor_right : static_cast<i32>(color.width),
      scissor_bottom > 0 ? scissor_bottom : static_cast<i32>(color.height)));

  std::array<plume::RenderVertexBufferView, kNativeVertexStreams> views;
  std::array<plume::RenderInputSlot, kNativeVertexStreams> slots;
  auto* null_buffer = HostDevice::NullVertexBuffer();
  for (u32 i = 0; i < kNativeVertexStreams; ++i) {
    const auto& binding = bindings.vertex_streams[i];
    const auto buffer = ResolveBufferResourceView(binding.buffer,
                                                  BufferKind::kVertex);
    const u32 offset = std::min(binding.offset, buffer.length);
    views[i] = buffer.buffer
                   ? plume::RenderVertexBufferView(buffer.buffer->at(offset),
                                                   buffer.length - offset)
                   : plume::RenderVertexBufferView(null_buffer->at(0), 256);
    slots[i] = plume::RenderInputSlot(i, binding.stride ? binding.stride : 16);
  }
  commands->setVertexBuffers(0, views.data(), views.size(), slots.data());

  if (indexed) {
    const auto index = ResolveBufferResourceView(bindings.index_buffer,
                                                 BufferKind::kIndex);
    if (!index.buffer) {
      return false;
    }
    const plume::RenderFormat index_format =
        index.guest_format == 1 ? plume::RenderFormat::R16_UINT
                                : plume::RenderFormat::R32_UINT;
    const plume::RenderIndexBufferView index_view(index.buffer->at(0),
                                                  index.length, index_format);
    commands->setIndexBuffer(&index_view);
    commands->drawIndexedInstanced(count, 1, start, base_vertex, 0);
  } else {
    commands->drawInstanced(count, 1, start, 0);
  }
  HostDevice::RetireResource(std::move(constants));
  return true;
}

u32 ClearHook(D3DDevice* /*device*/, u32 rectangle_count,
              u32 rectangles_address, u32 flags, u32 color, f64 depth,
              u32 /*depth_gpr_slot*/, u32 stencil, u32 /*edram_clear*/) {
  const DrawBindings bindings = SnapshotDrawBindings();
  const auto target = ResolveTextureResource(bindings.render_targets[0]);
  const auto depth_target = ResolveTextureResource(bindings.depth_stencil);
  auto* framebuffer = ResolveFramebuffer(bindings.render_targets[0],
                                         bindings.depth_stencil);
  auto* commands = HostDevice::BeginFrameCommands();
  if (!framebuffer || !commands) {
    return 0;
  }
  std::array<plume::RenderTextureBarrier, 2> barriers;
  u32 barrier_count = 0;
  if (target.texture) {
    barriers[barrier_count++] = plume::RenderTextureBarrier(
        target.texture, plume::RenderTextureLayout::COLOR_WRITE);
  }
  if (depth_target.texture) {
    barriers[barrier_count++] = plume::RenderTextureBarrier(
        depth_target.texture, plume::RenderTextureLayout::DEPTH_WRITE);
  }
  commands->barriers(plume::RenderBarrierStage::GRAPHICS, barriers.data(),
                     barrier_count);
  commands->setFramebuffer(framebuffer);

  std::vector<plume::RenderRect> rectangles;
  if (rectangle_count && rectangles_address) {
    const auto* source = reinterpret_cast<const D3DRect*>(
        REX_KERNEL_MEMORY()->virtual_membase() + rectangles_address);
    rectangles.reserve(rectangle_count);
    for (u32 i = 0; i < rectangle_count; ++i) {
      rectangles.emplace_back(static_cast<i32>(source[i].left),
                              static_cast<i32>(source[i].top),
                              static_cast<i32>(source[i].right),
                              static_cast<i32>(source[i].bottom));
    }
  }
  const plume::RenderRect* clear_rectangles =
      rectangles.empty() ? nullptr : rectangles.data();
  const u32 clear_rectangle_count = static_cast<u32>(rectangles.size());
  if ((flags & 0x1) && target.texture) {
    const float scale = 1.0f / 255.0f;
    commands->clearColor(
        0,
        plume::RenderColor(float((color >> 16) & 0xFF) * scale,
                           float((color >> 8) & 0xFF) * scale,
                           float(color & 0xFF) * scale,
                           float((color >> 24) & 0xFF) * scale),
        clear_rectangles, clear_rectangle_count);
  }
  if ((flags & 0x30) && depth_target.texture) {
    commands->clearDepthStencil((flags & 0x10) != 0, (flags & 0x20) != 0,
                                static_cast<float>(depth), stencil,
                                clear_rectangles, clear_rectangle_count);
  }
  return 0;
}

u32 DrawVerticesHook(D3DDevice* device, u32 primitive_type, u32 start_vertex,
                     u32 vertex_count) {
  DispatchDraw(device, primitive_type, false, start_vertex, vertex_count, 0);
  return 0;
}

u32 DrawIndexedVerticesHook(D3DDevice* device, u32 primitive_type,
                            u32 base_vertex, u32 start_index, u32 index_count) {
  DispatchDraw(device, primitive_type, true, start_index, index_count,
               static_cast<i32>(base_vertex));
  return 0;
}

u32 ResolveHook(D3DDevice* /*device*/, u32 /*flags*/, u32 /*source_rect*/,
                u32 destination_texture, u32 /*destination_point*/,
                u32 destination_level, u32 destination_slice,
                u32 /*clear_color*/, u32 /*clear_z_hi*/, u32 /*clear_z_lo*/,
                u32 /*clear_stencil*/, u32 /*parameters*/) {
  if (!ResolveTextureFromSurface(destination_texture, BoundRenderTarget(0),
                                 destination_level, destination_slice) &&
      g_unsupported_draw_logs++ < 20) {
    REXLOG_WARN("Native GPU: skipped unsupported resolve to 0x{:08X}",
                destination_texture);
  }
  return 0;
}

u32 SwapHook(D3DDevice* /*device*/, u32 front_buffer, u32 /*parameters*/) {
  const auto texture = ResolveTextureResource(front_buffer);
  if (!HostDevice::PresentTexture(texture.texture, texture.descriptor_index) &&
      g_unsupported_draw_logs++ < 20) {
    REXLOG_WARN("Native GPU: skipped present for front buffer 0x{:08X}",
                front_buffer);
  }
  return 0;
}

}  // namespace

void ResetDrawResources() {
  std::lock_guard lock(g_pipeline_mutex);
  g_pipelines.clear();
}

}  // namespace legodimensions::gpu_native

REX_HOOK(sub_83FC6640, legodimensions::gpu_native::DrawVerticesHook);
REX_HOOK(sub_83FC6A58, legodimensions::gpu_native::DrawIndexedVerticesHook);
REX_HOOK(sub_83FBF0D8, legodimensions::gpu_native::ResolveHook);
REX_HOOK(sub_83FB33E0, legodimensions::gpu_native::SwapHook);
REX_HOOK(sub_83FBC9D8, legodimensions::gpu_native::ClearHook);
