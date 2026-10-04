#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <plume_render_interface.h>
#include <plume_d3d12.h>
#include <rex/graphics/xenos.h>
#include "gpu_native/renderer_route.h"
#include <rex/logging.h>
#include <rex/cvar.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include "gpu_native/buffers.h"
#include "gpu_native/buffer_window.h"
#include "gpu_native/alpha_test.h"
#include "gpu_native/blend_state.h"
#include "gpu_native/queries.h"
#include "gpu_native/d3d.h"
#include "gpu_native/draw.h"
#include "gpu_native/depth_state.h"
#include "gpu_native/stencil_state.h"
#include "gpu_native/device.h"
#include "gpu_native/shaders.h"
#include "gpu_native/state.h"
#include "gpu_native/textures.h"
#include "gpu_native/tile_extent.h"
#include "gpu_native/tonemap_dof.h"
#include "gpu_native/portrait_probe.h"
#include "gpu_native/long_probe.h"
#include "gpu_native/renderdoc_probe.h"
#include "gpu_native/vertex_declarations.h"

REXCVAR_DEFINE_BOOL(gpu_native_buffer_windows, false, "GPU",
    "Upload and retain only the vertex buffer ranges fetched by native draws")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace legodimensions::gpu_native {
namespace {

struct SharedConstants {
  u32 texture_2d[32]{};
  u32 texture_3d[32]{};
  u32 texture_cube[32]{};
  u32 samplers[32]{};
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
  float viewport_scale_x = 0.0f;
  float viewport_scale_y = 0.0f;
  u32 alpha_function = 7;
  u32 viewport_mode = 0;
  float color_output_scale[4]{1, 1, 1, 1};
  float fetch_lod_bias[32]{};
};
static_assert(sizeof(SharedConstants) == 752);
static_assert(offsetof(SharedConstants, fetch_lod_bias) == 624);
static_assert(offsetof(SharedConstants, booleans) == 512);
static_assert(offsetof(SharedConstants, color_output_scale) == 608);
static_assert(offsetof(SharedConstants, alpha_function) == 600);
static_assert(offsetof(SharedConstants, viewport_scale_x) == 592);
static_assert(offsetof(SharedConstants, viewport_mode) == 604);
static_assert(offsetof(SharedConstants, alpha_threshold) == 556);

struct PipelineKey {
  u32 vertex_shader = 0;
  u32 pixel_shader = 0;
  u32 declaration = 0;
  // Guest allocation addresses may be reused after Release or XGRegister.
  // Pipeline identity must follow the actual shader and input-layout contents.
  u64 vertex_shader_hash = 0;
  u64 pixel_shader_hash = 0;
  u64 declaration_hash = 0;
  std::array<u32, kNativeRenderTargets> render_target_formats{};
  u32 depth_format = 0;
  u32 depth_control = 0;
  u32 clip_disable = 0;
  u32 stencil_masks = 0;
  u32 stencil_face = 0;
  u32 color_write_mask = 0;
  std::array<u32, kNativeRenderTargets> blend_controls{0x10001, 0x10001, 0x10001, 0x10001};
  u32 topology = 0;
  u32 spec_constants = 0;
  std::array<u32, kNativeVertexStreams> strides{};
  bool operator==(const PipelineKey&) const = default;
};

struct PipelineKeyHash {
  size_t operator()(const PipelineKey& key) const {
    size_t hash = 1469598103934665603ull;
    const auto mix = [&](u64 value) { hash = (hash ^ value) * 1099511628211ull; };
    mix(key.vertex_shader); mix(key.pixel_shader); mix(key.declaration);
    mix(key.vertex_shader_hash); mix(key.pixel_shader_hash); mix(key.declaration_hash);
    for (const auto format : key.render_target_formats) mix(format);
    mix(key.depth_format); mix(key.depth_control); mix(key.color_write_mask); mix(key.clip_disable);
    mix(key.stencil_masks); mix(key.stencil_face);
    for (const auto blend : key.blend_controls) mix(blend);
    mix(key.topology); mix(key.spec_constants);
    for (const auto stride : key.strides) mix(stride);
    return hash;
  }
};

std::mutex g_pipeline_mutex;
std::unordered_map<PipelineKey, std::unique_ptr<plume::RenderPipeline>,
                   PipelineKeyHash>
    g_pipelines;
u32 g_unsupported_draw_logs = 0;
DrawTiming g_draw_timing;
bool g_seen_tiled_pass = false; // Serialized by LockRecording; diagnostics only.

struct DrawTimer {
  bool enabled = NativeTextureTimingEnabled();
  size_t stage = 0;
  std::chrono::steady_clock::time_point last;
  std::array<double, 7> elapsed{};
  DrawTimer() {
    if (enabled) {
      ++g_draw_timing.calls;
      last = std::chrono::steady_clock::now();
    }
  }
  void Next() {
    if (!enabled) return;
    const auto now = std::chrono::steady_clock::now();
    elapsed[stage] += std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    ++stage;
  }
  ~DrawTimer() {
    if (!enabled) return;
    Next();
    double total = 0;
    for (size_t i = 0; i < elapsed.size(); ++i) {
      g_draw_timing.stages_ms[i] += elapsed[i];
      total += elapsed[i];
    }
    static u32 slow_draws = 0;
    if (total > 250.0 && slow_draws++ < 24)
      REXLOG_INFO("Native slow draw: total={:.2f}ms bindings={:.2f} begin={:.2f} "
          "pipeline={:.2f} constants={:.2f} vertices={:.2f} issue={:.2f} tail={:.2f}",
          total, elapsed[0], elapsed[1], elapsed[2], elapsed[3], elapsed[4],
          elapsed[5], elapsed[6]);
  }
};

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
    case PrimitiveType::kQuadList:
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

bool BindConstants(
    plume::RenderCommandList* commands, const D3DDevice* device,
    const DrawBindings& bindings, const VertexDeclarationView& declaration, bool capture_constants) {
  constexpr u32 kConstantsSize = 0x1000;
  constexpr u32 kSharedOffset = kConstantsSize * 2;
  DrawUploadSlice upload;
  std::shared_ptr<plume::RenderBuffer> separate_upload;
  static const bool separate_constants = std::getenv("LEGO_NATIVE_NO_DRAW_ARENA") != nullptr;
  if (separate_constants) {
    auto* host = HostDevice::Device();
    if (!host) return false;
    separate_upload = host->createBuffer(plume::RenderBufferDesc::UploadBuffer(
        kSharedOffset + sizeof(SharedConstants)));
    if (!separate_upload) return false;
    upload = {separate_upload.get(), separate_upload->map(), 0};
  } else upload = HostDevice::AllocateDrawUpload(kSharedOffset + sizeof(SharedConstants));
  if (!upload) return false;
  // Build in cacheable CPU memory. Persistently mapped UPLOAD storage is
  // write-combined and must never be read for diagnostics or partial updates.
  alignas(16) std::array<u8, kSharedOffset + sizeof(SharedConstants)> data;
  auto* mapped = data.data();
  const auto* bytes = reinterpret_cast<const u8*>(device);
  CopyBigEndianDwords(mapped, bytes + 0x780, kConstantsSize);
  CopyBigEndianDwords(mapped + kConstantsSize, bytes + 0x1780,
                      kConstantsSize);

  // Filtering the CoC pass alone leaves the tonemap's sharp/mip blend active.
  // Change only this draw's immutable host constants, never guest state.
  if (BoundShaderHash(ShaderStage::kPixel) == 0x3A47E5DDE66B42C6ull &&
      ApplyDisabledTonemapDof(BoundShaderHash(ShaderStage::kPixel),
          rex::cvar::GetFlagByName("depth_of_field") == "false",
          mapped + kConstantsSize, kConstantsSize)) {
    static bool logged_disabled_mix = false;
    if (!logged_disabled_mix) {
      logged_disabled_mix = true;
      REXLOG_INFO("Native GPU: disabled DoF sharp/mip tonemap mix");
    }
  }

  SharedConstants shared;
  // Isolated ABI624 candidate; requires the matching viewport shader banks.
  static const bool viewport_candidate = std::getenv("LEGO_NATIVE_VIEWPORT") != nullptr;
  if (viewport_candidate && u32(*reinterpret_cast<const be_u32*>(bytes + 10572)) == 0x400) {
    const auto target = ResolveTextureResource(bindings.render_targets[0]);
    if (!target.width || !target.height) return false;
    shared.viewport_mode = 1;
    shared.viewport_scale_x = 2.0f / target.width;
    shared.viewport_scale_y = -2.0f / target.height;
  }
  const auto alpha = DecodeNativeAlphaState(
      *reinterpret_cast<const be_u32*>(bytes + 10556),
      *reinterpret_cast<const be_f32*>(bytes + 10620));
  shared.alpha_function = alpha.function;
  shared.alpha_threshold = alpha.reference;
  std::fill(std::begin(shared.texture_3d), std::end(shared.texture_3d), 1u);
  std::fill(std::begin(shared.texture_cube), std::end(shared.texture_cube), 2u);
  const auto* bools = reinterpret_cast<const be_u32*>(bytes + 0x2780);
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
  for (u32 i = 0; i < kNativeRenderTargets; ++i)
    shared.color_output_scale[i] = SurfaceColorOutputScale(bindings.render_targets[i]);
  static const bool force_all_textures = std::getenv("LEGO_NATIVE_ALL_TEXTURES") != nullptr;
  const u32 texture_mask = force_all_textures ? ~u32{0} :
      BoundShaderTextureMask(ShaderStage::kVertex) | BoundShaderTextureMask(ShaderStage::kPixel);
  static const bool trace_constants = std::getenv("LEGO_GPU_SNAPSHOT_DIR") != nullptr;
  if (trace_constants && BoundShaderHash(ShaderStage::kPixel) == 0x3A47E5DDE66B42C6ull) {
    static u32 tonemap_logs = 0;
    if (tonemap_logs++ < 12) {
      const auto* vs = reinterpret_cast<const float*>(mapped);
      const auto* ps = reinterpret_cast<const float*>(mapped + kConstantsSize);
      for (u32 index : {4u, 10u, 11u, 12u})
        REXLOG_INFO("Native tonemap constants c{} VS={},{},{},{} PS={},{},{},{}", index,
            vs[index*4], vs[index*4+1], vs[index*4+2], vs[index*4+3],
            ps[index*4], ps[index*4+1], ps[index*4+2], ps[index*4+3]);
    }
  }
  for (u32 i = 0; i < kNativeTextureSlots; ++i) {
    if (!(texture_mask & (1u << i))) continue;
    const auto texture = ResolveTextureResource(bindings.textures[i]);
    if (!texture.texture || texture.descriptor_index == ~u32{0}) {
      if (LongProbeEnabled() && bindings.textures[i] &&
          LongProbeOnce(0xBAD1000000000000ull | bindings.textures[i]))
        LongProbeEvent("unavailable_bound_texture", true, "slot=", i,
            "guest=", bindings.textures[i]);
      continue;
    }
    // Vertex textures contain animation data. Until every writer participates
    // in invalidation, verify their contents at every use, even with watches.
    UploadTextureResource(bindings.textures[i], commands,
        (BoundShaderTextureMask(ShaderStage::kVertex) & (1u << i)) != 0);
    TextureFetchWords fetch;
    for (u32 word = 0; word < fetch.size(); ++word) fetch[word] = device->fetch_constants[i].dword[word];
    if (LongProbeEnabled()) {
      static std::unordered_map<u64, TextureFetchWords> seen;
      const u64 identity=(u64(bindings.textures[i])<<8)|i;
      const auto found=seen.find(identity);
      if (found==seen.end() || found->second!=fetch) {
        seen[identity]=fetch;
        LongProbeEvent("texture_fetch_changed", false, "slot=", i,
            "guest=", bindings.textures[i], "descriptor=", texture.descriptor_index,
            "VS=", BoundShaderHash(ShaderStage::kVertex),
            "PS=", BoundShaderHash(ShaderStage::kPixel), "fetch=", LongProbeHex(fetch));
      }
    }
    shared.samplers[i] = HostDevice::RegisterSampler(fetch);
    const int32_t lod_bias = int32_t((fetch[4] >> 12) & 1023);
    shared.fetch_lod_bias[i] = float(lod_bias >= 512 ? lod_bias - 1024 : lod_bias) / 32.0f;
    if (trace_constants && i >= 16) {
      struct VertexTextureTrace {
        u32 guest = 0;
        TextureFetchWords fetch{};
        bool valid = false;
      };
      // Trace changes rather than the first 32 draws of the same character.
      // These are the active fetch words, which can differ from its header.
      static std::array<VertexTextureTrace, kNativeTextureSlots> previous{};
      static u32 vertex_texture_logs = 0;
      auto& seen = previous[i];
      if (!seen.valid || seen.guest != bindings.textures[i] || seen.fetch != fetch) {
        seen = {bindings.textures[i], fetch, true};
        if (vertex_texture_logs++ < 64) REXLOG_INFO(
            "Native vertex texture slot={} guest={:08X} descriptor={} sampler={} VS={:016X} "
            "fetch={:08X},{:08X},{:08X},{:08X},{:08X},{:08X}",
            i, bindings.textures[i], texture.descriptor_index, shared.samplers[i],
            BoundShaderHash(ShaderStage::kVertex),
            fetch[0], fetch[1], fetch[2], fetch[3], fetch[4], fetch[5]);
      }
    }
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
  if (capture_constants && HostDevice::LongProbeSnapshotActive()) {
    const auto name=SaveLongProbeConstants({mapped,kSharedOffset+sizeof(shared)});
    if (!name.empty()) LongProbeEvent("draw_constants", false, "file=", name,
        "VS=", BoundShaderHash(ShaderStage::kVertex),
        "PS=", BoundShaderHash(ShaderStage::kPixel), "texture_mask=", texture_mask,
        "VS_offset=0 PS_offset=4096 shared_offset=8192 shared_bytes=752");
  }
  std::memcpy(upload.mapped, data.data(), data.size());
  commands->setGraphicsRootDescriptor(upload.buffer->at(upload.offset), 0);
  commands->setGraphicsRootDescriptor(upload.buffer->at(upload.offset + kConstantsSize), 1);
  commands->setGraphicsRootDescriptor(upload.buffer->at(upload.offset + kSharedOffset), 2);
  if (separate_upload) {
    separate_upload->unmap();
    HostDevice::RetireResource(std::move(separate_upload));
  }
  return true;
}

plume::RenderPipeline* GetPipeline(
    const PipelineKey& key, const VertexDeclarationView& declaration,
    plume::RenderPrimitiveTopology topology, plume::RenderFormat depth_format) {
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
  const auto depth_state = DecodeDepthState(
      key.depth_control, depth_format != plume::RenderFormat::UNKNOWN);
  constexpr plume::RenderComparisonFunction comparisons[] = {
      plume::RenderComparisonFunction::NEVER, plume::RenderComparisonFunction::LESS,
      plume::RenderComparisonFunction::EQUAL, plume::RenderComparisonFunction::LESS_EQUAL,
      plume::RenderComparisonFunction::GREATER, plume::RenderComparisonFunction::NOT_EQUAL,
      plume::RenderComparisonFunction::GREATER_EQUAL, plume::RenderComparisonFunction::ALWAYS};
  desc.depthFunction = comparisons[depth_state.comparison];
  desc.depthEnabled = depth_state.enabled;
  desc.depthWriteEnabled = depth_state.write;
  desc.depthClipEnabled = !key.clip_disable;
  const auto stencil = DecodeStencilState(key.depth_control, key.stencil_masks,
      depth_format == plume::RenderFormat::D32_FLOAT_S8_UINT,
      topology == plume::RenderPrimitiveTopology::TRIANGLE_LIST ||
      topology == plume::RenderPrimitiveTopology::TRIANGLE_STRIP);
  desc.stencilEnabled = stencil.enabled;
  if (stencil.enabled) {
    constexpr plume::RenderStencilOp ops[] = {
        plume::RenderStencilOp::KEEP, plume::RenderStencilOp::ZERO,
        plume::RenderStencilOp::REPLACE, plume::RenderStencilOp::INCREMENT_AND_CLAMP,
        plume::RenderStencilOp::DECREMENT_AND_CLAMP, plume::RenderStencilOp::INVERT,
        plume::RenderStencilOp::INCREMENT_AND_WRAP, plume::RenderStencilOp::DECREMENT_AND_WRAP};
    const auto face = [&](const StencilFaceState& value) {
      plume::RenderStencilFaceDesc result;
      result.compareFunction = comparisons[value.comparison];
      result.failOp = ops[value.fail];
      result.passOp = ops[value.pass];
      result.depthFailOp = ops[value.depth_fail];
      return result;
    };
    desc.stencilFrontFace = face(stencil.front);
    desc.stencilBackFace = face(stencil.back);
    desc.stencilReadMask = stencil.read_mask;
    desc.stencilWriteMask = stencil.write_mask;
    desc.frontFace = key.stencil_face ? plume::RenderFrontFace::CLOCKWISE
                                     : plume::RenderFrontFace::COUNTER_CLOCKWISE;
  }
  desc.primitiveTopology = topology;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.fillMode = plume::RenderFillMode::SOLID;
  desc.renderTargetCount = 0;
  for (u32 i = 0; i < kNativeRenderTargets; ++i) {
    const auto color_format = static_cast<plume::RenderFormat>(key.render_target_formats[i]);
    if (color_format == plume::RenderFormat::UNKNOWN) continue;
    desc.renderTargetCount = i + 1;
    desc.renderTargetFormat[i] = color_format;
    desc.renderTargetBlend[i] = plume::RenderBlendDesc::Copy();
    desc.renderTargetBlend[i].renderTargetWriteMask = (key.color_write_mask >> (i * 4)) & 15u;
    const auto blend = DecodeColorBlend(key.blend_controls[i]);
    if (!blend.supported) {
      static std::atomic<u32> unsupported_blends{0};
      if (unsupported_blends.fetch_add(1) < 16)
        REXLOG_WARN("Native GPU: unsupported blend state {:08X}", key.blend_controls[i]);
      return nullptr;
    }
    using B = plume::RenderBlend;
    constexpr B factors[] = {B::ZERO, B::ONE, B::UNKNOWN, B::UNKNOWN,
        B::SRC_COLOR, B::INV_SRC_COLOR, B::SRC_ALPHA, B::INV_SRC_ALPHA,
        B::DEST_COLOR, B::INV_DEST_COLOR, B::DEST_ALPHA, B::INV_DEST_ALPHA,
        B::BLEND_FACTOR, B::INV_BLEND_FACTOR, B::UNKNOWN, B::UNKNOWN, B::SRC_ALPHA_SAT};
    using O = plume::RenderBlendOperation;
    constexpr O operations[] = {O::ADD, O::SUBTRACT, O::MIN, O::MAX, O::REV_SUBTRACT};
    auto alpha_factor = [&](u32 factor) {
      // Xenos color factors reduce to their alpha component in the alpha equation.
      if (factor == 4 || factor == 5 || factor == 8 || factor == 9) factor += 2;
      return factor == 16 ? B::ONE : factors[factor];
    };
    auto& host_blend = desc.renderTargetBlend[i];
    host_blend.blendEnabled = blend.enabled;
    host_blend.srcBlend = factors[blend.source];
    host_blend.dstBlend = factors[blend.destination];
    host_blend.blendOp = operations[blend.operation];
    host_blend.srcBlendAlpha = alpha_factor(blend.alpha_source);
    host_blend.dstBlendAlpha = alpha_factor(blend.alpha_destination);
    host_blend.blendOpAlpha = operations[blend.alpha_operation];
  }
  desc.depthTargetFormat = depth_format;
  desc.inputElements = declaration.elements;
  desc.inputElementsCount = declaration.element_count;
  desc.inputSlots = slots.data();
  desc.inputSlotsCount = static_cast<u32>(slots.size());
  auto pipeline = HostDevice::Device()->createGraphicsPipeline(desc);
    if (!pipeline || !static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->d3d) {
      static std::atomic<u32> failed_pipeline_logs{0};
      if (failed_pipeline_logs.fetch_add(1) < 16)
        REXLOG_ERROR("Native GPU: D3D12 rejected pipeline VS=0x{:08X} PS=0x{:08X} decl=0x{:08X}",
                     key.vertex_shader, key.pixel_shader, key.declaration);
      return nullptr;
  }
  auto* result = pipeline.get();
  static u32 mrt_logs = 0;
  if (desc.renderTargetCount > 1 && mrt_logs++ < 16)
    REXLOG_INFO("Native GPU: MRT pipeline targets={} formats={},{},{},{} masks={:04X} blends={:08X},{:08X},{:08X},{:08X}",
        desc.renderTargetCount, key.render_target_formats[0], key.render_target_formats[1],
        key.render_target_formats[2], key.render_target_formats[3], key.color_write_mask,
        key.blend_controls[0], key.blend_controls[1], key.blend_controls[2], key.blend_controls[3]);
  g_pipelines.emplace(key, std::move(pipeline));
  return result;
}

u32 TraceMeshDraw(D3DDevice* device, u32 primitive, bool indexed,
                  u32 start, u32 count, i32 base_vertex) {
  static const bool enabled = std::getenv("LEGO_NATIVE_MESH_TRACE") != nullptr;
  if (!enabled || !device) return 0;
  const u64 vs = BoundShaderHash(ShaderStage::kVertex);
  const u64 ps = BoundShaderHash(ShaderStage::kPixel);
  const bool brick_trace =
      (vs == 0x5FE844BD78B0368Cull && ps == 0xDAB518E550952312ull) ||
      (vs == 0xC0C579A9C7D3ACD9ull && ps == 0xBC67385D86BABED8ull) ||
      (vs == 0xFFC80E0F8B28B228ull &&
          (ps == 0x1998CB74A7EDAFE1ull || ps == 0x336657153E47E0C3ull)) ||
      (vs == 0x362C416499E90A3Bull && ps == 0x0705E91ECC4BA3BBull);
  const bool primary_trace = vs == 0x35DB03916F21B74Full || vs == 0xA9FDB0A3AC4591FEull ||
      vs == 0x0406577EF00099BDull || vs == 0x5EFE4DB4D125C51Cull ||
      vs == 0x91007ACB3E640E3Dull || vs == 0xC3958E2D1B795ED9ull;
  if (vs != 0x35DB03916F21B74Full && vs != 0xA9FDB0A3AC4591FEull &&
      vs != 0x0406577EF00099BDull && vs != 0x5EFE4DB4D125C51Cull &&
      vs != 0x91007ACB3E640E3Dull && vs != 0xC3958E2D1B795ED9ull &&
      vs != 0xF8F4E64872206660ull && vs != 0x386847F375C2779Cull &&
      vs != 0xC9DB3F6A8FA579E2ull && vs != 0xE634DE2932FB7C5Full &&
      vs != 0x6E25E88B9AE03E54ull && vs != 0x5D70ED051F7FFB7Cull &&
      vs != 0x07ADC2A5768CE459ull && !brick_trace) return 0;
  const auto bindings = SnapshotDrawBindings();
  // Sample distinct geometry ranges, before any native draw rejection. Do not
  // hash changing bone matrices into the key and flood every animated frame.
  const auto* bytes = reinterpret_cast<const u8*>(device);
  const float alpha = *reinterpret_cast<const be_f32*>(bytes + 0x780 + 73 * 16 + 12);
  // The brick shaders are shared by other assets. Retain a bounded temporal
  // sample and distinct instance matrices rather than attributing every hit
  // to PrologueC or exhausting the quota on its first cloud of bricks.
  const u64 frame_bucket = brick_trace ? g_probe_frame.load() / 60 : 0;
  u64 instance_signature = 0;
  if (brick_trace) {
    // Diagnostic-only FNV over the exact guest c48..54 bytes.
    instance_signature = 14695981039346656037ull;
    for (u32 i = 0; i < 7 * 16; ++i)
      instance_signature = (instance_signature ^ bytes[0x780 + 48 * 16 + i]) * 1099511628211ull;
  }
  using Signature = std::array<u64, 15>;
  const auto& stream = bindings.vertex_streams[0];
  const Signature signature{vs, BoundShaderHash(ShaderStage::kPixel),
      stream.buffer, stream.offset, stream.stride, bindings.index_buffer,
      start, count, u32(base_vertex), primitive, indexed, device->vertex_declaration,
      alpha <= 0.0f ? 0u : alpha < 0.5f ? 1u : alpha < 0.99f ? 2u : 3u,
      frame_bucket, instance_signature};
  static std::set<Signature> seen;  // Recording lock serializes callers.
  // Shared body shader hashes can also occur on earlier scenery. Keep their
  // quota separate so they cannot exhaust the arm/logo trace before Vortech.
  static std::array<u32, 3> group_counts{};
  static u64 brick_bucket = UINT64_MAX;
  static u32 brick_bucket_count = 0;
  if (brick_trace && brick_bucket != frame_bucket) {
    brick_bucket = frame_bucket;
    brick_bucket_count = 0;
  }
  const u32 group = brick_trace ? 2 : primary_trace ? 0 : 1;
  if (group_counts[group] >= (brick_trace ? 512u : 256u) ||
      (brick_trace && brick_bucket_count >= 8) || !seen.insert(signature).second) return 0;
  ++group_counts[group];
  if (brick_trace) ++brick_bucket_count;
  const u32 id = u32(seen.size());
  const auto read = [&](u32 offset) { return u32(*reinterpret_cast<const be_u32*>(bytes + offset)); };
  REXLOG_INFO("Native mesh: id={} VS={:016X} PS={:016X} primitive={} indexed={} start={} count={} base={} decl={:08X} IB={:08X} RT={:08X} DS={:08X} depth={:08X} stencil={:08X} raster={:08X} mask={:08X} blend={:08X} tex5={:08X}",
      id, vs, signature[1], primitive, indexed, start, count, base_vertex,
      u32(device->vertex_declaration), bindings.index_buffer, bindings.render_targets[0],
      bindings.depth_stencil, read(10548), read(10496), read(10568), read(10460), read(10552), bindings.textures[5]);
  REXLOG_INFO("Native mesh: id={} alpha_control={:08X} alpha_ref={}", id,
      read(10556), float(*reinterpret_cast<const be_f32*>(bytes + 10620)));
  REXLOG_INFO("Native mesh: id={} frame={} brick_pair={}", id,
      g_probe_frame.load(), brick_trace);
  for (u32 i = 0; i < bindings.vertex_streams.size(); ++i) {
    const auto& s = bindings.vertex_streams[i];
    if (s.buffer) REXLOG_INFO("Native mesh: id={} stream={} VB={:08X} offset={} stride={}", id, i, s.buffer, s.offset, s.stride);
  }
  // Camera and the complete 52-bone palette are needed to evaluate skinned
  // positions offline. The first four rows alone miss influences 2..51.
  for (u32 c = 0; c < 256; ++c) {
    if (!(c <= 8 || (c >= 16 && c <= 24) || (c >= 43 && c <= 54) ||
          c == 73 || (!brick_trace && c >= 92 && c <= 247) ||
          (brick_trace && ((c >= 36 && c <= 42) || c == 72)))) continue;
    const auto* value = reinterpret_cast<const be_f32*>(bytes + 0x780 + c * 16);
    REXLOG_INFO("Native mesh: id={} c{}=({},{},{},{})", id, c,
        float(value[0]), float(value[1]), float(value[2]), float(value[3]));
  }
  for (u32 c : {4u,45u,53u,54u,55u,56u}) {
    const auto* value = reinterpret_cast<const be_f32*>(bytes + 0x1780 + c * 16);
    REXLOG_INFO("Native mesh: id={} PS c{}=({},{},{},{})", id, c,
        float(value[0]), float(value[1]), float(value[2]), float(value[3]));
  }
  return id;
}

bool DispatchDraw(D3DDevice* device, u32 primitive_type, bool indexed,
                  u32 start, u32 count, i32 base_vertex) {
  DrawTimer timing;
  const u32 mesh_trace = TraceMeshDraw(device, primitive_type, indexed, start, count, base_vertex);
  // Report early exits as well as successful submissions for each sample.
  struct MeshTraceResult {
    u32 id;
    bool submitted = false;
    ~MeshTraceResult() { if (id) REXLOG_INFO("Native mesh: id={} submitted={}", id, submitted); }
  } mesh_result{mesh_trace};
  if (!device || !count) {
    return false;
  }
  // Trace attempted bindings before shader/declaration/pipeline early exits.
  // A mask on a rejected draw is diagnostic evidence, not a submitted draw.
  if (PortraitProbeEnabled()) {
    const auto portrait_bindings = SnapshotDrawBindings();
    for (u32 i = 0; i < kNativeTextureSlots; ++i)
      TracePortraitDraw(portrait_bindings.textures[i], i,
          BoundShaderHash(ShaderStage::kVertex), BoundShaderHash(ShaderStage::kPixel));
  }
  if (g_logo_capture_api && indexed && count == 6 && start == 24 && base_vertex == 16 &&
      BoundShaderHash(ShaderStage::kVertex) == 0x91007ACB3E640E3Dull &&
      BoundShaderHash(ShaderStage::kPixel) == 0xEEE573BE160E037Dull &&
      float(*reinterpret_cast<const be_f32*>(reinterpret_cast<const u8*>(device) +
          0x780 + 73 * 16 + 12)) >= 0.95f) RequestLogoCapture();
  if (ShouldSkipBoundPixelShader()) return true;
  plume::RenderPrimitiveTopology topology;
  if (!MapTopology(primitive_type, topology)) {
    LongProbeEvent("unsupported_primitive", true, "type=", primitive_type);
    if (g_unsupported_draw_logs++ < 20) {
      REXLOG_WARN("Native GPU: skipped unsupported primitive type {}",
                  primitive_type);
    }
    return false;
  }
  DrawBindings bindings = SnapshotDrawBindings();
  if (g_logo_capture_api) {
    static std::set<std::array<u64, 6>> traced;
    const std::array<u64, 6> identity{BoundShaderHash(ShaderStage::kVertex),
        BoundShaderHash(ShaderStage::kPixel), bindings.render_targets[0],
        bindings.depth_stencil, u32(device->render_targets[0]), u32(device->depth_stencil)};
    if (traced.size() < 96 && traced.insert(identity).second)
      REXLOG_INFO("Native attachment capture: VS={:016X} PS={:016X} count={} native_rt={:08X} native_ds={:08X} shadow_rt={:08X} shadow_ds={:08X} depth_control={:08X}",
          identity[0], identity[1], count, identity[2], identity[3], identity[4], identity[5],
          u32(*reinterpret_cast<const be_u32*>(reinterpret_cast<const u8*>(device)+10548)));
  }
  // SetFVF and inline SDK paths may write the declaration without invoking
  // the public SetVertexDeclaration hook.
  bindings.vertex_declaration = device->vertex_declaration;
  const auto declaration = ResolveVertexDeclaration(bindings.vertex_declaration);
  std::array<TextureResourceView, kNativeRenderTargets> colors;
  for (u32 i = 0; i < colors.size(); ++i)
    colors[i] = ResolveTextureResource(bindings.render_targets[i]);
  const auto& color = colors[0];
  if (!PrepareSurfaceDepthAlias(bindings.depth_stencil)) return false;
  const auto depth = ResolveTextureResource(bindings.depth_stencil);
  auto* framebuffer = ResolveFramebuffer(bindings.render_targets, bindings.depth_stencil);
  if (!declaration.supported || !declaration.elements || !framebuffer ||
      (!color.texture && !depth.texture)) {
    LongProbeEvent("incomplete_draw_bindings", true, "decl=", bindings.vertex_declaration,
        "rt=", bindings.render_targets[0], "ds=", bindings.depth_stencil);
    if (g_unsupported_draw_logs++ < 20) {
      REXLOG_WARN("Native GPU: skipped draw with incomplete bindings "
                  "(decl=0x{:08X}, rt=0x{:08X}, ds=0x{:08X})",
                  bindings.vertex_declaration, bindings.render_targets[0],
                  bindings.depth_stencil);
    }
    return false;
  }
  timing.Next();
  auto* commands = HostDevice::BeginFrameCommands();
  if (!commands) {
    return false;
  }
  timing.Next();

  std::array<plume::RenderTextureBarrier, kNativeRenderTargets + 1> texture_barriers;
  u32 barrier_count = 0;
  for (const auto& target : colors) {
    if (target.texture) texture_barriers[barrier_count++] = plume::RenderTextureBarrier(
        target.texture, plume::RenderTextureLayout::COLOR_WRITE);
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
  key.vertex_shader_hash = BoundShaderHash(ShaderStage::kVertex);
  key.pixel_shader_hash = BoundShaderHash(ShaderStage::kPixel);
  key.declaration_hash = declaration.content_hash;
  key.declaration = bindings.vertex_declaration;
  for (u32 i = 0; i < colors.size(); ++i)
    key.render_target_formats[i] = static_cast<u32>(colors[i].format);
  key.depth_format = static_cast<u32>(depth.format);
  static const bool viewport_candidate = std::getenv("LEGO_NATIVE_VIEWPORT") != nullptr;
  if (viewport_candidate)
    key.clip_disable = (u32(*reinterpret_cast<const be_u32*>(
        reinterpret_cast<const u8*>(device) + 10564)) >> 16) & 1u;
  // TU23 stencil enable / two-sided / z-fail setters update the
  // RB_DEPTHCONTROL shadow at +10548 (0x2934), with hardware bit positions.
  // Reading the shadow also covers inlined setters, unlike hooking one caller.
  key.depth_control = *reinterpret_cast<const be_u32*>(
      reinterpret_cast<const u8*>(device) + 0x2934) & 0x76u;
  // Isolated candidate until cutscene/menu pixels have been compared. The
  // normal path retains its previous depth-only pipeline identity.
  static const bool enable_stencil = std::getenv("LEGO_NATIVE_STENCIL") != nullptr;
  if (enable_stencil) {
    key.depth_control = *reinterpret_cast<const be_u32*>(
        reinterpret_cast<const u8*>(device) + 10548);
    if ((key.depth_control & 1u) && depth.format == plume::RenderFormat::D32_FLOAT_S8_UINT) {
      // D3D12 has one mask pair. With native culling disabled, follow Xenia's
      // front-face mask choice. Per-face mask differences remain unsupported.
      key.stencil_masks = *reinterpret_cast<const be_u32*>(
          reinterpret_cast<const u8*>(device) + 10496) & 0x00FFFF00u;
      key.stencil_face = (*reinterpret_cast<const be_u32*>(
          reinterpret_cast<const u8*>(device) + 10568) >> 2) & 1u;
      static std::unordered_set<u64> logged_stencil;
      const u64 identity = (u64(key.depth_control) << 32) | key.stencil_masks;
      if (logged_stencil.size() < 32 && logged_stencil.insert(identity).second)
        REXLOG_INFO("Native GPU: stencil candidate control={:08X} masks={:06X} ref={} VS={:016X} PS={:016X}",
            key.depth_control, key.stencil_masks, reinterpret_cast<const u8*>(device)[10499],
            key.vertex_shader_hash, key.pixel_shader_hash);
    }
  }
  // TU23 setter 83FB8A68 and getter 83FB8AA0 prove the requested RT0
  // channel mask at +12292. Unlike the hardware shadow it is independent of
  // whether an RT was attached when the setter ran (SetRT is hooked here).
  for (u32 i = 0; i < kNativeRenderTargets; ++i)
    key.color_write_mask |= (*reinterpret_cast<const be_u32*>(
        reinterpret_cast<const u8*>(device) + 12292 + 4 * i) & 15u) << (4 * i);
  // TU23 83FB7E38 folds enable/separate-alpha into RB_BLENDCONTROL0:
  // disabling blending writes 0x00010001 at +10552. Read this folded state.
  // 83FB7E38 updates RT0 at +10552 and RT1..3 at +10584/+10588/+10592.
  constexpr u32 blend_offsets[] = {10552, 10584, 10588, 10592};
  for (u32 i = 0; i < kNativeRenderTargets; ++i)
    key.blend_controls[i] = *reinterpret_cast<const be_u32*>(
        reinterpret_cast<const u8*>(device) + blend_offsets[i]) & 0x1FFF1FFFu;
  static std::atomic<u32> depth_state_logs{0};
  if (depth_state_logs.fetch_add(1) < 32)
    REXLOG_INFO("Native GPU: draw depth-control=0x{:02X} target=0x{:08X}",
                key.depth_control, bindings.depth_stencil);
  key.topology = static_cast<u32>(topology);
  key.spec_constants = declaration.has_r11g11b10_normal ? 1u : 0u;
  const auto* state_bytes = reinterpret_cast<const u8*>(device);
  const auto alpha = DecodeNativeAlphaState(
      *reinterpret_cast<const be_u32*>(state_bytes + 10556),
      *reinterpret_cast<const be_f32*>(state_bytes + 10620));
  if (alpha.enabled) key.spec_constants |= 2u;
  for (u32 i = 0; i < kNativeVertexStreams; ++i) {
    key.strides[i] = bindings.vertex_streams[i].stride;
  }
  auto* pipeline = GetPipeline(key, declaration, topology, depth.format);
  if (!pipeline) {
    LongProbeEvent("draw_pipeline_unavailable", true, "VS=", key.vertex_shader_hash,
        "PS=", key.pixel_shader_hash, "decl_hash=", key.declaration_hash,
        "guest_vs=", key.vertex_shader, "guest_ps=", key.pixel_shader,
        "guest_decl=", bindings.vertex_declaration, "primitive=", primitive_type,
        "indexed=", indexed, "start=", start, "count=", count,
        "base_vertex=", base_vertex, "rt=", bindings.render_targets[0],
        "depth=", bindings.depth_stencil);
    return false;
  }
  timing.Next();

  commands->setGraphicsPipelineLayout(HostDevice::PipelineLayout());
  if (enable_stencil && (key.depth_control & 1u))
    static_cast<plume::D3D12CommandList*>(commands)->d3d->OMSetStencilRef(
        reinterpret_cast<const u8*>(device)[10499]);
  commands->setGraphicsDescriptorSet(HostDevice::TextureDescriptorSet(), 0);
  commands->setGraphicsDescriptorSet(HostDevice::TextureDescriptorSet(), 1);
  commands->setGraphicsDescriptorSet(HostDevice::TextureDescriptorSet(), 2);
  commands->setGraphicsDescriptorSet(HostDevice::SamplerDescriptorSet(), 3);
  auto constants = BindConstants(commands, device, bindings, declaration, count<=6);
  if (!constants) {
    return false;
  }
  // A raw depth texture view may need a conversion draw while binding textures.
  // Restore the guest framebuffer after that helper's temporary target.
  commands->barriers(plume::RenderBarrierStage::GRAPHICS, texture_barriers.data(), barrier_count);
  commands->setFramebuffer(framebuffer);
  timing.Next();
  commands->setPipeline(pipeline);
  // 83FB82F0 writes normalized R,G,B,A blend constants at +10464..10476.
  const auto* guest_blend_factor = reinterpret_cast<const be_f32*>(
      reinterpret_cast<const u8*>(device) + 10464);
  const float blend_factor[] = {guest_blend_factor[0], guest_blend_factor[1],
                               guest_blend_factor[2], guest_blend_factor[3]};
  static_cast<plume::D3D12CommandList*>(commands)->d3d->OMSetBlendFactor(blend_factor);

  const float viewport_width = static_cast<u32>(device->viewport.width);
  const float viewport_height = static_cast<u32>(device->viewport.height);
  const bool screen_space = viewport_candidate && u32(*reinterpret_cast<const be_u32*>(
      reinterpret_cast<const u8*>(device) + 10572)) == 0x400;
  commands->setViewports(plume::RenderViewport(
      screen_space ? 0 : static_cast<u32>(device->viewport.x),
      screen_space ? 0 : static_cast<u32>(device->viewport.y),
      screen_space ? float(color.width) : viewport_width > 0 ? viewport_width : float(color.width),
      screen_space ? float(color.height) : viewport_height > 0 ? viewport_height : float(color.height),
      screen_space ? 0.0f : static_cast<float>(device->viewport.min_z),
      screen_space ? 1.0f : static_cast<float>(device->viewport.max_z)));
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
  std::array<BufferResourceView, kNativeVertexStreams> vertex_buffers;
  static const bool window_uploads = [] {
    const char* value = std::getenv("LEGO_NATIVE_BUFFER_WINDOWS");
    const bool enabled = value ? std::strcmp(value, "0") != 0 : REXCVAR_GET(gpu_native_buffer_windows);
    REXLOG_INFO("Native buffer range cache: {} ({})", enabled,
        value ? "environment override" : "saved configuration");
    return enabled;
  }();
  std::array<VertexBufferWindow, kNativeVertexStreams> vertex_windows{};
  bool use_windows = false;
  u32 min_index = 0, index_offset = 0, index_bytes = 0;
  if (window_uploads && indexed && count &&
      primitive_type != static_cast<u32>(rex::graphics::xenos::PrimitiveType::kQuadList)) {
    const auto index = InspectBufferResource(bindings.index_buffer, BufferKind::kIndex);
    const u32 element_size = index.guest_format == 1 ? 2 : 4;
    if (index.mirror_address && u64(start) * element_size <= index.length &&
        u64(count) * element_size <= index.length - u64(start) * element_size) {
      index_offset = start * element_size;
      index_bytes = count * element_size;
      const auto* source = REX_KERNEL_MEMORY()->TranslateVirtual<const u8*>(index.mirror_address + index_offset);
      min_index = UINT32_MAX;
      u32 max_index = 0;
      for (u32 i = 0; i < count; ++i) {
        const u32 value = element_size == 2 ?
            u32(reinterpret_cast<const be_u16*>(source)[i]) :
            u32(reinterpret_cast<const be_u32*>(source)[i]);
        min_index = std::min(min_index, value);
        max_index = std::max(max_index, value);
      }
      // D3D12's signed base vertex must represent -min_index exactly.
      use_windows = min_index <= INT32_MAX;
      for (u32 i = 0; use_windows && i < kNativeVertexStreams; ++i) {
        const auto& binding = bindings.vertex_streams[i];
        if (!binding.buffer) continue;  // Synthetic/optional zero stream.
        const bool used = std::any_of(declaration.elements,
            declaration.elements + declaration.element_count,
            [i](const auto& e) { return e.slotIndex == i; });
        if (!used) continue;
        for (u32 e = 0; e < declaration.element_count; ++e) {
          const auto& element = declaration.elements[e];
          if (element.slotIndex != i) continue;
          const u32 width = plume::RenderFormatSize(element.format);
          if (!width || element.alignedByteOffset > binding.stride ||
              width > binding.stride - element.alignedByteOffset) use_windows = false;
        }
        if (!use_windows) break;
        const auto metadata = InspectBufferResource(binding.buffer, BufferKind::kVertex);
        vertex_windows[i] = DrawVertexWindow(min_index, max_index, base_vertex,
            binding.stride, binding.offset, metadata.length);
        if (!vertex_windows[i].length) use_windows = false;
      }
    }
  }
  for (u32 i = 0; i < kNativeVertexStreams; ++i) {
    const auto& binding = bindings.vertex_streams[i];
    // D3D keeps old stream bindings across declaration changes. A stream with
    // no input element cannot be fetched by this draw. Do not scan/upload a
    // previously bound world mesh for a fullscreen quad using another stream.
    const bool used = std::any_of(declaration.elements,
        declaration.elements + declaration.element_count,
        [i](const auto& element) { return element.slotIndex == i; });
    slots[i] = plume::RenderInputSlot(i, binding.stride ? binding.stride : 16);
    if (!used) {
      views[i] = plume::RenderVertexBufferView(null_buffer->at(0), 256);
      continue;
    }
    std::vector<u32> reversed_elements;
    for (u32 e = 0; e < std::min(declaration.element_count, 64u); ++e) {
      if ((declaration.reversed_byte_elements & (u64{1} << e)) &&
          declaration.elements[e].slotIndex == i)
        reversed_elements.push_back(declaration.elements[e].alignedByteOffset);
    }
    std::sort(reversed_elements.begin(), reversed_elements.end());
    reversed_elements.erase(std::unique(reversed_elements.begin(), reversed_elements.end()),
                             reversed_elements.end());
    const auto window = vertex_windows[i];
    const auto buffer = use_windows && window.length ?
        ResolveBufferResourceWindow(binding.buffer, BufferKind::kVertex,
            window.offset, window.length, {binding.stride, 0, reversed_elements}) :
        ResolveBufferResourceView(binding.buffer,
            BufferKind::kVertex, {binding.stride, binding.offset, reversed_elements});
    vertex_buffers[i] = buffer;
    const u32 offset = use_windows && window.length ? 0 : std::min(binding.offset, buffer.length);
    views[i] = buffer.buffer
                   ? plume::RenderVertexBufferView(buffer.buffer->at(offset),
                                                   buffer.length - offset)
                   : plume::RenderVertexBufferView(null_buffer->at(0), 256);
  }
  commands->setVertexBuffers(0, views.data(), views.size(), slots.data());
  if (mesh_trace) {
    for (u32 i = 0; i < vertex_buffers.size(); ++i) {
      const auto& buffer = vertex_buffers[i];
      if (buffer.buffer)
        REXLOG_INFO("Native mesh: id={} stream={} data={:08X} bytes={} content={:016X} conversion={:016X}",
            mesh_trace, i, buffer.mirror_address, buffer.length,
            buffer.content_hash, buffer.byte_order_hash);
    }
    i64 first_vertex = start;
    if (indexed) {
      const auto index = ResolveBufferResourceView(bindings.index_buffer, BufferKind::kIndex);
      REXLOG_INFO("Native mesh: id={} index_data={:08X} bytes={} format={} content={:016X}",
          mesh_trace, index.mirror_address, index.length, index.guest_format, index.content_hash);
      const u32 size = index.guest_format == 1 ? 2 : 4;
      if (index.buffer && (u64(start) + 1) * size <= index.length) {
        const auto* raw = static_cast<const u8*>(index.buffer->map());
        if (raw) {
          first_vertex = (size == 2 ? reinterpret_cast<const u16*>(raw)[start]
                                   : reinterpret_cast<const u32*>(raw)[start]) + i64(base_vertex);
          index.buffer->unmap();
        } else first_vertex = -1;
      } else first_vertex = -1;
    }
    REXLOG_INFO("Native mesh: id={} first_vertex={} packed_normal={}", mesh_trace,
        first_vertex, declaration.has_r11g11b10_normal);
    for (u32 e = 0; e < declaration.element_count; ++e) {
      const auto& element = declaration.elements[e];
      if (element.slotIndex >= vertex_buffers.size()) continue;
      const auto& binding = bindings.vertex_streams[element.slotIndex];
      const auto buffer = vertex_buffers[element.slotIndex];
      const u64 absolute_offset = first_vertex < 0 ? UINT64_MAX :
          u64(binding.offset) + u64(first_vertex) * binding.stride + element.alignedByteOffset;
      const u64 origin = use_windows ? vertex_windows[element.slotIndex].offset : 0;
      const u64 offset = absolute_offset >= origin ? absolute_offset - origin : UINT64_MAX;
      u32 words[2]{};
      bool sampled = false;
      if (buffer.buffer && offset <= buffer.length && buffer.length - offset >= sizeof(words)) {
        if (const auto* raw = static_cast<const u8*>(buffer.buffer->map())) {
          std::memcpy(words, raw + offset, sizeof(words));
          buffer.buffer->unmap();
          sampled = true;
        }
      }
      REXLOG_INFO("Native mesh: id={} attr={}{} slot={} offset={} stride={} format={} reversed={} length={} sampled={} words={:08X},{:08X}",
          mesh_trace, element.semanticName, element.semanticIndex, element.slotIndex,
          element.alignedByteOffset, binding.stride, u32(element.format),
          (declaration.reversed_byte_elements >> e) & 1, buffer.length, sampled, words[0], words[1]);
    }
  }
  timing.Next();

  // Opt-in, bounded startup trace: separate texture color from vertex tint,
  // float constants, and declaration decoding without altering the draw.
  static const bool trace_color = [] {
    char* value = nullptr;
    size_t length = 0;
    _dupenv_s(&value, &length, "LEGO_NATIVE_COLOR_TRACE");
    const bool enabled = value && *value;
    std::free(value);
    return enabled;
  }();
  static u32 color_traces = 0;  // Serialized by LockRecording.
  if (trace_color && color_traces++ < 8) {
    const auto* constant_bytes = reinterpret_cast<const u8*>(device);
    const auto* tint = reinterpret_cast<const be_f32*>(constant_bytes + 0x780 + 73 * 16);
    const auto* ps45 = reinterpret_cast<const be_f32*>(constant_bytes + 0x1780 + 45 * 16);
    const auto* ps32 = reinterpret_cast<const be_f32*>(constant_bytes + 0x1780 + 32 * 16);
    REXLOG_INFO("Native color: draw={} VS={:08X} PS={:08X} start={} base={} indexed={} "
                "VS73=({},{},{},{}) PS45=({},{},{},{}) PS32.x={}",
        color_traces, key.vertex_shader, key.pixel_shader, start, base_vertex, indexed,
        float(tint[0]), float(tint[1]), float(tint[2]), float(tint[3]),
        float(ps45[0]), float(ps45[1]), float(ps45[2]), float(ps45[3]), float(ps32[0]));
    for (u32 i = 0; i < declaration.element_count; ++i) {
      const auto& element = declaration.elements[i];
      if (element.slotIndex >= bindings.vertex_streams.size()) continue;
      const auto& binding = bindings.vertex_streams[element.slotIndex];
      const auto buffer = vertex_buffers[element.slotIndex];
      const u64 offset = (use_windows ? 0 : u64(binding.offset)) + element.alignedByteOffset;
      if (!buffer.buffer || offset + 4 > buffer.length) continue;
      const auto* mapped_input = static_cast<const u8*>(buffer.buffer->map());
      if (!mapped_input) continue;
      u32 word = 0;
      std::memcpy(&word, mapped_input + offset, sizeof(word));
      buffer.buffer->unmap();
      REXLOG_INFO("Native color: {}{} slot={} offset={} stride={} format={} host-word={:08X}",
          element.semanticName, element.semanticIndex, element.slotIndex,
          element.alignedByteOffset, binding.stride, u32(element.format), word);
    }
  }

  if (primitive_type == static_cast<u32>(rex::graphics::xenos::PrimitiveType::kQuadList)) {
    if (count % 4 != 0) return false;
    const auto source = indexed
        ? ResolveBufferResourceView(bindings.index_buffer, BufferKind::kIndex)
        : BufferResourceView{};
    const u32 element_size = source.guest_format == 1 ? 2 : 4;
    if (indexed && (!source.buffer || u64(start + u64(count)) * element_size > source.length))
      return false;
    auto expanded = std::shared_ptr<plume::RenderBuffer>(HostDevice::Device()->createBuffer(
        plume::RenderBufferDesc::IndexBuffer(u64(count / 4) * 6 * sizeof(u32),
                                            plume::RenderHeapType::UPLOAD)).release());
    if (!expanded) return false;
    auto* output = static_cast<u32*>(expanded->map());
    const auto* input = indexed ? static_cast<const u8*>(source.buffer->map()) : nullptr;
    if (!output || (indexed && !input)) {
      if (output) expanded->unmap();
      return false;
    }
    constexpr u32 order[] = {0, 1, 2, 0, 2, 3};
    for (u32 quad = 0; quad < count / 4; ++quad) {
      for (u32 corner = 0; corner < 6; ++corner) {
        const u32 index = start + quad * 4 + order[corner];
        output[quad * 6 + corner] = !indexed ? index
            : element_size == 2 ? reinterpret_cast<const u16*>(input)[index]
                                : reinterpret_cast<const u32*>(input)[index];
      }
    }
    if (indexed) source.buffer->unmap();
    expanded->unmap();
    const plume::RenderIndexBufferView index_view(expanded->at(0),
        u64(count / 4) * 6 * sizeof(u32), plume::RenderFormat::R32_UINT);
    commands->setIndexBuffer(&index_view);
    {
      QueryDrawScope query_scope(commands);
      commands->drawIndexedInstanced(count / 4 * 6, 1, 0, indexed ? base_vertex : 0, 0);
    }
    HostDevice::RetireResource(std::move(expanded));
  } else if (indexed) {
    const auto index = use_windows ?
        ResolveBufferResourceWindow(bindings.index_buffer, BufferKind::kIndex, index_offset, index_bytes) :
        ResolveBufferResourceView(bindings.index_buffer, BufferKind::kIndex);
    if (!index.buffer) {
      return false;
    }
    const plume::RenderFormat index_format =
        index.guest_format == 1 ? plume::RenderFormat::R16_UINT
                                : plume::RenderFormat::R32_UINT;
    const plume::RenderIndexBufferView index_view(index.buffer->at(0),
                                                  index.length, index_format);
    commands->setIndexBuffer(&index_view);
    QueryDrawScope query_scope(commands);
    commands->drawIndexedInstanced(count, 1, use_windows ? 0 : start,
        use_windows ? -i32(min_index) : base_vertex, 0);
  } else {
    QueryDrawScope query_scope(commands);
    commands->drawInstanced(count, 1, start, 0);
  }
  timing.Next();
  static const bool trace_stages = [] {
    char* value = nullptr;
    size_t length = 0;
    _dupenv_s(&value, &length, "LEGO_GPU_SNAPSHOT_DIR");
    const bool enabled = value && *value;
    std::free(value);
    return enabled;
  }();
  if (trace_stages) {
    if (LongProbeEnabled() && (HostDevice::LongProbeSnapshotActive() ||
        LongProbeOnce(key.vertex_shader_hash ^ key.pixel_shader_hash ^ declaration.content_hash)))
      LongProbeEvent("draw_stage", false, "VS=", key.vertex_shader_hash,
          "PS=", key.pixel_shader_hash, "count=", count, "indexed=", indexed,
          "guest_vs=", key.vertex_shader, "guest_ps=", key.pixel_shader,
          "guest_decl=", bindings.vertex_declaration, "primitive=", primitive_type,
          "start=", start, "base_vertex=", base_vertex,
          "decl_hash=", declaration.content_hash, "rt=", bindings.render_targets[0],
          "width=", color.width, "height=", color.height, "format=", u32(color.format),
          "depth=", bindings.depth_stencil, "write_mask=", key.color_write_mask,
          "textures=", LongProbeHex(bindings.textures));
    static std::unordered_set<PipelineKey, PipelineKeyHash> traced;
    if (traced.size() < 100 && traced.insert(key).second) {
      REXLOG_INFO("Native stage: VS={:08X} PS={:08X} rt={:08X} fmt={} ds={:08X} z={:02X} "
          "view={},{},{},{} scissor={},{},{},{} tex={:08X},{:08X},{:08X},{:08X} "
          "tex4-7={:08X},{:08X},{:08X},{:08X} mask={:X} blend={:08X}",
          key.vertex_shader, key.pixel_shader, bindings.render_targets[0], key.render_target_formats[0],
          bindings.depth_stencil, key.depth_control, u32(device->viewport.x), u32(device->viewport.y),
          u32(device->viewport.width), u32(device->viewport.height), i32(device->scissor.left),
          i32(device->scissor.top), scissor_right, scissor_bottom,
          bindings.textures[0], bindings.textures[1], bindings.textures[2], bindings.textures[3],
          bindings.textures[4], bindings.textures[5], bindings.textures[6], bindings.textures[7],
          key.color_write_mask, u32(*reinterpret_cast<const be_u32*>(
              reinterpret_cast<const u8*>(device) + 10552)));
    }
    if (color.texture && (!LongProbeEnabled() || count <= 6))
      HostDevice::SnapshotTexture(color.texture,
          "draw-vs-" + std::to_string(key.vertex_shader) + "-ps-" +
          std::to_string(key.pixel_shader), true);
  }
  static std::atomic<u32> issued_draw_logs{0};
  if (issued_draw_logs.fetch_add(1) < 8)
    REXLOG_INFO("Native GPU: issued draw primitive={} count={} indexed={} decl=0x{:08X}",
                 primitive_type, count, indexed, bindings.vertex_declaration);
  mesh_result.submitted = true;
  for (u32 i = 0; i < kNativeRenderTargets; ++i)
    if ((key.color_write_mask >> (i * 4)) & 15u) MarkSurfaceWritten(bindings.render_targets[i]);
  if (DecodeDepthState(key.depth_control, depth.texture != nullptr).write)
    MarkSurfaceWritten(bindings.depth_stencil);
  return true;
}

u32 ClearHook(D3DDevice* /*device*/, u32 rectangle_count,
              u32 rectangles_address, u32 flags, u32 color, f64 depth,
              u32 /*depth_gpr_slot*/, u32 stencil, u32 /*edram_clear*/) {
  auto recording = HostDevice::LockRecording();
  const DrawBindings bindings = SnapshotDrawBindings();
  auto* commands = HostDevice::BeginFrameCommands();
  if (!commands) return 0x80004005u;
  static std::atomic<u32> clear_sequence{0};
  const u32 clear_id = clear_sequence.fetch_add(1);
  static u32 tiled_clear_logs = 0;
  const bool trace_clear = clear_id < 128 || (g_seen_tiled_pass && tiled_clear_logs++ < 24);
  if (trace_clear)
    REXLOG_INFO("Native GPU: clear #{} flags={:X} rt={:08X},{:08X},{:08X},{:08X} ds={:08X} color={:08X} z={}",
        clear_id, flags, bindings.render_targets[0], bindings.render_targets[1],
        bindings.render_targets[2], bindings.render_targets[3], bindings.depth_stencil, color, depth);

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
  const float scale = 1.0f / 255.0f;
  // X360 TARGET0..3 are independent bits 1/2/4/8. Bind each selected
  // attachment alone for clear; its shader output slot is unchanged for draws.
  for (u32 i = 0; i < kNativeRenderTargets; ++i) {
    if (!(flags & (1u << i))) continue;
    const auto target = ResolveTextureResource(bindings.render_targets[i]);
    auto* framebuffer = ResolveFramebuffer(bindings.render_targets[i], 0);
    if (!target.texture || !framebuffer) return 0x80004005u;
    commands->barriers(plume::RenderBarrierStage::GRAPHICS,
        plume::RenderTextureBarrier(target.texture, plume::RenderTextureLayout::COLOR_WRITE));
    commands->setFramebuffer(framebuffer);
    const bool fixed16 = target.format == plume::RenderFormat::R16G16B16A16_SNORM ||
                         target.format == plume::RenderFormat::R16G16_SNORM;
    const float clear_scale = fixed16 ? 1.0f / 32.0f : 1.0f;
    const plume::RenderColor encoded_color(float((color >> 16) & 0xFF) * scale * clear_scale,
        float((color >> 8) & 0xFF) * scale * clear_scale,
        float(color & 0xFF) * scale * clear_scale,
        float((color >> 24) & 0xFF) * scale * clear_scale);
    commands->clearColor(0, encoded_color, clear_rectangles, clear_rectangle_count);
    MarkSurfaceWritten(bindings.render_targets[i]);
  }
  if (flags & 0x30) {
    if (!PrepareSurfaceDepthAlias(bindings.depth_stencil)) return 0x80004005u;
    const auto depth_target = ResolveTextureResource(bindings.depth_stencil);
    auto* framebuffer = ResolveFramebuffer(0, bindings.depth_stencil);
    if (!depth_target.texture || !framebuffer) return 0x80004005u;
    commands->barriers(plume::RenderBarrierStage::GRAPHICS,
        plume::RenderTextureBarrier(depth_target.texture, plume::RenderTextureLayout::DEPTH_WRITE));
    commands->setFramebuffer(framebuffer);
    commands->clearDepthStencil((flags & 0x10) != 0, (flags & 0x20) != 0,
        static_cast<float>(depth), stencil, clear_rectangles, clear_rectangle_count);
    if (flags & 0x10) MarkSurfaceWritten(bindings.depth_stencil);
  }
  if (trace_clear) REXLOG_INFO("Native GPU: clear #{} complete", clear_id);
  return 0;
}

u32 DrawVerticesHook(D3DDevice* device, u32 primitive_type, u32 start_vertex,
                     u32 vertex_count) {
  auto recording = HostDevice::LockRecording();
  if (ConsumeCpuPoolCopyDraw(primitive_type, start_vertex, vertex_count)) return 0;
  if (!DispatchDraw(device, primitive_type, false, start_vertex, vertex_count, 0))
    FailActiveQueries();
  return 0;
}

u32 DrawIndexedVerticesHook(D3DDevice* device, u32 primitive_type,
                            u32 base_vertex, u32 start_index, u32 index_count) {
  auto recording = HostDevice::LockRecording();
  if (!DispatchDraw(device, primitive_type, true, start_index, index_count,
                    static_cast<i32>(base_vertex))) FailActiveQueries();
  return 0;
}

bool ReadResolveWords(u32 address, i32* output, u32 count) {
  if (!address || u64(address) + count * 4 > 0x100000000ull) return false;
  auto* memory = REX_KERNEL_MEMORY();
  for (u64 at = address; at < u64(address) + count * 4; at = (at & ~u64(4095)) + 4096) {
    auto* heap = memory->LookupHeap(u32(at));
    rex::memory::HeapAllocationInfo info{};
    if (!heap || !heap->QueryRegionInfo(u32(at), &info) ||
        !(info.state & rex::memory::kMemoryAllocationCommit) ||
        !(info.protect & rex::memory::kMemoryProtectRead)) return false;
  }
  const auto* words = memory->TranslateVirtual<const be_u32*>(address);
  for (u32 i = 0; i < count; ++i) output[i] = i32(u32(words[i]));
  return true;
}

u32 ResolveHook(u32 flags, u32 source_rect,
                u32 destination_texture, u32 destination_point,
                u32 destination_level, u32 destination_slice,
                u32 clear_color, f64 clear_z, u32 lr, u32 sp) {
  auto recording = HostDevice::LockRecording();
  i32 rect_words[4]{}, point_words[2]{};
  if ((source_rect && !ReadResolveWords(source_rect, rect_words, 4)) ||
      (destination_point && !ReadResolveWords(destination_point, point_words, 2)))
    return 0x80070057u;
  const ResolveRect rect{rect_words[0], rect_words[1], rect_words[2], rect_words[3]};
  const ResolvePoint point{point_words[0], point_words[1]};
  // Log distinct transfers, not the same loading-screen depth copy hundreds
  // of times before the first shadow atlas appears.
  using Transfer = std::array<u32, 10>;
  static std::vector<Transfer> traced_transfers;
  const Transfer transfer{flags, destination_texture, destination_level, destination_slice,
      u32(rect.left), u32(rect.top), u32(rect.right), u32(rect.bottom), u32(point.x), u32(point.y)};
  if (traced_transfers.size() < 128 &&
      std::find(traced_transfers.begin(), traced_transfers.end(), transfer) == traced_transfers.end()) {
    traced_transfers.push_back(transfer);
    i32 backchain = 0, caller_lr = 0;
    if (ReadResolveWords(sp, &backchain, 1) && u32(backchain) >= 8)
      ReadResolveWords(u32(backchain) - 8, &caller_lr, 1);
    REXLOG_INFO("Native GPU: Resolve ABI lr={:08X} caller={:08X} flags={:08X} "
        "rect@{:08X}=({},{},{},{}) point@{:08X}=({},{}) clear@{:08X} z={}",
        lr, u32(caller_lr), flags, source_rect, rect.left, rect.top, rect.right, rect.bottom,
        destination_point, point.x, point.y, clear_color, clear_z);
  }
  // TU23 sub_83FBDF80 selects +12832 for index 4, or +12816+4*index for 0..3.
  // Depth conversion remains explicit
  // unsupported work; never feed it unrelated color data.
  const u32 source = (flags & 4u) ? BoundDepthStencil() : BoundRenderTarget(flags & 3u);
  const bool resolved = ResolveTextureFromSurface(destination_texture, source,
                                 destination_level, destination_slice, flags,
                                 source_rect ? &rect : nullptr,
                                 destination_point ? &point : nullptr);
  if (!resolved &&
      g_unsupported_draw_logs++ < 20) {
    REXLOG_WARN("Native GPU: skipped resolve src=0x{:08X} dst=0x{:08X} "
                "flags=0x{:08X} level={} slice={}", source,
                destination_texture, flags, destination_level, destination_slice);
  }
  // RESOLVE_CLEARRENDERTARGET takes float RGBA, unlike Clear's packed ARGB.
  // Clear only after the copy, so accumulated intermediate passes start clean.
  if (resolved && (flags & 0x100u) && !(flags & 4u) && clear_color) {
    i32 clear_words[4];
    if (!ReadResolveWords(clear_color, clear_words, 4)) return 0x80070057u;
    float rgba[4];
    std::memcpy(rgba, clear_words, sizeof(rgba));
    const auto target = ResolveTextureResource(source);
    auto* framebuffer = ResolveFramebuffer(source, 0);
    auto* commands = HostDevice::BeginFrameCommands();
    if (!target.texture || !framebuffer || !commands) return 0x80004005u;
    const bool fixed16 = target.format == plume::RenderFormat::R16G16B16A16_SNORM ||
                         target.format == plume::RenderFormat::R16G16_SNORM;
    const float scale = fixed16 ? 1.0f / 32.0f : 1.0f;
    commands->barriers(plume::RenderBarrierStage::GRAPHICS,
        plume::RenderTextureBarrier(target.texture, plume::RenderTextureLayout::COLOR_WRITE));
    commands->setFramebuffer(framebuffer);
    const plume::RenderRect area(rect.left, rect.top, rect.right, rect.bottom);
    commands->clearColor(0, plume::RenderColor(rgba[0]*scale, rgba[1]*scale,
        rgba[2]*scale, rgba[3]*scale), source_rect ? &area : nullptr, source_rect ? 1 : 0);
    MarkSurfaceWritten(source);
    static std::atomic<u32> clear_logs{0};
    if (clear_logs.fetch_add(1) < 8)
      REXLOG_INFO("Native GPU: resolve cleared source={:08X} rgba={},{},{},{}",
          source, rgba[0], rgba[1], rgba[2], rgba[3]);
  }
  return 0;
}

u32 SwapHook(D3DDevice* /*device*/, u32 front_buffer, u32 /*parameters*/) {
  HostDevice::PollCompletionCallbacks();
  auto recording = HostDevice::LockRecording();
  const auto texture = ResolveTextureResource(front_buffer);
  if (!HostDevice::PresentTexture(texture.texture, texture.descriptor_index) &&
      g_unsupported_draw_logs++ < 20) {
    LogUnknownTexture(front_buffer, "present");
    REXLOG_WARN("Native GPU: skipped present for front buffer 0x{:08X}",
                front_buffer);
  }
  recording.unlock();
  HostDevice::PollCompletionCallbacks();
  return 0;
}

// Sep'13 tile.obj: Xbox replays these draws through a worker command queue
// to fit EDRAM. Host render targets hold the whole image, so draw once between
// the same clear/resolve boundaries without entering that Xbox worker queue.
u32 BeginTilingHook(D3DDevice* device, u32 flags, u32 count,
                    u32 rectangles, mapped_f32 clear_color, f64 clear_z,
                    u32 /*z_gpr_slot*/, u32 clear_stencil) {
  u32 color = 0;
  if (const be_f32* rgba = clear_color) {
    const auto pack = [](float value) {
      return static_cast<u32>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    color = (pack(rgba[3]) << 24) | (pack(rgba[0]) << 16) |
            (pack(rgba[1]) << 8) | pack(rgba[2]);
  }
  auto recording = HostDevice::LockRecording();
  auto* memory = REX_KERNEL_MEMORY();
  const u64 end = u64(rectangles) + u64(count) * sizeof(D3DRect);
  bool readable = rectangles && count && count <= 16 && end <= 0x100000000ull;
  for (u64 at = rectangles; readable && at < end; at = (at & ~u64(4095)) + 4096) {
    auto* heap = memory->LookupHeap(static_cast<u32>(at));
    rex::memory::HeapAllocationInfo info{};
    readable = heap && heap->QueryRegionInfo(static_cast<u32>(at), &info) &&
        (info.state & rex::memory::kMemoryAllocationCommit) &&
        (info.protect & rex::memory::kMemoryProtectRead);
  }
  std::array<NativeTileRect, 16> tiles;
  if (!readable) return 0x80004001u;
  const auto* guest_tiles = memory->TranslateVirtual<const D3DRect*>(rectangles);
  for (u32 i = 0; i < count; ++i)
    tiles[i] = {i32(guest_tiles[i].left), i32(guest_tiles[i].top),
                i32(guest_tiles[i].right), i32(guest_tiles[i].bottom)};
  const auto extent = LogicalTileExtent(std::span(tiles.data(), count));
  if (!extent.width) return 0x80004001u;
  const auto bindings = SnapshotDrawBindings();
  // The observed scene clears AFTER BeginTiling(flags=1), before geometry.
  // Allocate synchronously here so the clear covers the new complete target.
  // Do not enable Xbox tiling count/flags or invoke its worker queue.
  for (u32 surface : bindings.render_targets)
    if (!PromoteTiledSurface(surface, extent.width, extent.height)) return 0x80004001u;
  if (!PromoteTiledSurface(bindings.depth_stencil, extent.width, extent.height))
    return 0x80004001u;
  g_seen_tiled_pass = true;
  HostDevice::BeginTiledPassSnapshot();
  static std::atomic<u32> logs{0};
  if (logs.fetch_add(1) < 8)
    REXLOG_INFO("Native GPU: BeginTiling flags={:X} tiles={} host={}x{} viewport={}x{} guest_count={}",
        flags, count, extent.width, extent.height, u32(device->viewport.width),
        u32(device->viewport.height), u32(*reinterpret_cast<const be_u32*>(
            reinterpret_cast<const u8*>(device) + 13124)));
  // The TU23 implementation branches past its initial clear when bit 0 is set.
  if (flags & 1) return 0;
  return ClearHook(device, 0, 0, 0x31, color, clear_z, 0, clear_stencil, 0);
}

u32 EndTilingHook(D3DDevice* /*device*/, u32 flags, u32 /*rectangles*/,
                  u32 destination, u32 /*clear_color*/, f64 /*clear_z*/,
                  u32 /*z_gpr_slot*/, u32 /*clear_stencil*/, u32 /*parameters*/) {
  if (!destination) return 0;
  const u32 source = (flags & 4) ? BoundDepthStencil() : BoundRenderTarget(flags & 3);
  const bool resolved = ResolveTextureFromSurface(destination, source, 0, 0, flags);
  static std::atomic<u32> logs{0};
  if (logs.fetch_add(1) < 8)
    REXLOG_INFO("Native GPU: EndTiling source=0x{:08X} destination=0x{:08X} resolved={}",
                 source, destination, resolved);
  return resolved ? 0 : 0x80004001u;
}

}  // namespace

DrawTiming ConsumeDrawTiming() {
  auto recording = HostDevice::LockRecording();
  return std::exchange(g_draw_timing, {});
}

void ResetDrawResources() {
  std::lock_guard lock(g_pipeline_mutex);
  g_pipelines.clear();
  g_seen_tiled_pass = false;
}

}  // namespace legodimensions::gpu_native

REX_HOOK(sub_83FC6640, legodimensions::gpu_native::DrawVerticesHook);
REX_HOOK(sub_83FC6A58, legodimensions::gpu_native::DrawIndexedVerticesHook);
REX_HOOK_RAW(sub_83FBF0D8) {
  ctx.r3.u64 = legodimensions::gpu_native::ResolveHook(ctx.r4.u32, ctx.r5.u32,
      ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32,
      ctx.f1.f64, ctx.lr, ctx.r1.u32);
}
REX_HOOK(sub_83FB33E0, legodimensions::gpu_native::SwapHook);
REX_HOOK(sub_83FBC9D8, legodimensions::gpu_native::ClearHook);
REX_HOOK(sub_83FBCD28, legodimensions::gpu_native::BeginTilingHook);
REX_HOOK(sub_83FBD098, legodimensions::gpu_native::EndTilingHook);
