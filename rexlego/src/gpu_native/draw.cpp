#include "gpu_native/native_recording.h"
#include "gpu_native/draw_work_pool.h"
#include "gpu_native/pipeline_creation_gate.h"
#include "gpu_native/preparation_dependencies.h"
#include "gpu_native/guest_metadata.h"
#include "gpu_native/device_snapshot_pool.h"
#include "gpu_native/renderer_optimizations.h"
#include "gpu_native/draw_batch.h"
#include "gpu_native/memory_watch.h"
#include <algorithm>
#if defined(LEGODIMENSIONS_TT_LOCK_TRACE)
#include "../tt_lock_trace.h"
#endif
#include <bit>
#include <array>
#include <immintrin.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <cstdio>
#include <set>
#include <string>
#include <span>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <xxhash.h>

#include <plume_render_interface.h>
#include <plume_d3d12.h>
#include <rex/graphics/xenos.h>
#include "gpu_native/gpu_pass_profile.h"
#include "gpu_native/renderer_route.h"
#include <rex/logging.h>
#include <rex/cvar.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include "gpu_native/buffers.h"
#include "gpu_native/buffer_window.h"
#include "gpu_native/vertex_fetch_range.h"
#include "gpu_native/shader_input_usage.h"
#include "gpu_native/vertex_binding_cache.h"
#include "gpu_native/draw_constant_cache.h"
#include "gpu_native/draw_preparation.h"
#include "gpu_native/vertex_layout_plan.h"
#include "gpu_native/vertex_fetch_plan.h"
#include "gpu_native/float_blend_indices.h"
#include "gpu_native/instanced_constants.h"
#include "gpu_native/integer_inputs.h"
#include "gpu_native/instanced_fetch.h"
#include "gpu_native/multi_rate_fetch.h"
#include "gpu_native/multi_rate_draw_order.h"
#include "gpu_native/multi_rate_guest_gather.h"
#include "gpu_native/native_worker.h"
#include "gpu_native/shader_archive.h"
#include "gpu_native/index_draw.h"
#include "gpu_native/quad_index_cache.h"
#include "gpu_native/index_bounds.h"
#include "gpu_native/alpha_test.h"
#include "gpu_native/blend_state.h"
#include "gpu_native/cull_state.h"
#include "gpu_native/queries.h"
#include "gpu_native/d3d.h"
#include "gpu_native/draw.h"
#include "gpu_native/draw_diagnostic.h"
#include "gpu_native/texture_fetch_history.h"
#include "gpu_native/temporal_draw_history.h"
#include "gpu_native/temporal_texture_history.h"
#include "gpu_native/temporal_camera_authority.h"
#include "gpu_native/temporal_scene_depth.h"
#include "gpu_native/temporal_scene_pass.h"
#include "gpu_native/temporal_present.h"
#include "gpu_native/temporal_composition.h"
#include "gpu_native/temporal_shaders.h"
#include "gpu_native/fsr3_settings.h"
#include "gpu_native/settings_runtime.h"
#include "gpu_native/depth_state.h"
#include "gpu_native/polygon_offset.h"
#include "gpu_native/stencil_state.h"
#include "gpu_native/scissor_state.h"
#include "gpu_native/device.h"
#include "gpu_native/shaders.h"
#include "gpu_native/state.h"
#include "gpu_native/textures.h"
#include "gpu_native/tile_extent.h"
#include "gpu_native/tonemap_dof.h"
#include "gpu_native/visual_effects.h"
#include "gpu_native/portrait_probe.h"
#include "gpu_native/long_probe.h"
#include "gpu_native/renderdoc_probe.h"
#include "gpu_native/vertex_declarations.h"
#include "gpu_native/state_call_diagnostic.h"

REXCVAR_DEFINE_BOOL(gpu_native_buffer_windows, false, "GPU",
    "Upload and retain only the vertex buffer ranges fetched by native draws")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(gpu_native_draw_trace_trigger, "", "GPU",
    "Opt-in path to a one-frame CPU draw capture trigger (restart required)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gpu_native_draw_trace_geometry, false, "GPU",
    "Include bounded raw geometry and complete constants in draw captures")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace legodimensions::gpu_native {
namespace {

TemporalDrawHistory g_temporal_draw_history;
TemporalCameraAuthority g_temporal_camera_authority;
QuadIndexCache<plume::RenderBuffer> g_quad_indices_serial;
QuadIndexCache<plume::RenderBuffer>& Select_g_quad_indices() {
  if (CurrentDrawPreparation()) { static thread_local QuadIndexCache<plume::RenderBuffer> state; return state; }
  return g_quad_indices_serial;
}
#define g_quad_indices Select_g_quad_indices()
VertexBindingCache g_vertex_bindings_serial;
VertexBindingCache& Select_g_vertex_bindings() {
  if (CurrentDrawPreparation()) { static thread_local VertexBindingCache state; return state; }
  return g_vertex_bindings_serial;
}
#define g_vertex_bindings Select_g_vertex_bindings()
VertexInputUsageCache g_vertex_input_usage_serial;
VertexInputUsageCache& Select_g_vertex_input_usage() {
  if (CurrentDrawPreparation()) { static thread_local VertexInputUsageCache state; return state; }
  return g_vertex_input_usage_serial;
}
#define g_vertex_input_usage Select_g_vertex_input_usage()
bool g_vertex_binding_backend_ready = false, g_vertex_binding_cache_enabled = false;
TemporalSceneDepth g_temporal_scene_depth;
u32 g_temporal_scene_depth_surface = 0;
bool g_temporal_scene_depth_dirty = false;
void SnapshotTemporalSceneDepth(u32 source) {
  if (!source || source != g_temporal_scene_depth_surface ||
      !g_temporal_scene_depth_dirty) return;
  const auto extent = NativeTextureExtent(source);
  if (extent.logical_width != 1280 || extent.logical_height != 720) return;
  // A later color resolve after Clear must not replace scene depth with the
  // cleared surface. Only newly submitted scene geometry makes it dirty again.
  g_temporal_scene_depth_dirty = false;
  auto snapshot = CaptureTemporalSceneDepth(source, extent.width, extent.height,
      g_probe_frame.load(), g_temporal_scene_depth);
  if (!snapshot.texture) {
    g_temporal_scene_depth.frame = ~uint64_t{0};
    return;
  }
  if (g_temporal_scene_depth.texture != snapshot.texture)
    HostDevice::RetireResource(std::move(g_temporal_scene_depth.texture));
  g_temporal_scene_depth = std::move(snapshot);
}
bool TemporalInputsCandidate() {
  static const bool enabled = std::getenv("LEGO_NATIVE_TEMPORAL_INPUTS") != nullptr;
  return NativeFsr3Enabled() || enabled;
}

// The saved LOTR world/prepass programs have a verified c0..3 camera
// matrix. Unsupported programs are deliberately not assigned history.
bool HasVerifiedTemporalCamera(u64 shader) {
  return shader == 0xB881E7DFA89CF8F4ull || shader == 0xAF42C59B97499AB6ull ||
      shader == 0x6CB6E1C4106F4571ull || shader == 0x1E13090CD0FA6780ull ||
      shader == 0xFFA4DD4B364A999Dull || shader == 0xE669A744921D81E3ull ||
      shader == 0x83291542DC4A71F8ull || shader == 0xC0C579A9C7D3ACD9ull || shader == 0x4025DEF9916B64ECull;
}

bool HasVerifiedTemporalTransform(u64 shader) {
  // C29D's captured position path is object c48..51 followed by camera
  // c0..3. Both specializations match the verified 6CB prepass under 128
  // controlled transforms and pass current/previous motion GPU checks.
  // This grants transform history, never ownership of the scene camera.
  // B8's exact companions preserve position/depth under128 affine transforms,
  // including camera and texture-driven wind changes. Its c4..7 view matrix
  // affects varyings, while position uses object c48..51 then camera c0..3.
  // CPU texture contents must still match, and the scene camera must already
  // be owned by an independently verified scene draw.
  return HasVerifiedTemporalCamera(shader) || shader == 0xC29DAB774F404DB9ull ||
      shader == 0xB8D5551806386C1Bull;
}

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
  float pixel_position_scale = 1.0f;
  float viewport_scale_x = 0.0f;
  float viewport_scale_y = 0.0f;
  u32 alpha_function = 7;
  u32 viewport_mode = 0;
  float color_output_scale[4]{1, 1, 1, 1};
};
static_assert(sizeof(SharedConstants) == 624);
static_assert(offsetof(SharedConstants, booleans) == 512);
static_assert(offsetof(SharedConstants, color_output_scale) == 608);
static_assert(offsetof(SharedConstants, pixel_position_scale) == 588);
static_assert(offsetof(SharedConstants, alpha_function) == 600);
static_assert(offsetof(SharedConstants, viewport_scale_x) == 592);
static_assert(offsetof(SharedConstants, viewport_mode) == 604);
static_assert(offsetof(SharedConstants, alpha_threshold) == 556);

struct PipelineKey {
  u32 vertex_shader = 0;
  u32 pixel_shader = 0;
  u32 declaration = 0;
  // Addresses are diagnostic metadata. Identical programs/layouts may be
  // placed at different addresses, and a released address can be reused.
  // Neither event changes PSO identity independently of the content hashes.
  u64 vertex_shader_hash = 0;
  u64 pixel_shader_hash = 0;
  u64 declaration_hash = 0;
  std::array<u32, kNativeRenderTargets> render_target_formats{};
  u32 depth_format = 0;
  u32 depth_control = 0;
  u32 clip_disable = 0;
  u32 raster_control = 0;
  u32 stencil_masks = 0;
  u32 stencil_face = 0;
  u32 color_write_mask = 0;
  std::array<u32, kNativeRenderTargets> blend_controls{0x10001, 0x10001, 0x10001, 0x10001};
  u32 topology = 0;
  u32 spec_constants = 0;
  // Polygon offset in host units: D3D12 integer DepthBias and the bits of the
  // float SlopeScaledDepthBias.
  i32 depth_bias = 0;
  u32 slope_scaled_depth_bias = 0;
  // Binding metadata only on D3D12: IASetVertexBuffers supplies the stride.
  // The PSO contains input format/offset/classification, never buffer stride.
  std::array<u32, kNativeVertexStreams> strides{};
  // Input slots stepped per instance (Xenos manual instancing, instanced_fetch.h).
  u32 instance_slots = 0;
  // Vertex shader is the instanced-constants variant (instanced_constants.h).
  bool instanced_constants = false;
  bool multi_rate_fetch = false;
  bool operator==(const PipelineKey& other) const {
    return vertex_shader_hash == other.vertex_shader_hash &&
        bool(vertex_shader) == bool(other.vertex_shader) &&
        bool(pixel_shader) == bool(other.pixel_shader) &&
        pixel_shader_hash == other.pixel_shader_hash &&
        declaration_hash == other.declaration_hash &&
        render_target_formats == other.render_target_formats &&
        depth_format == other.depth_format && depth_control == other.depth_control &&
        clip_disable == other.clip_disable && raster_control == other.raster_control &&
        stencil_masks == other.stencil_masks && stencil_face == other.stencil_face &&
        color_write_mask == other.color_write_mask && blend_controls == other.blend_controls &&
        topology == other.topology && spec_constants == other.spec_constants &&
        depth_bias == other.depth_bias && slope_scaled_depth_bias == other.slope_scaled_depth_bias &&
        instance_slots == other.instance_slots && instanced_constants == other.instanced_constants &&
        multi_rate_fetch == other.multi_rate_fetch;
  }
};

struct PipelineKeyHash {
  size_t operator()(const PipelineKey& key) const {
    size_t hash = 1469598103934665603ull;
    const auto mix = [&](u64 value) { hash = (hash ^ value) * 1099511628211ull; };
    mix(bool(key.vertex_shader)); mix(bool(key.pixel_shader));
    mix(key.vertex_shader_hash); mix(key.pixel_shader_hash); mix(key.declaration_hash);
    for (const auto format : key.render_target_formats) mix(format);
    mix(key.depth_format); mix(key.depth_control); mix(key.color_write_mask); mix(key.clip_disable);
    mix(key.stencil_masks); mix(key.stencil_face);
    mix(key.raster_control);
    for (const auto blend : key.blend_controls) mix(blend);
    mix(key.topology); mix(key.spec_constants);
    mix(u32(key.depth_bias)); mix(key.slope_scaled_depth_bias); mix(key.instance_slots);
    // Base keys keep their hash.
    if (key.instanced_constants) mix(1);
    if (key.multi_rate_fetch) mix(2);
    return hash;
  }
};

bool RetainedDrawPreparationEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_RETAINED_DRAW_STATE");
    const bool selected = !value || std::strcmp(value, "0") != 0;
    REXLOG_INFO("Native draw preparation: {}", selected ? "retained state" : "legacy per-draw state");
    return selected;
  }();
  return enabled;
}

struct PreparedDrawState {
  PreparedConstantBanks<DrawUploadSlice> constants;
  PreparedValue<PipelineKey, plume::RenderPipeline*> pipeline;
  PreparedValue<std::array<u64, 5>, VertexLayoutPlan> vertex_layout;
  void Reset() { constants.Reset(); pipeline.Reset(); vertex_layout.Reset(); }
};
PreparedDrawState g_prepared_draw_serial;
PreparedDrawState& Select_g_prepared_draw() {
  if (CurrentDrawPreparation()) { static thread_local PreparedDrawState state; return state; }
  return g_prepared_draw_serial;
}
#define g_prepared_draw Select_g_prepared_draw()

struct CapturedMultiRateInput;
struct ParallelDrawWork {
  std::shared_ptr<D3DDevice> device;
  DrawBindings bindings;
  ShaderBindingsSnapshot shaders;
  VertexDeclarationView declaration;
  std::vector<plume::RenderInputElement> elements;
  std::vector<u32> guest_types;
  u32 primitive = 0, start = 0, count = 0;
  i32 base_vertex = 0;
  bool indexed = false, constants_ready = false, geometry_ready = false;
  bool geometry_valid = false, use_windows = false;
  u64 serial = 0;
  u32 missing_textures = 0, min_index = 0;
  NativeRenderExtent raster_extent;
  std::array<float, 4> output_scales{};
  std::array<TexturePreparationSnapshot, kNativeTextureSlots> texture_inputs;
  std::array<TextureResourceView, kNativeTextureSlots> textures;
  std::array<BufferPreparationSnapshot, kNativeVertexStreams> vertex_inputs;
  BufferPreparationSnapshot index_input;
  std::shared_ptr<const IndexBoundsSnapshot> index_bounds;
  std::shared_ptr<const CapturedMultiRateInput> multi_rate_input;
  DrawUploadSlice multi_rate_upload;
  u32 multi_rate_upload_capacity=0;
  bool multi_rate_capture_attempted=false;
  IndexDrawWindow index_window;
  std::shared_ptr<const VertexLayoutPlan> layout;
  std::array<bool, kNativeVertexStreams> active_streams{};
  std::array<VertexBufferWindow, kNativeVertexStreams> windows{};
  std::array<PreparedBufferVersion, kNativeVertexStreams> vertices;
  PreparedBufferVersion indices;
  DrawUploadSlice allocation;
  std::array<DrawUploadSlice, 3> constants;
  SharedConstants shared;
  // GetPipeline's arguments for the last plain single draw recorded from this work; its
  // declaration is this work's own.
  PipelineKey pipeline_key;
  plume::RenderPrimitiveTopology pipeline_topology = plume::RenderPrimitiveTopology::UNKNOWN;
  plume::RenderFormat pipeline_depth_format = plume::RenderFormat::UNKNOWN;
  bool pipeline_recorded = false;
  void RecordPipeline(const PipelineKey& key, plume::RenderPrimitiveTopology topology,
                      plume::RenderFormat depth_format) {
    pipeline_key = key;
    pipeline_topology = topology;
    pipeline_depth_format = depth_format;
    pipeline_recorded = true;
  }
  void Reset() {
    device.reset();
    shaders = {};
    declaration = {};
    elements.clear();
    guest_types.clear();
    indexed = constants_ready = geometry_ready = geometry_valid = use_windows = false;
    serial = missing_textures = min_index = 0;
    texture_inputs = {};
    vertex_inputs = {};
    index_input = {};
    index_bounds.reset();
    multi_rate_input.reset();multi_rate_upload={};multi_rate_upload_capacity=0;
    multi_rate_capture_attempted=false;
    index_window = {};
    active_streams = {};
    windows = {};
    vertices = {};
    indices = {};
    allocation = {};
    constants = {};
    pipeline_key = {};
    pipeline_topology = plume::RenderPrimitiveTopology::UNKNOWN;
    pipeline_depth_format = plume::RenderFormat::UNKNOWN;
    pipeline_recorded = false;
  }
};
thread_local ParallelDrawWork* t_preparing_draw = nullptr;
thread_local ParallelDrawWork* t_recording_draw = nullptr;
thread_local const ParallelDrawWork* t_multi_rate_draw = nullptr;
struct MultiRateDrawScope {
  const ParallelDrawWork* previous;
  explicit MultiRateDrawScope(const ParallelDrawWork& draw):previous(std::exchange(t_multi_rate_draw,&draw)){}
  ~MultiRateDrawScope(){t_multi_rate_draw=previous;}
};
const ParallelDrawWork* RecordedMultiRateDraw(){return t_recording_draw?t_recording_draw:t_multi_rate_draw;}
std::vector<std::shared_ptr<ParallelDrawWork>> g_draw_batch;
struct PendingDrawBatch {
  std::shared_ptr<std::vector<std::shared_ptr<ParallelDrawWork>>> draws;
  std::vector<std::future<void>> jobs;
};
std::vector<PendingDrawBatch> g_pending_draw_batches;
std::atomic<bool> g_draw_batch_pending{false};
std::atomic<int> g_draw_benchmark_mode{-1};

std::mutex g_pipeline_mutex;
PipelineCreationGate<PipelineKey, PipelineKeyHash> g_pipeline_creation;
PipelineCreationGate<PipelineKey, PipelineKeyHash> g_temporal_pipeline_creation;
std::unordered_map<PipelineKey, std::unique_ptr<plume::RenderPipeline>,
                   PipelineKeyHash>
    g_pipelines;
std::unordered_map<PipelineKey, std::unique_ptr<plume::RenderPipeline>, PipelineKeyHash> g_temporal_pipelines;
std::atomic<u32> g_unsupported_draw_logs{0};
DrawTiming g_draw_timing_serial;
DrawTiming& Select_g_draw_timing() {
  if (CurrentDrawPreparation()) { static thread_local DrawTiming state; return state; }
  return g_draw_timing_serial;
}
#define g_draw_timing Select_g_draw_timing()
bool g_seen_tiled_pass = false; // Serialized by LockRecording; diagnostics only.

struct DrawTimer {
  bool enabled = NativeTextureTimingEnabled();
  size_t stage = 0;
  std::chrono::steady_clock::time_point last;
  std::array<double, 7> elapsed{};
  DrawTimer() {
    // Keep workload counts available to the per-present cadence probe even
    // when detailed per-draw clocks are disabled.
    ++g_draw_timing.calls;
    if (enabled) {
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
    static thread_local u32 slow_draws = 0;
    if (total > 250.0 && slow_draws++ < 24)
      REXLOG_INFO("Native slow draw: total={:.2f}ms bindings={:.2f} begin={:.2f} "
          "pipeline={:.2f} constants={:.2f} vertices={:.2f} issue={:.2f} tail={:.2f}",
          total, elapsed[0], elapsed[1], elapsed[2], elapsed[3], elapsed[4],
          elapsed[5], elapsed[6]);
  }
};

enum class ConstantCost : size_t { kShadow, kUpload, kBindings, kTextures, kRestore };
struct ConstantTimer {
  bool enabled = !t_preparing_draw && NativeTextureTimingEnabled();
  ConstantCost stage;
  std::chrono::steady_clock::time_point last;
  explicit ConstantTimer(ConstantCost initial) : stage(initial) {
    if (enabled) last = std::chrono::steady_clock::now();
  }
  void Next(ConstantCost next) {
    if (!enabled) return;
    const auto now = std::chrono::steady_clock::now();
    g_draw_timing.constants_ms[static_cast<size_t>(stage)] +=
        std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    stage = next;
  }
  ~ConstantTimer() { Next(stage); }
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

void ApplyCullPipeline(plume::RenderGraphicsPipelineDesc& desc,
                       u32 control, bool polygonal) {
  desc.cullMode = plume::RenderCullMode::NONE;
  if (!(control & kNativeCullCandidate)) return;
  const auto raster = DecodeNativeCullState(control, polygonal);
  // Winding also controls SV_IsFrontFace when stencil is disabled.
  desc.frontFace = raster.clockwise ? plume::RenderFrontFace::CLOCKWISE
                                   : plume::RenderFrontFace::COUNTER_CLOCKWISE;
  if (raster.faces == 1) desc.cullMode = plume::RenderCullMode::FRONT;
  else if (raster.faces == 2) desc.cullMode = plume::RenderCullMode::BACK;
}

plume::RenderRect CullDrawScissor(plume::RenderRect scissor,
                                 u32 control, bool polygonal) {
  // D3D12 has no BOTH cull mode. Empty coverage preserves the draw and its
  // vertex processing/query scope, while producing no pixel/depth/stencil work.
  if ((control & kNativeCullCandidate) &&
      DecodeNativeCullState(control, polygonal).discard) return {0, 0, 0, 0};
  return scissor;
}

StateCallCapture* NativeStateCalls() {
  static std::unique_ptr<StateCallCapture> capture = [] {
    const char* output = std::getenv("LEGO_NATIVE_STATE_CALLS");
    if (!output || !*output) return std::unique_ptr<StateCallCapture>{};
    const char* trigger = std::getenv("LEGO_NATIVE_STATE_CALLS_TRIGGER");
    return std::make_unique<StateCallCapture>(output, trigger ? trigger : "");
  }();
  return capture.get();
}

void BindNativeDrawLayout(plume::RenderCommandList* commands,
                          const DrawDeviceView& view, StateCallLedger* diagnostic) {
  if (diagnostic) diagnostic->Layout(view.pipeline_layout);
  commands->setGraphicsPipelineLayout(view.pipeline_layout);
}

void BindNativeDrawDescriptorSets(plume::RenderCommandList* commands,
                                  const DrawDeviceView& view, StateCallLedger* diagnostic) {
  if (diagnostic) {
    for (u32 i = 0; i < 3; ++i) diagnostic->Descriptor(view.texture_descriptors, i);
    diagnostic->Descriptor(view.sampler_descriptors, 3);
  }
  commands->setGraphicsDescriptorSet(view.texture_descriptors, 0);
  commands->setGraphicsDescriptorSet(view.texture_descriptors, 1);
  commands->setGraphicsDescriptorSet(view.texture_descriptors, 2);
  commands->setGraphicsDescriptorSet(view.sampler_descriptors, 3);
}

void BindNativeDrawPipeline(plume::RenderCommandList* commands,
                            plume::RenderPipeline* pipeline, StateCallLedger* diagnostic) {
  if (diagnostic) diagnostic->Pipeline(pipeline);
  commands->setPipeline(pipeline);
}

// Included inside draw.cpp's existing namespace. Private integration candidate.
struct GatheredMultiRateDraw {
  std::array<plume::RenderVertexBufferView,kNativeVertexStreams> views;
  std::array<plume::RenderInputSlot,kNativeVertexStreams> slots;
  u32 count = 0;
  u64 content_hash = 0;
};

struct CapturedMultiRateInput {
  std::array<std::vector<u8>,11> bytes;
  BufferResourceView index_metadata;
  MultiRateIndexState index_state;
  u64 declaration_hash = 0, content_hash = 0;
  u32 start = 0, input_count = 0, count = 0;
  i32 base_vertex = 0;
  bool indexed = false, quads = false;
};

bool MultiRateFetchShader(u64 shader) { return shader == 0xF77D99A4104EA49Bull; }

bool MultiRateListPrimitive(u32 primitive) {
  using rex::graphics::xenos::PrimitiveType;
  return primitive==u32(PrimitiveType::kPointList) || primitive==u32(PrimitiveType::kLineList) ||
      primitive==u32(PrimitiveType::kTriangleList) || primitive==u32(PrimitiveType::kQuadList);
}

// Xenos UBYTE4/BYTE4 integer fetches produce numeric float registers. The
// gathered host record already has room for float4, so convert these exact
// small integers before upload instead of requiring every integer-mask VS.
// All other formats retain the original IA decode and shader fix-ups.
bool ExpandNativeMultiRateByteInput(std::span<u8> records, bool signed_bytes) {
  if (records.size()%16) return false;
  for (size_t record=0;record<records.size();record+=16) {
    const std::array<u8,4> bytes{records[record],records[record+1],records[record+2],records[record+3]};
    float values[4];
    for (u32 c=0;c<4;++c)
      values[c]=float(i32(bytes[c])-(signed_bytes && bytes[c]>=128 ? 256 : 0));
    std::memcpy(records.data()+record,values,sizeof(values));
  }
  return true;
}

bool CaptureNativeMultiRateInput(const D3DDevice* device, const DrawBindings& bindings,
    const VertexDeclarationView& declaration, const BufferResourceView& expected_index,
    bool indexed, u32 start, u32 count, i32 base_vertex, bool quads,
    std::shared_ptr<const CapturedMultiRateInput>& output) {
  std::array<plume::RenderInputElement,64> remapped;
  std::array<MultiRateFetchChannel,11> channels;
  u32 channel_count = 0;
  if (!BuildMultiRateFetchLayout(std::span(declaration.elements,declaration.element_count),
          std::span<plume::RenderInputElement>(remapped),std::span(channels),channel_count,
          [](auto format) { return plume::RenderFormatSize(format); })) return false;
  const u32 vertices = MultiRateOutputVertexCount(count,quads);
  const u64 total = u64(vertices)*16*channel_count;
  if (!vertices || total>64u*1024*1024 || !device) return false;
  const auto* shadow = reinterpret_cast<const u8*>(device);
  const MultiRateIndexState index_state{
      indexed ? u32(base_vertex) : 0,
      u32(*reinterpret_cast<const be_u32*>(shadow+10448))&0xFFFFFF,
      u32(*reinterpret_cast<const be_u32*>(shadow+10444))&0xFFFFFF};
  // Pin every adopted owner while its CPU mirror is consumed. GPU commands
  // reference only the newly gathered frame upload, never these CPU mirrors.
  std::array<BufferPreparationSnapshot,kNativeVertexStreams> sources;
  std::array<std::vector<u8>,kNativeVertexStreams> mirrors;
  std::array<bool,kNativeVertexStreams> captured{};
  BufferPreparationSnapshot indices;
  std::span<const u8> index_bytes;
  if (indexed) {
    indices = CaptureBufferPreparation(bindings.index_buffer,BufferKind::kIndex);
    const auto& current = indices.metadata;
    if (!indices.owner || !current.mirror_address ||
        current.mirror_address!=expected_index.mirror_address ||
        current.length!=expected_index.length || current.guest_format!=expected_index.guest_format)
      return false;
    index_bytes = {REX_KERNEL_MEMORY()->TranslateVirtual<const u8*>(current.mirror_address),current.length};
  }
  std::vector<u32> order(vertices);
  if (!BuildMultiRateVertexOrder(index_bytes,indexed,expected_index.guest_format==1?2:4,
          start,count,quads,index_state,order)) return false;
  auto result=std::make_shared<CapturedMultiRateInput>();
  auto& gathered=result->bytes;
  u32 channel_index = 0;
  for (u32 e=0;e<declaration.element_count;++e) {
    const auto& element=declaration.elements[e];
    if (MultiRateFetchOrdinal(element.semanticName,element.semanticIndex)==11) continue;
    const auto& channel=channels[channel_index];
    const auto& binding=bindings.vertex_streams[channel.source_slot];
    const bool zero=binding.buffer==0;
    if (!zero && !captured[channel.source_slot]) {
      sources[channel.source_slot]=CaptureBufferPreparation(binding.buffer,BufferKind::kVertex);
      const auto& source=sources[channel.source_slot];
      if (!source.owner || !source.metadata.mirror_address || !source.metadata.length) return false;
      const auto* raw=REX_KERNEL_MEMORY()->TranslateVirtual<const u8*>(source.metadata.mirror_address);
      // Snapshot once per stream, so all differently indexed attributes use
      // the same CPU version even if a later guest write changes the mirror.
      mirrors[channel.source_slot].assign(raw,raw+source.metadata.length);
      captured[channel.source_slot]=true;
    }
    const auto& source=sources[channel.source_slot];
    if (!zero && (!source.owner || !source.metadata.mirror_address || !source.metadata.length)) return false;
    const auto raw=zero ? std::span<const u8>{} : std::span<const u8>(mirrors[channel.source_slot]);
    MultiRateFieldOrder byte_order=MultiRateFieldOrder::kDwordOnly;
    if ((declaration.reversed_byte_elements>>e)&1) byte_order=MultiRateFieldOrder::kReverse4;
    if ((declaration.half_swapped_elements>>e)&1) {
      // Conflicting field conversions must not silently choose one.
      if (byte_order!=MultiRateFieldOrder::kDwordOnly) return false;
      byte_order=((declaration.half_swapped_wide_elements>>e)&1)
          ? MultiRateFieldOrder::kHalfSwap8 : MultiRateFieldOrder::kHalfSwap4;
    }
    auto& bytes=gathered[channel_index];bytes.resize(u64(vertices)*16);
    if (!GatherMultiRateGuestChannel(raw,binding.stride?binding.stride:16,binding.offset,
            channel,byte_order,order,bytes,16,zero)) {
      static std::atomic<u32> failures{0};
      if (failures.fetch_add(1)<16) REXLOG_WARN(
          "Native multi-rate gather rejected: stream={} buffer={:08X} bytes={} stride={} offset={} field={} width={} count={}",
          channel.source_slot,binding.buffer,source.metadata.length,binding.stride,binding.offset,
          channel.source_offset,channel.bytes,vertices);
      return false;
    }
    if (element.format==plume::RenderFormat::R8G8B8A8_UINT ||
        element.format==plume::RenderFormat::R8G8B8A8_SINT) {
      const bool signed_bytes=element.format==plume::RenderFormat::R8G8B8A8_SINT ||
          ((declaration.signed_integer_elements>>e)&1);
      if (!ExpandNativeMultiRateByteInput(bytes,signed_bytes)) return false;
    }
    ++channel_index;
  }
  if (channel_index!=channel_count) return false;
  result->index_metadata=expected_index;result->index_state=index_state;
  result->declaration_hash=declaration.content_hash;
  result->start=start;result->input_count=count;result->count=vertices;
  result->base_vertex=base_vertex;result->indexed=indexed;result->quads=quads;
  result->content_hash=XXH3_64bits(order.data(),order.size()*sizeof(u32));
  for (const auto& bytes:gathered)
    result->content_hash=XXH3_64bits_withSeed(bytes.data(),bytes.size(),result->content_hash);
  output=std::move(result);
  return true;
}

u32 NativeMultiRateUploadBytes(const CapturedMultiRateInput& input) {
  const u64 bytes=u64(input.count)*16*input.bytes.size();
  return bytes && bytes<=64u*1024*1024 ? u32(bytes) : 0;
}

bool MatchesNativeMultiRateInput(const CapturedMultiRateInput& input,const D3DDevice* device,
    const VertexDeclarationView& declaration,bool indexed,u32 start,u32 count,i32 base,bool quads) {
  if (!device) return false;
  const auto* shadow=reinterpret_cast<const u8*>(device);
  return input.declaration_hash==declaration.content_hash && input.indexed==indexed &&
      input.start==start && input.input_count==count && input.base_vertex==base && input.quads==quads &&
      input.index_state.minimum==(u32(*reinterpret_cast<const be_u32*>(shadow+10448))&0xFFFFFF) &&
      input.index_state.maximum==(u32(*reinterpret_cast<const be_u32*>(shadow+10444))&0xFFFFFF);
}

bool UploadNativeMultiRateInput(const CapturedMultiRateInput& input,plume::RenderBuffer* null_buffer,
    GatheredMultiRateDraw& output,DrawUploadSlice reserved={},u32 reserved_bytes=0) {
  const auto total=NativeMultiRateUploadBytes(input);
  if (!total || !null_buffer || (reserved && reserved_bytes<total)) return false;
  for (const auto& bytes:input.bytes) if (bytes.size()!=u64(input.count)*16) return false;
  // One immutable region for all attributes: the full-preparation coordinator
  // reserves it from a fence-retained arena. Serial paths use the frame ring.
  const auto upload=reserved ? reserved : HostDevice::AllocateDrawUpload(total);
  if (!upload) return false;
  GatheredMultiRateDraw prepared;
  prepared.count=input.count;prepared.content_hash=input.content_hash;
  for (u32 i=0;i<kNativeVertexStreams;++i) {
    prepared.slots[i]=plume::RenderInputSlot(i,16);
    prepared.views[i]=plume::RenderVertexBufferView(null_buffer->at(0),256);
  }
  u64 offset=0;
  for (u32 i=0;i<input.bytes.size();++i) {
    const auto& bytes=input.bytes[i];
    std::memcpy(static_cast<u8*>(upload.mapped)+offset,bytes.data(),bytes.size());
    prepared.views[i]=plume::RenderVertexBufferView(upload.buffer->at(upload.offset+offset),bytes.size());
    offset+=bytes.size();
  }
  output=std::move(prepared);
  return true;
}

bool PrepareNativeMultiRateGather(const D3DDevice* device,const DrawBindings& bindings,
    const VertexDeclarationView& declaration,const BufferResourceView& expected_index,
    bool indexed,u32 start,u32 count,i32 base_vertex,bool quads,
    plume::RenderBuffer* null_buffer,GatheredMultiRateDraw& output) {
  std::shared_ptr<const CapturedMultiRateInput> captured;
  return CaptureNativeMultiRateInput(device,bindings,declaration,expected_index,indexed,start,count,
      base_vertex,quads,captured) && UploadNativeMultiRateInput(*captured,null_buffer,output);
}


// Gathers one stream of a de-indexed instanced-fetch draw (instanced_fetch.h)
// into this frame's upload ring, converted like a regular vertex upload.
struct ExpandedVertexStream {
  plume::RenderBuffer* buffer = nullptr;
  u64 offset = 0;
  u32 length = 0;
};
ExpandedVertexStream ExpandInstancedFetchStream(u32 mirror_address, u32 buffer_bytes, u32 stride,
    u32 stream_offset, const InstancedFetchPlan& plan, bool per_instance,
    std::span<const u32> converted_fields) {
  if (!mirror_address || stride < 4 || stride % 4 || stream_offset > buffer_bytes ||
      !plan.expand_vertices || !plan.expand_count) return {};
  const u64 length = u64(plan.expand_count) * stride;
  if (length > 64u * 1024 * 1024) return {};
  thread_local std::vector<u8> gathered, converted;
  gathered.resize(length);
  converted.resize(length);
  const auto* source = REX_KERNEL_MEMORY()->TranslateVirtual<const u8*>(mirror_address + stream_offset);
  if (!GatherInstancedFetchRecords(gathered.data(), source, buffer_bytes - stream_offset, stride,
          plan.expand_vertices, plan.expand_count, plan.vertices, per_instance)) {
    static std::atomic<u32> logs{0};
    if (logs.fetch_add(1) < 16)
      REXLOG_WARN("Native GPU: instanced fetch record outside stream data={:08X} bytes={} "
                  "offset={} stride={} N={} per_instance={}", mirror_address, buffer_bytes,
                  stream_offset, stride, plan.vertices, per_instance);
    return {};
  }
  // Common DWORD endian swap, then the packed-field fix-ups of this stream.
  for (u64 i = 0; i < length; i += 4)
    for (u32 b = 0; b < 4; ++b) converted[i + b] = gathered[i + 3 - b];
  if (!ApplyVertexByteOrder(converted.data(), length, {stride, 0, converted_fields})) return {};
  const auto upload = HostDevice::AllocateDrawUpload(u32(length));
  if (!upload) return {};
  std::memcpy(upload.mapped, converted.data(), length);
  return {upload.buffer, upload.offset, u32(length)};
}

void BindNativeDrawVertexBuffers(plume::RenderCommandList* commands,
    const DrawDeviceView& view,
    const std::array<plume::RenderVertexBufferView,kNativeVertexStreams>& buffers,
    const std::array<plume::RenderInputSlot,kNativeVertexStreams>& slots) {
  if (CurrentDrawPreparation()) {
    commands->setVertexBuffers(0, buffers.data(), buffers.size(), slots.data());
    return;
  }
  if (!g_vertex_binding_backend_ready) {
    const char* value=std::getenv("LEGO_NATIVE_VERTEX_BINDING_CACHE");
    g_vertex_binding_cache_enabled=HostDevice::BackendName()=="D3D12" &&
        (!value || !*value || value[0]!='0');
    g_vertex_binding_backend_ready=true;
  }
  if (!g_vertex_binding_cache_enabled) {
    commands->setVertexBuffers(0,buffers.data(),buffers.size(),slots.data());return;
  }
  g_vertex_bindings.Bind(commands,view.recording_serial,buffers,slots,
      [&](u32 first,u32 count) {
        commands->setVertexBuffers(first,buffers.data()+first,count,slots.data()+first);
      });
}

void SetNativePipelineStencilReference(plume::RenderPipeline* pipeline,
                                       const plume::RenderGraphicsPipelineDesc& desc) {
  // Pinned Plume D3D12 never copies desc.stencilReference into this field,
  // although checkStencilRef consumes it before every draw. Keep the fix local
  // to native D3D12 and set it once before publishing the immutable pipeline.
  static_cast<plume::D3D12GraphicsPipeline*>(pipeline)->stencilRef =
      desc.stencilEnabled ? desc.stencilReference : 0;
}

void ReportTextureBindingProblem(u32 kind, u32 slot, u32 guest) {
  // Lightweight evidence for black/null samples and streaming draw rejection
  // without enabling per-draw traces or GPU readbacks during a normal test.
  static std::array<u64,3> counts{};
  static std::unordered_set<u64> reported;
  if (kind >= counts.size()) return;
  ++counts[kind];
  const u64 identity = (u64(kind)<<32) | guest;
  if (reported.size() < 32 && reported.insert(identity).second) {
    constexpr const char* reasons[]{"unavailable bound resource","upload rejected","view lost after upload"};
    REXLOG_WARN("Native texture binding: {} slot={} guest={:08X} VS={:016X} PS={:016X}",
        reasons[kind],slot,guest,BoundShaderHash(ShaderStage::kVertex),BoundShaderHash(ShaderStage::kPixel));
  }
  if ((counts[0]+counts[1]+counts[2]) % 4096 == 0)
    REXLOG_WARN("Native texture binding totals: unavailable={} upload_rejected={} view_lost={}",counts[0],counts[1],counts[2]);
}

bool BindPreparedConstants(plume::RenderCommandList* commands, ParallelDrawWork& work) {
  auto uploads = work.constants;
  if (work.missing_textures) {
    auto shared = work.shared;
    std::array<TextureFetchWords, 32> fetches{};
    std::array<u32, 32> samplers{};
    u32 mask = 0;
    const auto* bytes = reinterpret_cast<const u8*>(work.device.get());
    for (u32 i = 0; i < kNativeTextureSlots; ++i) {
      if (!(work.missing_textures & (1u << i))) continue;
      TextureFetchWords fetch;
      for (u32 word = 0; word < fetch.size(); ++word) fetch[word] = work.device->fetch_constants[i].dword[word];
      TextureResourceView texture;
      const auto status = PrepareTextureBinding(i, work.bindings.textures[i], fetch,
          u32(bytes[12356 + i]) | (u32(bytes[12382 + i]) << 8), commands,
          (BoundShaderTextureMask(ShaderStage::kVertex) & (1u << i)) != 0, texture);
      if (status == TexturePreparationResult::kUploadFailed) return false;
      if (!texture.texture || texture.descriptor_index == ~u32{0}) continue;
      if (texture.has_sampling_fetch)
        fetch = MergeTextureFetch(fetch, texture.sampling_fetch, bytes[12356 + i], bytes[12382 + i]);
      fetches[i] = fetch; mask |= 1u << i;
      if (texture.d3d_type == static_cast<u32>(D3DResourceType::kVolumeTexture))
        shared.texture_3d[i] = texture.descriptor_index;
      else if (texture.d3d_type == static_cast<u32>(D3DResourceType::kCubeTexture))
        shared.texture_cube[i] = texture.descriptor_index;
      else shared.texture_2d[i] = texture.descriptor_index;
    }
    HostDevice::RegisterSamplers(fetches, mask, samplers);
    for (u32 i = 0; i < 32; ++i) if (mask & (1u << i)) shared.samplers[i] = samplers[i];
    uploads[2] = HostDevice::AllocateDrawUpload(sizeof(shared));
    if (!uploads[2]) return false;
    std::memcpy(uploads[2].mapped, &shared, sizeof(shared));
    TraceNativeBloomBinding(BoundShaderHash(ShaderStage::kPixel),
        BoundShaderTextureMask(ShaderStage::kPixel), CurrentNativeVisualEffects().bloom,
        work.bindings.textures[3], shared.texture_2d[3], work.serial, true);
  }
  for (u32 i = 0; i < uploads.size(); ++i)
    commands->setGraphicsRootDescriptor(uploads[i].buffer->at(uploads[i].offset), i);
  return true;
}

// The preparation workers' constant banks; a merged draw reuses what its head left in them.
PreparedConstantBanks<DrawUploadSlice>& WorkerConstantBanks() {
  static thread_local PreparedConstantBanks<DrawUploadSlice> banks;
  return banks;
}

bool BindConstants(
    plume::RenderCommandList* commands, const D3DDevice* device,
    const DrawBindings& bindings, const VertexDeclarationView& declaration, bool capture_constants,
    u64 recording_serial, u64 vertex_shader_hash, u64 pixel_shader_hash,
    std::array<u8, 0x2000 + 624>* temporal_snapshot = nullptr,
    TemporalTextureHistory* temporal_textures = nullptr,
    const TextureResourceView* composition_input = nullptr,
    bool composition_complete = false) {
  if (t_recording_draw && t_recording_draw->constants_ready)
    return BindPreparedConstants(commands, *t_recording_draw);
  auto& constant_banks = t_preparing_draw ? WorkerConstantBanks() : g_prepared_draw.constants;
  ConstantTimer constant_timing(ConstantCost::kUpload);
  // DispatchDraw holds the recording lock, so these bindings cannot change
  // while resource uploads/conversion helpers prepare this draw.
  const u32 vertex_texture_mask = BoundShaderTextureMask(ShaderStage::kVertex);
  if (temporal_textures) temporal_textures->required = vertex_texture_mask;
  const u32 pixel_texture_mask = BoundShaderTextureMask(ShaderStage::kPixel);
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
    if (!upload) return false;
  }
  constant_timing.Next(ConstantCost::kShadow);
  // Build in cacheable CPU memory. Persistently mapped UPLOAD storage is
  // write-combined and must never be read for diagnostics or partial updates.
  alignas(16) std::array<u8, kSharedOffset + sizeof(SharedConstants)> legacy_data;
  const bool retained = RetainedDrawPreparationEnabled() && !separate_constants;
  auto& data = retained ? constant_banks.Bytes() : legacy_data;
  auto* mapped = data.data();
  const auto* bytes = reinterpret_cast<const u8*>(device);
  const bool disabled_tonemap_dof = pixel_shader_hash == 0x3A47E5DDE66B42C6ull &&
      rex::cvar::GetFlagByName("depth_of_field") == "false";
  if (retained) {
    for (u32 bank = 0; bank < 2; ++bank) {
      const u8* guest = bytes + (bank ? 0x1780 : 0x780);
      constant_banks.UpdateFloatBank(bank, {guest, kConstantsSize}, bank ? disabled_tonemap_dof : 0, CopyBigEndianDwords);
    }
  } else {
    CopyBigEndianDwords(mapped, bytes + 0x780, kConstantsSize);
    CopyBigEndianDwords(mapped + kConstantsSize, bytes + 0x1780, kConstantsSize);
  }

  // Filtering the CoC pass alone leaves the tonemap's sharp/mip blend active.
  // Change only this draw's immutable host constants, never guest state.
  if (pixel_shader_hash == 0x3A47E5DDE66B42C6ull &&
      ApplyDisabledTonemapDof(pixel_shader_hash,
          disabled_tonemap_dof,
          mapped + kConstantsSize, kConstantsSize)) {
    static std::atomic<bool> logged_disabled_mix{false};
    if (!logged_disabled_mix.exchange(true)) {
      REXLOG_INFO("Native GPU: disabled DoF sharp/mip tonemap mix");
    }
  }

  SharedConstants shared;
  const auto raster_extent = t_preparing_draw ? t_preparing_draw->raster_extent :
      NativeTextureExtent(bindings.render_targets[0] ? bindings.render_targets[0] : bindings.depth_stencil);
  if (raster_extent.width) shared.pixel_position_scale =
      float(raster_extent.logical_width) / raster_extent.width;
  // Isolated ABI624 candidate; requires the matching viewport shader banks.
  static const bool viewport_candidate = std::getenv("LEGO_NATIVE_VIEWPORT") != nullptr;
  if (viewport_candidate && u32(*reinterpret_cast<const be_u32*>(bytes + 10572)) == 0x400) {
    const auto target = ResolveTextureResource(bindings.render_targets[0]
        ? bindings.render_targets[0] : bindings.depth_stencil);
    if (!target.width || !target.height) return false;
    shared.viewport_mode = 1;
    const auto extent=NativeTextureExtent(bindings.render_targets[0] ? bindings.render_targets[0] : bindings.depth_stencil);
    shared.viewport_scale_x = 2.0f / extent.logical_width;
    shared.viewport_scale_y = -2.0f / extent.logical_height;
  }
  const auto alpha = DecodeNativeAlphaState(
      *reinterpret_cast<const be_u32*>(bytes + 10556),
      *reinterpret_cast<const be_f32*>(bytes + 10500));
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
    shared.color_output_scale[i] = t_preparing_draw ? t_preparing_draw->output_scales[i] :
        SurfaceColorOutputScale(bindings.render_targets[i]);
  static const bool force_all_textures = std::getenv("LEGO_NATIVE_ALL_TEXTURES") != nullptr;
  const u32 texture_mask = force_all_textures ? ~u32{0} :
      vertex_texture_mask | pixel_texture_mask;
  static const bool trace_constants = std::getenv("LEGO_GPU_SNAPSHOT_DIR") != nullptr;
  if (!t_preparing_draw && trace_constants && pixel_shader_hash == 0x3A47E5DDE66B42C6ull) {
    static thread_local u32 tonemap_logs = 0;
    if (tonemap_logs++ < 12) {
      const auto* vs = reinterpret_cast<const float*>(mapped);
      const auto* ps = reinterpret_cast<const float*>(mapped + kConstantsSize);
      for (u32 index : {4u, 10u, 11u, 12u})
        REXLOG_INFO("Native tonemap constants c{} VS={},{},{},{} PS={},{},{},{}", index,
            vs[index*4], vs[index*4+1], vs[index*4+2], vs[index*4+3],
            ps[index*4], ps[index*4+1], ps[index*4+2], ps[index*4+3]);
    }
  }
  constant_timing.Next(ConstantCost::kTextures);
  std::array<TextureFetchWords, 32> sampler_fetches{};
  u32 sampler_mask = 0;
  const bool scene_sampling = NativeFsr3Enabled() && !composition_complete &&
      raster_extent.logical_width == 1280 && raster_extent.logical_height == 720 &&
      float(device->viewport.width) == 1280 && float(device->viewport.height) == 720 &&
      DecodeDepthState(*reinterpret_cast<const be_u32*>(bytes + 0x2934),
          bindings.depth_stencil != 0).enabled;
  const float fsr_texture_bias = scene_sampling ? NativeFsr3MipBias() : 0.0f;
  const bool bloom = CurrentNativeVisualEffects().bloom;
  for (u32 i = 0; i < kNativeTextureSlots; ++i) {
    if (!(texture_mask & (1u << i))) continue;
    // Descriptor 0 is the initialized black 2D texture. Do this before
    // preparation/missing-texture tracking so deferred repair keeps it black.
    if (SuppressNativeBloomTexture(pixel_shader_hash, i, bloom)) continue;
    const bool overridden = composition_input && i==0;
    TextureFetchWords fetch;
    for (u32 word = 0; word < fetch.size(); ++word) fetch[word] = device->fetch_constants[i].dword[word];
    TextureResourceView texture;
    if (t_preparing_draw) {
      texture = t_preparing_draw->textures[i];
      if (!texture.texture || texture.descriptor_index == ~u32{0}) {
        if (bindings.textures[i]) t_preparing_draw->missing_textures |= 1u << i;
        continue;
      }
    }
    const auto preparation=(overridden || t_preparing_draw) ? TexturePreparationResult::kReady :
        PrepareTextureBinding(i,bindings.textures[i],fetch,
            u32(bytes[12356 + i]) | (u32(bytes[12382 + i]) << 8),commands,
            (vertex_texture_mask & (1u<<i))!=0,texture);
    if (overridden) texture=*composition_input;
    if (preparation==TexturePreparationResult::kUploadFailed) {
      LongProbeEvent("bound_texture_upload_failed", true, "slot=", i,
          "guest=", bindings.textures[i]);
      ReportTextureBindingProblem(1,i,bindings.textures[i]);
      return false;
    }
    if (!texture.texture || texture.descriptor_index == ~u32{0}) {
      if (bindings.textures[i]) ReportTextureBindingProblem(0,i,bindings.textures[i]);
      if (LongProbeEnabled() && bindings.textures[i] &&
          LongProbeOnce(0xBAD1000000000000ull | bindings.textures[i]))
        LongProbeEvent("unavailable_bound_texture", true, "slot=", i,
            "guest=", bindings.textures[i]);
      continue;
    }
    // Preparation verifies animation contents even with memory watches and
    // selects the post-upload depth/color SRV from the validated placement.
    if (temporal_textures)
      temporal_textures->Observe(i, texture.content_hash, texture.content_hash_valid);
    if (LongProbeEnabled()) {
      static thread_local std::unordered_map<u64, TextureFetchWords> seen;
      const u64 identity=(u64(bindings.textures[i])<<8)|i;
      const auto found=seen.find(identity);
      if (found==seen.end() || found->second!=fetch) {
        seen[identity]=fetch;
        LongProbeEvent("texture_fetch_changed", false, "slot=", i,
            "guest=", bindings.textures[i], "descriptor=", texture.descriptor_index,
            "VS=", vertex_shader_hash,
            "PS=", pixel_shader_hash, "fetch=", LongProbeHex(fetch));
      }
    }
    // Use one resource generation for the immutable SRV, upload and sampler.
    // A stale/corrupted device fetch can otherwise clamp a 1024px texture to
    // mip 8 even though the bound resource contains its full mip chain.
    // TU23's mip setters keep the application requests in these byte arrays;
    // SetTexture intersects them with the resource's available levels. Apply
    // that same merge here without modifying guest state or queued draws.
    if (texture.has_sampling_fetch) {
      const auto bound_fetch = MergeTextureFetch(fetch, texture.sampling_fetch,
          bytes[12356 + i], bytes[12382 + i]);
      if (bound_fetch != fetch && LongProbeEnabled())
        LongProbeEvent("sampler_resource_reconciled", false, "slot=", i,
            "guest=", bindings.textures[i], "active=", LongProbeHex(fetch),
            "bound=", LongProbeHex(bound_fetch), "requested_min=", u32(bytes[12356+i]),
            "requested_max=", u32(bytes[12382+i]));
      fetch = bound_fetch;
    }
    if (scene_sampling && !texture.depth && !(vertex_texture_mask & (1u << i)))
      ApplyFsrTextureBias(fetch,fsr_texture_bias);
    sampler_fetches[i] = fetch;
    sampler_mask |= 1u << i;
    if (!t_preparing_draw && trace_constants && i >= 16) {
      struct VertexTextureTrace {
        u32 guest = 0;
        TextureFetchWords fetch{};
        bool valid = false;
      };
      // Trace changes rather than the first 32 draws of the same character.
      // These are the active fetch words, which can differ from its header.
      static thread_local std::array<VertexTextureTrace, kNativeTextureSlots> previous{};
      static thread_local u32 vertex_texture_logs = 0;
      auto& seen = previous[i];
      if (!seen.valid || seen.guest != bindings.textures[i] || seen.fetch != fetch) {
        seen = {bindings.textures[i], fetch, true};
        if (vertex_texture_logs++ < 64) REXLOG_INFO(
            "Native vertex texture slot={} guest={:08X} descriptor={} sampler_key={} VS={:016X} "
            "fetch={:08X},{:08X},{:08X},{:08X},{:08X},{:08X}",
            i, bindings.textures[i], texture.descriptor_index,
            TextureSamplerKey(DecodeTextureSampler(fetch)).words[0],
            vertex_shader_hash,
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
  std::array<u32, 32> sampler_descriptors{};
  TraceNativeBloomBinding(pixel_shader_hash, pixel_texture_mask, bloom,
      bindings.textures[3], shared.texture_2d[3], recording_serial, false);
  HostDevice::RegisterSamplers(sampler_fetches, sampler_mask, sampler_descriptors);
  std::copy(sampler_descriptors.begin(), sampler_descriptors.end(), shared.samplers);
  constant_timing.Next(ConstantCost::kShadow);
  if (retained)
    constant_banks.UpdateShared({reinterpret_cast<const u8*>(&shared), sizeof(shared)});
  else
    std::memcpy(mapped + kSharedOffset, &shared, sizeof(shared));
  if (temporal_snapshot) {
    static_assert(sizeof(SharedConstants) == 624);
    std::memcpy(temporal_snapshot->data(), data.data(), data.size());
  }
  if (capture_constants && HostDevice::LongProbeSnapshotActive()) {
    const auto name=SaveLongProbeConstants({mapped,kSharedOffset+sizeof(shared)});
    if (!name.empty()) LongProbeEvent("draw_constants", false, "file=", name,
        "VS=", vertex_shader_hash,
        "PS=", pixel_shader_hash, "texture_mask=", texture_mask,
        "VS_offset=0 PS_offset=4096 shared_offset=8192 shared_bytes=624");
  }
  constant_timing.Next(ConstantCost::kUpload);
  std::array<DrawUploadSlice, 3> constant_uploads;
  // Bandwidth experiment stays opt-in: real UPLOAD replay has not shown
  // a reliable CPU-time improvement yet, despite fewer uploaded bytes.
  static const bool no_constant_cache = [] {
    const char* value = std::getenv("LEGO_NATIVE_CONSTANT_CACHE");
    return !value || !*value || std::strcmp(value, "0") == 0 ||
        std::getenv("LEGO_NATIVE_NO_CONSTANT_CACHE") != nullptr;
  }();
  if (retained) {
    static const bool shader_ranges = [] {
      const char* value = std::getenv("LEGO_NATIVE_SHADER_CONSTANT_RANGES");
      return value && value[0] == '1';
    }();
    constant_banks.SetUploadRegisters(
        shader_ranges ? BoundShaderFloatConstantCount(ShaderStage::kVertex) : 256,
        shader_ranges ? BoundShaderFloatConstantCount(ShaderStage::kPixel) : 256);
    static const bool record_uploads = [] {
      const char* value = std::getenv("LEGO_NATIVE_RECORD_CONSTANT_UPLOADS");
      return value && value[0] == '1';
    }();
    if (!constant_banks.ResolveUploads(recording_serial,
        [](u32 size) { return t_preparing_draw ? (size <= 8960 ? t_preparing_draw->allocation : DrawUploadSlice{}) :
            HostDevice::AllocateDrawUpload(size); }, constant_uploads,
        [commands](void* dst, const void* src, u32 bytes) {
          if (record_uploads && !t_preparing_draw) RecordNativeUpload(commands, dst, src, bytes);
          else std::memcpy(dst, src, bytes);
        })) return false;
  } else if (separate_constants || no_constant_cache) {
    if (!separate_constants)
      upload = HostDevice::AllocateDrawUpload(data.size());
    if (!upload) return false;
    std::memcpy(upload.mapped, data.data(), data.size());
    for (u32 i = 0, offset = 0; i < constant_uploads.size(); ++i) {
      constant_uploads[i] = {upload.buffer,
          static_cast<u8*>(upload.mapped) + offset, upload.offset + offset};
      offset += DrawConstantCache<DrawUploadSlice>::sizes[i];
    }
  } else {
    static thread_local DrawConstantCache<DrawUploadSlice> cache;
    if (!cache.Resolve(recording_serial, data,
          [](u32 size) { return HostDevice::AllocateDrawUpload(size); },
          constant_uploads)) return false;
  }
  constant_timing.Next(ConstantCost::kBindings);
  if (t_preparing_draw) {
    t_preparing_draw->constants = constant_uploads;
    t_preparing_draw->shared = shared;
    t_preparing_draw->constants_ready = true;
    return true;
  }
  for (u32 i = 0; i < constant_uploads.size(); ++i)
    commands->setGraphicsRootDescriptor(
        constant_uploads[i].buffer->at(constant_uploads[i].offset), i);
  if (separate_upload) {
    separate_upload->unmap();
    HostDevice::RetireResource(std::move(separate_upload));
  }
  return true;
}


bool PrepareDrawGeometryLegacy(ParallelDrawWork& work, bool resolve_misses = false) {
  const auto& bindings = work.bindings;
  const auto& declaration = work.declaration;
  const auto indexed = work.indexed;
  const auto start = work.start, count = work.count;
  const auto base_vertex = work.base_vertex;
  PipelineKey key;
  key.vertex_shader_hash = BoundShaderHash(ShaderStage::kVertex);
  key.pixel_shader_hash = BoundShaderHash(ShaderStage::kPixel);
  key.spec_constants = declaration.has_r11g11b10_normal ? 1u : 0u;
  const auto* state_bytes = reinterpret_cast<const u8*>(work.device.get());
  if (DecodeNativeAlphaState(*reinterpret_cast<const be_u32*>(state_bytes + 10556),
      *reinterpret_cast<const be_f32*>(state_bytes + 10500)).enabled) key.spec_constants |= 2u;
  if (!declaration.supported || !declaration.elements || InstancedFetchShader(key.vertex_shader_hash))
    return false;
  if (MultiRateFetchShader(key.vertex_shader_hash) && MultiRateListPrimitive(work.primitive)) {
    // The coordinator has captured immutable bytes. Preparation must never
    // recapture current guest memory after a later write or pool relocation.
    work.geometry_ready=true;work.use_windows=false;
    work.layout=std::make_shared<const VertexLayoutPlan>(PrepareVertexLayout(declaration,
        [](auto format){return plume::RenderFormatSize(format);}));
    return work.multi_rate_input && work.layout->valid;
  }
  static thread_local VertexInputUsageCache input_usage;
  static thread_local PreparedValue<std::array<u64, 5>, std::shared_ptr<const VertexLayoutPlan>> layout_cache;
  work.layout = layout_cache.Resolve({declaration.content_hash, declaration.element_count,
      declaration.reversed_byte_elements, declaration.half_swapped_elements,
      declaration.half_swapped_wide_elements}, [&] {
        return std::make_shared<const VertexLayoutPlan>(
            PrepareVertexLayout(declaration, [](auto format) { return plume::RenderFormatSize(format); }));
      });
  const auto& layout = *work.layout;
  if (!layout.valid) { work.geometry_ready = true; return false; }
  auto& active_vertex_streams = work.active_streams;
  for (u32 i = 0; i < kNativeVertexStreams; ++i) active_vertex_streams[i] = layout.streams[i].active;
  const auto inspect_vertex_stream = [&](u32 i) -> const BufferResourceView& {
    return work.vertex_inputs[i].metadata;
  };
  const auto& index_metadata = work.index_input.metadata;
  const auto index_offset = work.index_window.offset, index_bytes = work.index_window.length;
  if (indexed && (!work.index_window || !work.index_bounds ||
      !CpuMemoryUnchanged(work.index_bounds->stamp))) return false;
  static const bool window_uploads = [] {
    const char* value = std::getenv("LEGO_NATIVE_BUFFER_WINDOWS");
    return value ? std::strcmp(value, "0") != 0 : REXCVAR_GET(gpu_native_buffer_windows);
  }();
  bool& use_windows = work.use_windows;
  auto& min_index = work.min_index;
  auto& vertex_windows = work.windows;
  InstancedFetchPlan instanced_fetch;
  work.geometry_ready = true;
  if (count) {
    u32 max_index = 0;
    if (indexed) {
      const auto bounds = work.index_bounds->bounds;
      min_index = bounds.min;
      max_index = bounds.max;
      // D3D12's signed base vertex must represent -min_index exactly.
      use_windows = window_uploads && min_index <= INT32_MAX;
    } else {
      min_index = start;
      if (!NonIndexedVertexRange(start, count, max_index)) return false;
      use_windows = window_uploads;
    }
    auto shader = decltype(FindDxil(key.vertex_shader_hash,key.spec_constants)){};
    bool shader_looked_up=false;
    static const bool input_usage_cache_enabled=[] {
      const char* value=std::getenv("LEGO_NATIVE_INPUT_SIGNATURE_CACHE");
      return !value || !*value || value[0]!='0';
    }();
    const auto required_inputs=input_usage.Get(
        {key.vertex_shader_hash,input_usage_cache_enabled?declaration.content_hash:0,
         key.spec_constants,declaration.element_count},
        [&]() -> std::optional<u64> {
          if(declaration.element_count>64)return {};
          shader=FindDxil(key.vertex_shader_hash,key.spec_constants);shader_looked_up=true;
          if(!shader.data || !shader.size)return {};
          u64 used=0;
          for(u32 e=0;e<declaration.element_count;++e) {
            const auto& element=declaration.elements[e];
            const auto reads=ShaderReadsVertexInput({shader.data,shader.size},
                element.semanticName,element.semanticIndex);
            if(!reads || *reads)used|=u64{1}<<e;
          }
          return used;
        });
    if(!required_inputs && !shader_looked_up)
      shader=FindDxil(key.vertex_shader_hash,key.spec_constants);
    // A declaration may retain a secondary stream that this specialization
    // never reads (captured position-only draws retain normal/UV streams).
    // Use the already cached signature, with no retained buffer metadata.
    // Temporal replays and alternate VS banks can have different inputs;
    // keep their existing declaration-based upload path conservative.
    static const bool skip_unused_streams=[] {
      const char* value=std::getenv("LEGO_NATIVE_SKIP_UNUSED_VERTEX_STREAMS");
      return !value || std::strcmp(value,"0")!=0;
    }();
    if (skip_unused_streams && required_inputs && !NativeFsr3Enabled()) {
      std::array<bool, kNativeVertexStreams> shader_streams{};
      for (u32 e=0; e<declaration.element_count; ++e)
        if (*required_inputs & (u64{1}<<e))
          shader_streams[declaration.elements[e].slotIndex]=true;
      if (shader_streams!=active_vertex_streams &&
          !(FloatBlendIndicesInput({declaration.elements,declaration.element_count}) &&
            FindFloatBlendIndicesDxil(key.vertex_shader_hash,key.spec_constants)) &&
          !FindPositionPrecisionDxil(key.vertex_shader_hash,key.spec_constants))
        active_vertex_streams=shader_streams;
    }
    // A failed window optimization must not authorize an out-of-range fetch
    // from the whole buffer. LOTR's captured bad draw reached vertex 254 in
    // a secondary stream with only 102 records, and wrote giant black triangles.
    for (u32 e=0; e<declaration.element_count; ++e) {
      const auto& element=declaration.elements[e];
      if(required_inputs) {
        if(!(*required_inputs & (u64{1}<<e)))continue;
      } else {
        const auto reads=ShaderReadsVertexInput({shader.data,shader.size},
            element.semanticName,element.semanticIndex);
        if(reads && !*reads)continue;
      }
      const auto& binding=bindings.vertex_streams[element.slotIndex];
      if (!binding.buffer) continue; // Deliberate synthetic zero attributes.
      const auto& metadata=inspect_vertex_stream(element.slotIndex);
      u32 fetch_min=min_index, fetch_max=max_index;
      i32 fetch_base=indexed?base_vertex:0;
      // Gathered streams check every record themselves.
      if (instanced_fetch.expand && (((instanced_fetch.template_slots |
          instanced_fetch.instance_slots) >> element.slotIndex) & 1)) continue;
      if (instanced_fetch) {
        // Template records 0..N-1; per-instance records follow the instance id.
        const bool per_instance=(instanced_fetch.instance_slots>>element.slotIndex)&1;
        fetch_min=per_instance?instanced_fetch.first_instance:0;
        fetch_max=per_instance?instanced_fetch.last_instance():instanced_fetch.vertices-1;
        fetch_base=0;
      }
      if (!VertexFetchRangeFits(fetch_min,fetch_max,fetch_base,
          binding.stride?binding.stride:16,binding.offset,element.alignedByteOffset,
          plume::RenderFormatSize(element.format),metadata.length)) {
        static std::atomic<u32> invalid_fetch_logs{0};
        if (invalid_fetch_logs++<32) REXLOG_WARN(
            "Native GPU: rejected vertex fetch VS={:016X} PS={:016X} stream={} "
            "buffer={:08X} bytes={} stride={} offset={} range={}..{} base={}",
            key.vertex_shader_hash,key.pixel_shader_hash,element.slotIndex,
            binding.buffer,metadata.length,binding.stride,binding.offset,
            min_index,max_index,base_vertex);
        return false;
      }
    }
    // Instanced draws bind whole streams; windows follow the plain index range.
    if (instanced_fetch) use_windows = false;
    for (u32 i = 0; use_windows && i < kNativeVertexStreams; ++i) {
      const auto& binding = bindings.vertex_streams[i];
      if (!binding.buffer) continue;  // Synthetic/optional zero stream.
      if (!active_vertex_streams[i]) continue;
      if (!layout.streams[i].widths_valid || layout.streams[i].minimum_stride > binding.stride) use_windows = false;
      if (!use_windows) break;
      const auto& metadata = inspect_vertex_stream(i);
      vertex_windows[i] = DrawVertexWindow(min_index, max_index, indexed ? base_vertex : 0,
          binding.stride, binding.offset, metadata.length);
      if (!vertex_windows[i].length) use_windows = false;
    }
  }

  for (u32 i = 0; i < kNativeVertexStreams; ++i) {
    if (!active_vertex_streams[i] || !bindings.vertex_streams[i].buffer) continue;
    const auto& binding = bindings.vertex_streams[i];
    const auto window = vertex_windows[i];
    if (resolve_misses) {
      // The version is made or found now, in order; the slot cache answers repeats.
      work.vertices[i] = {};
      work.vertices[i].view = use_windows && window.length
          ? ResolveBufferStreamWindow(i, binding.buffer, window.offset, window.length,
                {binding.stride, 0, layout.streams[i].Reversal()})
          : ResolveBufferResourceView(binding.buffer, BufferKind::kVertex,
                {binding.stride, binding.offset, layout.streams[i].Reversal()});
      if (!work.vertices[i].view.buffer) return false;
      continue;
    }
    work.vertices[i] = PrepareCachedBuffer(work.vertex_inputs[i], window.offset, window.length,
        use_windows && window.length, {binding.stride, use_windows ? 0 : binding.offset, layout.streams[i].Reversal()});
    if (!work.vertices[i].view.buffer) {
      if (!resolve_misses) { work.geometry_ready = false; return false; }
      // A stale or absent version is made now, so later draws see it in order.
      work.vertices[i].view = use_windows && window.length
          ? ResolveBufferStreamWindow(i, binding.buffer, window.offset, window.length,
                {binding.stride, 0, layout.streams[i].Reversal()})
          : ResolveBufferResourceView(binding.buffer, BufferKind::kVertex,
                {binding.stride, binding.offset, layout.streams[i].Reversal()});
      if (!work.vertices[i].view.buffer) return false;
    }
  }
  if (indexed) {
    work.indices = resolve_misses ? PreparedBufferVersion{} :
        PrepareCachedBuffer(work.index_input, index_offset, index_bytes, use_windows);
    if (!work.indices.view.buffer) {
      if (!resolve_misses) { work.geometry_ready = false; return false; }
      work.indices.view = use_windows
          ? ResolveBufferIndexWindow(bindings.index_buffer, index_offset, index_bytes)
          : ResolveBufferResourceView(bindings.index_buffer, BufferKind::kIndex);
      if (!work.indices.view.buffer) return false;
    }
  }
  return true;
}

bool PrepareDrawGeometry(ParallelDrawWork& work, bool resolve_misses = false) {
  const auto& declaration = work.declaration;
  static const bool input_usage_cache_enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_INPUT_SIGNATURE_CACHE");
    return !value || !*value || value[0] != '0';
  }();
  if ((MultiRateFetchShader(BoundShaderHash(ShaderStage::kVertex)) && MultiRateListPrimitive(work.primitive)) ||
      !VertexFetchPlanEnabled() || !input_usage_cache_enabled || !work.count ||
      !declaration.content_hash || declaration.element_count > 64)
    return PrepareDrawGeometryLegacy(work, resolve_misses);

  const auto& bindings = work.bindings;
  const auto indexed = work.indexed;
  const auto start = work.start, count = work.count;
  const auto base_vertex = work.base_vertex;
  PipelineKey key;
  key.vertex_shader_hash = BoundShaderHash(ShaderStage::kVertex);
  key.pixel_shader_hash = BoundShaderHash(ShaderStage::kPixel);
  key.spec_constants = declaration.has_r11g11b10_normal ? 1u : 0u;
  const auto* state_bytes = reinterpret_cast<const u8*>(work.device.get());
  if (DecodeNativeAlphaState(*reinterpret_cast<const be_u32*>(state_bytes + 10556),
      *reinterpret_cast<const be_f32*>(state_bytes + 10500)).enabled) key.spec_constants |= 2u;
  if (!declaration.supported || !declaration.elements || InstancedFetchShader(key.vertex_shader_hash))
    return false;

  static const bool skip_unused_streams = [] {
    const char* value = std::getenv("LEGO_NATIVE_SKIP_UNUSED_VERTEX_STREAMS");
    return !value || std::strcmp(value, "0") != 0;
  }();
  const bool reduce_unused = skip_unused_streams && !NativeFsr3Enabled();
  auto& fetch_plans = ThreadVertexFetchPlans();
  const auto& plan = fetch_plans.Resolve({key.vertex_shader_hash, declaration.content_hash,
      declaration.reversed_byte_elements, declaration.half_swapped_elements,
      declaration.half_swapped_wide_elements, PrecompiledShaderPackGeneration(),
      key.spec_constants, declaration.element_count, reduce_unused}, [&]() -> std::shared_ptr<const VertexFetchPlan> {
    const auto shader = FindDxil(key.vertex_shader_hash, key.spec_constants);
    if (!shader.data || !shader.size) return {};
    u64 required_inputs = 0;
    for (u32 e = 0; e < declaration.element_count; ++e) {
      const auto& element = declaration.elements[e];
      const auto reads = ShaderReadsVertexInput({shader.data, shader.size},
          element.semanticName, element.semanticIndex);
      if (!reads || *reads) required_inputs |= u64{1} << e;
    }
    // Alternate shader banks retain declaration-based uploads, exactly as the
    // legacy planner does; their ordinary shader signature still bounds reads.
    const bool reduce_uploads = reduce_unused &&
        !(FloatBlendIndicesInput({declaration.elements, declaration.element_count}) &&
          FindFloatBlendIndicesDxil(key.vertex_shader_hash, key.spec_constants)) &&
        !FindPositionPrecisionDxil(key.vertex_shader_hash, key.spec_constants);
    return std::make_shared<const VertexFetchPlan>(PrepareVertexFetchPlan(declaration,
        required_inputs, reduce_uploads, [](auto format) { return plume::RenderFormatSize(format); }));
  });
  if (!plan) return PrepareDrawGeometryLegacy(work, resolve_misses);
  // The alias owns the entire immutable plan even if its cache entry is evicted.
  work.layout = std::shared_ptr<const VertexLayoutPlan>(plan, &plan->layout);
  const auto& layout = *work.layout;
  if (!layout.valid) { work.geometry_ready = true; return false; }
  auto& active_vertex_streams = work.active_streams;
  for (u32 i = 0; i < kNativeVertexStreams; ++i) active_vertex_streams[i] = layout.streams[i].active;
  const auto inspect_vertex_stream = [&](u32 i) -> const BufferResourceView& {
    return work.vertex_inputs[i].metadata;
  };
  const auto index_offset = work.index_window.offset, index_bytes = work.index_window.length;
  if (indexed && (!work.index_window || !work.index_bounds ||
      !CpuMemoryUnchanged(work.index_bounds->stamp))) return false;
  static const bool window_uploads = [] {
    const char* value = std::getenv("LEGO_NATIVE_BUFFER_WINDOWS");
    return value ? std::strcmp(value, "0") != 0 : REXCVAR_GET(gpu_native_buffer_windows);
  }();
  bool& use_windows = work.use_windows;
  auto& min_index = work.min_index;
  auto& vertex_windows = work.windows;
  work.geometry_ready = true;
  u32 max_index = 0;
  if (indexed) {
    const auto bounds = work.index_bounds->bounds;
    min_index = bounds.min;
    max_index = bounds.max;
    use_windows = window_uploads && min_index <= INT32_MAX;
  } else {
    min_index = start;
    if (!NonIndexedVertexRange(start, count, max_index)) return false;
    use_windows = window_uploads;
  }
  active_vertex_streams = plan->upload_streams;
  for (u32 i = 0; i < kNativeVertexStreams; ++i) {
    const auto& binding = bindings.vertex_streams[i];
    if (!plan->reads[i].required || !binding.buffer) continue;
    const auto& metadata = inspect_vertex_stream(i);
    if (!VertexFetchStreamFits(plan->reads[i], min_index, max_index,
        indexed ? base_vertex : 0, binding.stride ? binding.stride : 16,
        binding.offset, metadata.length)) {
      static std::atomic<u32> invalid_fetch_logs{0};
      if (invalid_fetch_logs++ < 32) REXLOG_WARN(
          "Native GPU: rejected vertex fetch VS={:016X} PS={:016X} stream={} "
          "buffer={:08X} bytes={} stride={} offset={} range={}..{} base={}",
          key.vertex_shader_hash, key.pixel_shader_hash, i,
          binding.buffer, metadata.length, binding.stride, binding.offset,
          min_index, max_index, base_vertex);
      return false;
    }
  }
  for (u32 i = 0; use_windows && i < kNativeVertexStreams; ++i) {
    const auto& binding = bindings.vertex_streams[i];
    if (!binding.buffer) continue;
    if (!active_vertex_streams[i]) continue;
    if (!layout.streams[i].widths_valid || layout.streams[i].minimum_stride > binding.stride) use_windows = false;
    if (!use_windows) break;
    const auto& metadata = inspect_vertex_stream(i);
    vertex_windows[i] = DrawVertexWindow(min_index, max_index, indexed ? base_vertex : 0,
        binding.stride, binding.offset, metadata.length);
    if (!vertex_windows[i].length) use_windows = false;
  }

  // Versions are selected in capture order; the plan never retains a buffer, content stamp, guest
  // header or bounds result.
  for (u32 i = 0; i < kNativeVertexStreams; ++i) {
    if (!active_vertex_streams[i] || !bindings.vertex_streams[i].buffer) continue;
    const auto& binding = bindings.vertex_streams[i];
    const auto window = vertex_windows[i];
    if (resolve_misses) {
      work.vertices[i] = {};
      work.vertices[i].view = use_windows && window.length
          ? ResolveBufferStreamWindow(i, binding.buffer, window.offset, window.length,
                {binding.stride, 0, layout.streams[i].Reversal()})
          : ResolveBufferResourceView(binding.buffer, BufferKind::kVertex,
                {binding.stride, binding.offset, layout.streams[i].Reversal()});
      if (!work.vertices[i].view.buffer) return false;
      continue;
    }
    work.vertices[i] = PrepareCachedBuffer(work.vertex_inputs[i], window.offset, window.length,
        use_windows && window.length, {binding.stride, use_windows ? 0 : binding.offset, layout.streams[i].Reversal()});
    if (!work.vertices[i].view.buffer) {
      if (!resolve_misses) { work.geometry_ready = false; return false; }
      work.vertices[i].view = use_windows && window.length
          ? ResolveBufferStreamWindow(i, binding.buffer, window.offset, window.length,
                {binding.stride, 0, layout.streams[i].Reversal()})
          : ResolveBufferResourceView(binding.buffer, BufferKind::kVertex,
                {binding.stride, binding.offset, layout.streams[i].Reversal()});
      if (!work.vertices[i].view.buffer) return false;
    }
  }
  if (indexed) {
    work.indices = resolve_misses ? PreparedBufferVersion{} :
        PrepareCachedBuffer(work.index_input, index_offset, index_bytes, use_windows);
    if (!work.indices.view.buffer) {
      if (!resolve_misses) { work.geometry_ready = false; return false; }
      work.indices.view = use_windows
          ? ResolveBufferIndexWindow(bindings.index_buffer, index_offset, index_bytes)
          : ResolveBufferResourceView(bindings.index_buffer, BufferKind::kIndex);
      if (!work.indices.view.buffer) return false;
    }
  }
  return true;
}

void PrepareDrawWork(ParallelDrawWork& work) {
  ShaderBindingsScope shaders(work.shaders);
  t_preparing_draw = &work;
  struct Reset { ~Reset() { t_preparing_draw = nullptr; } } reset;
  const auto vertex_mask = BoundShaderTextureMask(ShaderStage::kVertex);
  const auto texture_mask = vertex_mask | BoundShaderTextureMask(ShaderStage::kPixel);
  for (u32 i = 0; i < kNativeTextureSlots; ++i)
    if (texture_mask & (1u << i))
      work.textures[i] = vertex_mask & (1u << i) ? TextureResourceView{} :
          PrepareCachedTexture(work.texture_inputs[i]);
  BindConstants(nullptr, work.device.get(), work.bindings, work.declaration, false,
      work.serial, BoundShaderHash(ShaderStage::kVertex), BoundShaderHash(ShaderStage::kPixel));
  if (!work.geometry_ready) work.geometry_valid = PrepareDrawGeometry(work);
}

plume::RenderPipeline* GetPipeline(
    const PipelineKey& key, const VertexDeclarationView& declaration,
    plume::RenderPrimitiveTopology topology, plume::RenderFormat depth_format) {
  std::unique_lock lock(g_pipeline_mutex);
  if (const auto it = g_pipelines.find(key); it != g_pipelines.end()) {
    return it->second.get();
  }
  lock.unlock();
  auto creation = g_pipeline_creation.Acquire(key);
  lock.lock();
  if (const auto it = g_pipelines.find(key); it != g_pipelines.end()) {
    return it->second.get();
  }
  const bool float_indices = FloatBlendIndicesInput({declaration.elements, declaration.element_count}) &&
      bool(FindFloatBlendIndicesDxil(key.vertex_shader_hash, key.spec_constants));
  const bool precise_position = bool(FindPositionPrecisionDxil(key.vertex_shader_hash, key.spec_constants));
  // Integer UBYTE4 inputs need the variant that converts them like Xenos;
  // without one the original shader is kept (and the mismatch is logged).
  const auto integer_inputs = key.multi_rate_fetch ? IntegerInputSelection{} :
      SelectIntegerInputShader(key.vertex_shader_hash, key.spec_constants, declaration);
  // The instanced-constants variant replaces only the plain vertex shader.
  if (key.instanced_constants && (integer_inputs.shader || float_indices || precise_position)) {
    return nullptr;
  }
  auto* vs = key.instanced_constants ? ResolveInstancedConstantsShader(key.vertex_shader_hash, key.spec_constants)
                       : integer_inputs.shader ? integer_inputs.shader
                       : float_indices ? ResolveFloatBlendIndicesShader(key.vertex_shader_hash, key.spec_constants)
                          : precise_position ? ResolvePositionPrecisionShader(key.vertex_shader_hash, key.spec_constants)
                          : ResolveBoundShader(ShaderStage::kVertex, key.spec_constants);
  auto* ps = ResolveBoundShader(ShaderStage::kPixel, key.spec_constants);
  if (!vs || (key.pixel_shader && !ps)) {
    return nullptr;
  }
  std::array<plume::RenderInputSlot, kNativeVertexStreams> slots;
  for (u32 i = 0; i < slots.size(); ++i) {
    slots[i] = plume::RenderInputSlot(i, key.strides[i] ? key.strides[i] : 16,
        ((key.instance_slots >> i) & 1) ? plume::RenderInputSlotClassification::PER_INSTANCE_DATA
                                        : plume::RenderInputSlotClassification::PER_VERTEX_DATA);
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
  if (depth_format != plume::RenderFormat::UNKNOWN) {
    desc.depthBias = key.depth_bias;
    desc.slopeScaledDepthBias = std::bit_cast<float>(key.slope_scaled_depth_bias);
  }
  const auto stencil = DecodeStencilState(key.depth_control, key.stencil_masks,
      plume::RenderFormatIsStencil(depth_format),
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
    // Plume applies this value immediately before draw submission. A raw
    // OMSetStencilRef earlier in binding can be overwritten by that step.
    desc.stencilReference = stencil.reference;
    desc.frontFace = key.stencil_face ? plume::RenderFrontFace::CLOCKWISE
                                     : plume::RenderFrontFace::COUNTER_CLOCKWISE;
  }
  desc.primitiveTopology = topology;
  ApplyCullPipeline(desc, key.raster_control,
      topology == plume::RenderPrimitiveTopology::TRIANGLE_LIST ||
      topology == plume::RenderPrimitiveTopology::TRIANGLE_STRIP);
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
  desc.inputElements = integer_inputs.shader ? integer_inputs.elements : declaration.elements;
  std::array<plume::RenderInputElement,64> gathered_elements;
  if (key.multi_rate_fetch) {
    std::array<MultiRateFetchChannel,11> channels;
    u32 channel_count=0;
    if (!BuildMultiRateFetchLayout(std::span(desc.inputElements,declaration.element_count),
            std::span<plume::RenderInputElement>(gathered_elements),std::span(channels),channel_count,
            [](auto format) { return plume::RenderFormatSize(format); })) return nullptr;
    desc.inputElements=gathered_elements.data();
    for (u32 e=0;e<declaration.element_count;++e) {
      auto& format=gathered_elements[e].format;
      if (format==plume::RenderFormat::R8G8B8A8_UINT || format==plume::RenderFormat::R8G8B8A8_SINT)
        format=plume::RenderFormat::R32G32B32A32_FLOAT;
    }
  }

  desc.inputElementsCount = declaration.element_count;
  desc.inputSlots = slots.data();
  desc.inputSlotsCount = static_cast<u32>(slots.size());
  // Draw snapshots retain shader/declaration owners; device teardown joins
  // every preparation worker before releasing the device, layout or caches.
  // Variant resolution above remains serialized, but the expensive driver
  // compile need not exclude workers building an unrelated pipeline.
  auto* device = HostDevice::Device();
  lock.unlock();
  const auto creation_start = std::chrono::steady_clock::now();
  auto pipeline = device->createGraphicsPipeline(desc);
  ++g_draw_timing.pipeline_creations;
  g_draw_timing.pipeline_creation_ms += std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - creation_start).count();
    if (!pipeline || !static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->d3d) {
      static std::atomic<u32> failed_pipeline_logs{0};
      if (failed_pipeline_logs.fetch_add(1) < 16)
        REXLOG_ERROR("Native GPU: D3D12 rejected pipeline VS=0x{:08X} PS=0x{:08X} decl=0x{:08X}",
                     key.vertex_shader, key.pixel_shader, key.declaration);
      return nullptr;
  }
  SetNativePipelineStencilReference(pipeline.get(), desc);
  auto* result = pipeline.get();
  static std::atomic<u32> mrt_logs{0};
  if (desc.renderTargetCount > 1 && mrt_logs.fetch_add(1, std::memory_order_relaxed) < 16)
    REXLOG_INFO("Native GPU: MRT pipeline targets={} formats={},{},{},{} masks={:04X} blends={:08X},{:08X},{:08X},{:08X}",
        desc.renderTargetCount, key.render_target_formats[0], key.render_target_formats[1],
        key.render_target_formats[2], key.render_target_formats[3], key.color_write_mask,
        key.blend_controls[0], key.blend_controls[1], key.blend_controls[2], key.blend_controls[3]);
  lock.lock();
  g_pipelines.emplace(key, std::move(pipeline));
  return result;
}

plume::RenderPipeline* InstancedConstantsPipeline(const ParallelDrawWork& draw) {
  if (!draw.pipeline_recorded || !draw.declaration.elements ||
      draw.declaration.content_hash != draw.pipeline_key.declaration_hash) {
    return nullptr;
  }
  auto key = draw.pipeline_key;
  key.instanced_constants = true;
  // A pipeline miss resolves the pixel shader from the bindings the draw recorded with.
  ShaderBindingsScope shaders(draw.shaders);
  return GetPipeline(key, draw.declaration, draw.pipeline_topology, draw.pipeline_depth_format);
}

plume::RenderPipeline* GetTemporalPipeline(
    const PipelineKey& key, const VertexDeclarationView& declaration,
    plume::RenderPrimitiveTopology topology, plume::RenderFormat depth_format) {
  std::unique_lock lock(g_pipeline_mutex);
  if (const auto it = g_temporal_pipelines.find(key); it != g_temporal_pipelines.end()) {
    return it->second.get();
  }
  lock.unlock();
  auto creation = g_temporal_pipeline_creation.Acquire(key);
  lock.lock();
  if (const auto it = g_temporal_pipelines.find(key); it != g_temporal_pipelines.end()) {
    return it->second.get();
  }
  // The audited UI material also draws with depth testing. Use its exact
  // discard/contribution there too; a depth attachment does not make it an
  // opaque world surface. The replay below retains the original depth test.
  const auto coverage_mode = TemporalCoverageForBlend(DecodeColorBlend(key.blend_controls[0]));
  const bool material_coverage = TemporalMaterialReactiveEnabled() &&
      HasTemporalCoverageShader(key.pixel_shader_hash,coverage_mode,key.spec_constants);
  const bool companion = !material_coverage && key.pixel_shader_hash &&
      (key.multi_rate_fetch || !HasIntegerInputShader(key.vertex_shader_hash, key.spec_constants, declaration)) &&
      (HasVerifiedTemporalCamera(key.vertex_shader_hash) ||
       HasTemporalCameraConstants(key.vertex_shader_hash,key.spec_constants)) &&
      !(FloatBlendIndicesInput({declaration.elements, declaration.element_count}) &&
        FindFloatBlendIndicesDxil(key.vertex_shader_hash,key.spec_constants)) &&
      !FindPositionPrecisionDxil(key.vertex_shader_hash,key.spec_constants) &&
      HasTemporalShader(key.vertex_shader_hash,false,key.spec_constants) &&
      HasTemporalShader(key.pixel_shader_hash,true,key.spec_constants);
  auto* vs = companion ? ResolveTemporalShader(key.vertex_shader_hash,false,key.spec_constants)
                       : (!key.multi_rate_fetch && HasIntegerInputShader(key.vertex_shader_hash,key.spec_constants,declaration))
                            ? SelectIntegerInputShader(key.vertex_shader_hash,key.spec_constants,declaration).shader
                       : (FloatBlendIndicesInput({declaration.elements, declaration.element_count}) &&
                          FindFloatBlendIndicesDxil(key.vertex_shader_hash,key.spec_constants)
                            ? ResolveFloatBlendIndicesShader(key.vertex_shader_hash,key.spec_constants)
                            : FindPositionPrecisionDxil(key.vertex_shader_hash,key.spec_constants)
                                ? ResolvePositionPrecisionShader(key.vertex_shader_hash,key.spec_constants)
                            : ResolveBoundShader(ShaderStage::kVertex,key.spec_constants));
  auto* ps = material_coverage
      ? ResolveTemporalCoverageShader(key.pixel_shader_hash,coverage_mode,key.spec_constants)
      : ResolveTemporalShader(companion ? key.pixel_shader_hash : 0,true,key.spec_constants);
  if (!vs || (key.pixel_shader && !ps)) {
    return nullptr;
  }
  std::array<plume::RenderInputSlot, kNativeVertexStreams> slots;
  for (u32 i = 0; i < slots.size(); ++i) {
    slots[i] = plume::RenderInputSlot(i, key.strides[i] ? key.strides[i] : 16,
        ((key.instance_slots >> i) & 1) ? plume::RenderInputSlotClassification::PER_INSTANCE_DATA
                                        : plume::RenderInputSlotClassification::PER_VERTEX_DATA);
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
  if (depth_format != plume::RenderFormat::UNKNOWN) {
    desc.depthBias = key.depth_bias;
    desc.slopeScaledDepthBias = std::bit_cast<float>(key.slope_scaled_depth_bias);
  }
  const auto stencil = DecodeStencilState(key.depth_control, key.stencil_masks,
      plume::RenderFormatIsStencil(depth_format),
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
    // Plume applies this value immediately before draw submission. A raw
    // OMSetStencilRef earlier in binding can be overwritten by that step.
    desc.stencilReference = stencil.reference;
    desc.frontFace = key.stencil_face ? plume::RenderFrontFace::CLOCKWISE
                                     : plume::RenderFrontFace::COUNTER_CLOCKWISE;
  }
  desc.primitiveTopology = topology;
  ApplyCullPipeline(desc, key.raster_control,
      topology == plume::RenderPrimitiveTopology::TRIANGLE_LIST ||
      topology == plume::RenderPrimitiveTopology::TRIANGLE_STRIP);
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
  // Replay tests depth without modifying depth or stencil. EQUAL avoids
  // losing surfaces whose original draw just wrote depth with LESS/GREATER.
  desc.depthWriteEnabled = false;
  // The original draw may have changed stencil. Conservative reactive
  // coverage must not retest that mutated stencil or change it again.
  desc.stencilEnabled = false;
  if (depth_state.write) desc.depthFunction = plume::RenderComparisonFunction::EQUAL;
  desc.stencilWriteMask = 0;
  desc.stencilFrontFace.failOp = desc.stencilFrontFace.passOp = desc.stencilFrontFace.depthFailOp = plume::RenderStencilOp::KEEP;
  desc.stencilBackFace.failOp = desc.stencilBackFace.passOp = desc.stencilBackFace.depthFailOp = plume::RenderStencilOp::KEEP;
  desc.renderTargetCount = 2;
  desc.renderTargetFormat[0] = plume::RenderFormat::R16G16_FLOAT;
  desc.renderTargetFormat[1] = plume::RenderFormat::R8_UNORM;
  desc.renderTargetBlend[0] = desc.renderTargetBlend[1] = plume::RenderBlendDesc::Copy();
  if (material_coverage) {
    // Keep scene motion, and only add rejection where the material contributes.
    // A transparent/additive overlay must never replace the underlying field
    // with zero motion or clear rejection already required by other geometry.
    desc.renderTargetBlend[0].renderTargetWriteMask = 0;
    auto& reactive = desc.renderTargetBlend[1];
    reactive.blendEnabled = true;
    reactive.srcBlend = reactive.dstBlend = reactive.srcBlendAlpha = reactive.dstBlendAlpha = plume::RenderBlend::ONE;
    reactive.blendOp = reactive.blendOpAlpha = plume::RenderBlendOperation::MAX;
  }
  desc.depthTargetFormat = depth_format;
  desc.inputElements = declaration.elements;
  std::array<plume::RenderInputElement,64> gathered_elements;
  if (key.multi_rate_fetch) {
    std::array<MultiRateFetchChannel,11> channels;
    u32 channel_count=0;
    if (!BuildMultiRateFetchLayout(std::span(desc.inputElements,declaration.element_count),
            std::span<plume::RenderInputElement>(gathered_elements),std::span(channels),channel_count,
            [](auto format) { return plume::RenderFormatSize(format); })) return nullptr;
    desc.inputElements=gathered_elements.data();
    for (u32 e=0;e<declaration.element_count;++e) {
      auto& format=gathered_elements[e].format;
      if (format==plume::RenderFormat::R8G8B8A8_UINT || format==plume::RenderFormat::R8G8B8A8_SINT)
        format=plume::RenderFormat::R32G32B32A32_FLOAT;
    }
  }

  desc.inputElementsCount = declaration.element_count;
  desc.inputSlots = slots.data();
  desc.inputSlotsCount = static_cast<u32>(slots.size());
  // Draw snapshots retain shader/declaration owners; device teardown joins
  // every preparation worker before releasing the device, layout or caches.
  // Variant resolution above remains serialized, but the expensive driver
  // compile need not exclude workers building an unrelated pipeline.
  auto* device = HostDevice::Device();
  lock.unlock();
  const auto creation_start = std::chrono::steady_clock::now();
  auto pipeline = device->createGraphicsPipeline(desc);
  ++g_draw_timing.temporal_pipeline_creations;
  g_draw_timing.temporal_pipeline_creation_ms += std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - creation_start).count();
    if (!pipeline || !static_cast<plume::D3D12GraphicsPipeline*>(pipeline.get())->d3d) {
      static std::atomic<u32> failed_pipeline_logs{0};
      if (failed_pipeline_logs.fetch_add(1) < 16)
        REXLOG_ERROR("Native GPU: D3D12 rejected pipeline VS=0x{:08X} PS=0x{:08X} decl=0x{:08X}",
                     key.vertex_shader, key.pixel_shader, key.declaration);
      return nullptr;
  }
  SetNativePipelineStencilReference(pipeline.get(), desc);
  auto* result = pipeline.get();
  static std::atomic<u32> mrt_logs{0};
  if (desc.renderTargetCount > 1 && mrt_logs.fetch_add(1, std::memory_order_relaxed) < 16)
    REXLOG_INFO("Native GPU: MRT pipeline targets={} formats={},{},{},{} masks={:04X} blends={:08X},{:08X},{:08X},{:08X}",
        desc.renderTargetCount, key.render_target_formats[0], key.render_target_formats[1],
        key.render_target_formats[2], key.render_target_formats[3], key.color_write_mask,
        key.blend_controls[0], key.blend_controls[1], key.blend_controls[2], key.blend_controls[3]);
  lock.lock();
  g_temporal_pipelines.emplace(key, std::move(pipeline));
  return result;
}

// Inspect only committed, readable guest spans. Diagnostic reads must not
// introduce a fault for a bad declaration/index or walk an unbounded buffer.
bool ReadDrawDiagnosticBytes(u32 address, void* output, u32 bytes) {
  if (!address || !output || !bytes || bytes > 64 * 12 ||
      u64(address) + bytes > 0x100000000ull) return false;
  auto* memory = REX_KERNEL_MEMORY();
  if (!memory) return false;
  const u64 end = u64(address) + bytes;
  for (u64 at = address; at < end;) {
    auto* heap = memory->LookupHeap(u32(at));
    if (!heap) return false;
    const u32 size = heap->page_size(), base = heap->heap_base();
    if (!size || (size & (size - 1)) || at < base) return false;
    const u32 page = base + ((u32(at) - base) & ~(size - 1));
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(page, &info) ||
        !(info.state & rex::memory::kMemoryAllocationCommit) ||
        !(info.protect & rex::memory::kMemoryProtectRead)) return false;
    const u64 region_end = std::min(u64(page) + info.region_size,
        u64(info.allocation_base) + info.allocation_size);
    if (region_end <= at) return false;
    at = std::min(end, region_end);
  }
  std::memcpy(output, memory->TranslateVirtual<const u8*>(address), bytes);
  return true;
}

std::optional<u32> ReadDrawDiagnosticDeviceWord(const D3DDevice* device, u32 offset) {
  auto* memory = REX_KERNEL_MEMORY();
  if (!device || !memory) return std::nullopt;
  const uintptr_t pointer = reinterpret_cast<uintptr_t>(device);
  const uintptr_t base = reinterpret_cast<uintptr_t>(memory->virtual_membase());
  if (pointer < base || pointer - base > UINT32_MAX) return std::nullopt;
  const u64 address = u64(pointer - base) + offset;
  be_u32 word{};
  if (address > UINT32_MAX || !ReadDrawDiagnosticBytes(u32(address), &word, sizeof(word)))
    return std::nullopt;
  return u32(word);
}

struct CpuDrawDiagnostic {
  DrawDiagnosticSession& session;
  std::ostringstream record;
  DrawBindings bindings;
  bool indexed = false, submitted = false;
  u32 start = 0, count = 0;
  i32 base_vertex = 0;

  CpuDrawDiagnostic(DrawDiagnosticSession& selected, u32 id,
      D3DDevice* device, u32 primitive, bool indexed_draw, u32 first,
      u32 vertices, i32 base)
      : session(selected), bindings(SnapshotDrawBindings()),
        indexed(indexed_draw), start(first), count(vertices), base_vertex(base) {
    record << "draw=" << id << " attempted_ordinal=" << session.attempted_ordinal()
        << " frame=" << g_probe_frame.load()
        << " primitive=" << primitive << " indexed=" << indexed
        << " start=" << start << " count=" << count << " base=" << base_vertex
        << std::hex << " VS=" << BoundShaderHash(ShaderStage::kVertex)
        << " PS=" << BoundShaderHash(ShaderStage::kPixel)
        << " IB=" << bindings.index_buffer << " RT=" << bindings.render_targets[0]
        << " DS=" << bindings.depth_stencil;
    if (!device) { record << " device=null\n"; return; }
    const auto guest_declaration = ReadDrawDiagnosticDeviceWord(device,
        offsetof(D3DDevice, vertex_declaration));
    if (!guest_declaration) { record << " device_fields=unreadable\n"; return; }
    bindings.vertex_declaration = *guest_declaration;
    record << " decl=" << bindings.vertex_declaration;
    constexpr std::pair<const char*, u32> fields[] = {{"raster", 10568}, {"depth", 10548},
        {"stencil", 10496}, {"alpha", 10556}, {"alpha_ref_bits", 10500},
        {"legacy_float10620_bits", 10620},
        {"blend", 10552}, {"color_mask", 12292}, {"clip_control", 10564},
        {"viewport_control", 10572}, {"viewport_x_bits", 13024}, {"viewport_y_bits", 13028},
        {"viewport_width_bits", 13032}, {"viewport_height_bits", 13036},
        {"viewport_min_z_bits", 13040}, {"viewport_max_z_bits", 13044},
        {"scissor_left", 13052}, {"scissor_top", 13056},
        {"scissor_right", 13060}, {"scissor_bottom", 13064}};
    for (const auto& [name, offset] : fields) {
      record << ' ' << name << '=';
      if (const auto word = ReadDrawDiagnosticDeviceWord(device, offset)) record << *word;
      else record << "unreadable";
    }
    record << '\n';
    // Raw guest constants used by the captured UI/world transforms and alpha.
    // Read individual words through the same protected guest reader; never
    // inspect write-combined host constant uploads. Fixed 96 VS + 64 PS
    // register budget includes the material transforms, wind c92..95 and
    // shader-local alpha cutoff c58 observed in the Oz foliage shaders.
    const auto constants = [&](const char* name, u32 bank, u32 index) {
      record << name << std::dec << index << "=" << std::hex;
      for (u32 component = 0; component < 4; ++component) {
        if (const auto word = ReadDrawDiagnosticDeviceWord(device,
            bank + index * 16 + component * 4))
          record << std::setfill('0') << std::setw(8) << *word;
        else record << "unreadable";
        record << ',';
      }
      record << '\n';
    };
    for (u32 index = 0; index < (session.geometry() ? 256u : 96u); ++index) constants("VS_C", 0x780, index);
    for (u32 index = 0; index < (session.geometry() ? 224u : 64u); ++index) constants("PS_C", 0x1780, index);
    if (session.geometry()) {
      for (u32 i = 0; i < bindings.render_targets.size(); ++i) {
        record << "attachment=" << std::dec << i << " guest=" << std::hex << bindings.render_targets[i];
        if (const auto word = ReadDrawDiagnosticDeviceWord(device, 12292 + 4*i))
          record << " color_mask=" << *word;
        record << '\n';
      }
    }
    // Keep actual per-draw bindings: change-only texture traces can omit a
    // slot that stayed unchanged across the selected material draw.
    for (u32 slot = 0; slot < bindings.textures.size(); ++slot) {
      if (!bindings.textures[slot]) continue;
      record << "texture_slot=" << std::dec << slot << " guest=" << std::hex
          << bindings.textures[slot] << " fetch=";
      std::array<u32,6> active_fetch{};
      bool fetch_readable=true;
      for (u32 word = 0; word < 6; ++word) {
        if (const auto value = ReadDrawDiagnosticDeviceWord(device, 0x480 + slot * 24 + word * 4)) {
          record << std::setfill('0') << std::setw(8) << *value;
          active_fetch[word]=*value;
        } else { record << "unreadable"; fetch_readable=false; }
        record << ',';
      }
      record << '\n';
      const auto device_pointer=reinterpret_cast<uintptr_t>(device);
      const auto memory_base=reinterpret_cast<uintptr_t>(REX_KERNEL_MEMORY()->virtual_membase());
      const auto* last_set=device_pointer>=memory_base && device_pointer-memory_base<=UINT32_MAX
          ? g_texture_fetch_history.Find(u32(device_pointer-memory_base),slot) : nullptr;
      if (last_set && fetch_readable) {
        const u32 resource_changes=TextureFetchHistory::ChangedWords(last_set->fetch,active_fetch,true);
        record << "texture_last_set_slot=" << std::dec << slot << " ordinal=" << last_set->ordinal
            << " caller=" << std::hex << last_set->caller << " guest=" << last_set->guest
            << " changed_words=" << TextureFetchHistory::ChangedWords(last_set->fetch,active_fetch,false)
            << " changed_resource_words=" << resource_changes;
        if (resource_changes || last_set->guest!=bindings.textures[slot]) {
          record << " fetch=";
          for (u32 value:last_set->fetch) record << std::setfill('0') << std::setw(8) << value << ',';
        }
        record << '\n';
      }
      // Compare the active fetch with the actual bound header. Do not repair
      // guest state here: a mismatch needs attribution to its writer first.
      std::array<be_u32, 6> texture_header{};
      record << "texture_header_slot=" << std::dec << slot << " fetch=" << std::hex;
      const u64 header_address = u64(bindings.textures[slot]) + 0x1C;
      if (header_address <= UINT32_MAX && ReadDrawDiagnosticBytes(
          u32(header_address), texture_header.data(), sizeof(texture_header))) {
        for (const auto& word : texture_header)
          record << std::setfill('0') << std::setw(8) << u32(word) << ',';
      } else record << "unreadable";
      record << '\n';
    }
    std::array<be_u32, 7> header{};
    if (!ReadDrawDiagnosticBytes(bindings.vertex_declaration, header.data(), sizeof(header))) {
      record << "raw_decl=unreadable\n";
      return;
    }
    const u32 elements = header[6];
    record << "raw_decl_header=" << LongProbeHex(header) << std::dec
        << " raw_count=" << elements << '\n';
    std::array<u8, 64 * 12> declaration{};
    const u64 address = u64(bindings.vertex_declaration) + 52;
    if (elements > 64 || address > UINT32_MAX ||
        (elements && !ReadDrawDiagnosticBytes(u32(address), declaration.data(), elements * 12))) {
      record << "raw_decl=invalid_or_unreadable\n";
      return;
    }
    record << "raw_decl_bytes=" << std::hex << std::setfill('0');
    for (u32 i = 0; i < elements * 12; ++i)
      record << std::setw(2) << u32(declaration[i]);
    record << '\n';
  }

  void Layout(const VertexDeclarationView& declaration) {
    record << "host_supported=" << declaration.supported
        << " swapped_position=" << declaration.swapped_positions
        << " swapped_normal=" << declaration.swapped_normals
        << " swapped_texcoord=" << declaration.swapped_texcoords
        << " sint_texcoord=" << declaration.sint_texcoords
        << " reversed_elements=" << declaration.reversed_byte_elements << '\n';
    std::array<i64, 4> vertices{start, i64(start) + 1, i64(start) + 2, i64(start) + 3};
    u32 vertex_count = std::min(count, 4u);
    if (indexed) {
      std::array<u8, sizeof(D3DBuffer)> header{};
      const auto index = ReadDrawDiagnosticBytes(bindings.index_buffer, header.data(), header.size())
          ? InspectBufferResource(bindings.index_buffer, BufferKind::kIndex) : BufferResourceView{};
      const u32 width = index.guest_format == 1 ? 2 : 4;
      if (session.geometry()) record << "index_blob=" << std::quoted(session.SaveGeometry(
          index.mirror_address, index.length, ReadDrawDiagnosticBytes)) << '\n';
      record << "index_source=" << index.mirror_address << std::dec
          << " length=" << index.length << " guest_format=" << index.guest_format
          << " first_indices=";
      vertex_count = 0;
      for (u32 i = 0; i < std::min(count, 6u); ++i) {
        const u64 offset = (u64(start) + i) * width;
        const u64 address = u64(index.mirror_address) + offset;
        std::array<be_u16, 2> raw{};
        if (!index.mirror_address || offset > index.length || width > index.length - offset ||
            address > UINT32_MAX || !ReadDrawDiagnosticBytes(u32(address), raw.data(), width)) {
          record << "unreadable,";
          continue;
        }
        const u32 value = width == 2 ? u32(raw[0]) :
            (u32(raw[0]) << 16) | u32(raw[1]);
        record << value << ',';
        // Preserve actual corner order, including the fourth quad corner.
        // Attribute reads retain bounded general, UV and tangent budgets.
        if (i < 4) vertices[vertex_count++] = i64(value) + base_vertex;
      }
      record << '\n' << std::hex;
    }
    std::array<BufferResourceView, kNativeVertexStreams> buffers{};
    std::array<bool, kNativeVertexStreams> inspected{};
    u32 samples_left = 12;
    u32 uv_samples_left = 12;
    u32 tangent_samples_left = 4;
    for (u32 i = 0; declaration.elements && i < std::min(declaration.element_count, 75u); ++i) {
      const auto& element = declaration.elements[i];
      record << "host_attr=" << element.semanticName << std::dec << element.semanticIndex
          << " slot=" << element.slotIndex << " offset=" << element.alignedByteOffset
          << " format=" << u32(element.format);
      if (element.slotIndex >= kNativeVertexStreams) { record << '\n'; continue; }
      const auto& binding = bindings.vertex_streams[element.slotIndex];
      record << " stride=" << binding.stride << " stream_offset=" << binding.offset
          << std::hex << " VB=" << binding.buffer << '\n';
      const bool texture_coordinate = std::strcmp(element.semanticName, "TEXCOORD") == 0;
      const bool tangent = std::strcmp(element.semanticName, "TANGENT") == 0;
      if (!binding.buffer || (!session.geometry() && !texture_coordinate && !tangent && std::strcmp(element.semanticName, "POSITION") &&
          std::strcmp(element.semanticName, "COLOR") &&
          std::strcmp(element.semanticName, "NORMAL"))) continue;
      if (!inspected[element.slotIndex]) {
        std::array<u8, sizeof(D3DBuffer)> header{};
        if (ReadDrawDiagnosticBytes(binding.buffer, header.data(), header.size()))
          buffers[element.slotIndex] = InspectBufferResource(binding.buffer, BufferKind::kVertex);
        inspected[element.slotIndex] = true;
        if (session.geometry()) {
          const auto& captured = buffers[element.slotIndex];
          record << "vertex_blob_slot=" << std::dec << element.slotIndex
              << " address=" << std::hex << captured.mirror_address
              << " bytes=" << std::dec << captured.length << " path="
              << std::quoted(session.SaveGeometry(captured.mirror_address,
                  captured.length, ReadDrawDiagnosticBytes)) << '\n';
        }
      }
      const auto& buffer = buffers[element.slotIndex];
      const u32 width = std::min(plume::RenderFormatSize(element.format), 16u);
      auto& remaining = texture_coordinate ? uv_samples_left :
          tangent ? tangent_samples_left : samples_left;
      for (u32 v = 0; v < vertex_count && remaining; ++v, --remaining) {
        const auto offset = DrawDiagnosticVertexOffset(vertices[v], binding.stride,
            binding.offset, element.alignedByteOffset, width, buffer.length);
        const u64 address = offset ? u64(buffer.mirror_address) + *offset : UINT64_MAX;
        std::array<u8, 16> sample{};
        record << "sample_attr=" << element.semanticName << std::dec << element.semanticIndex
            << " vertex=" << vertices[v] << std::hex << " source=" << address;
        if (!buffer.mirror_address || !offset || address > UINT32_MAX ||
            !ReadDrawDiagnosticBytes(u32(address), sample.data(), width)) {
          record << " bytes=unreadable\n";
          continue;
        }
        record << " bytes=" << std::setfill('0');
        for (u32 b = 0; b < width; ++b) record << std::setw(2) << u32(sample[b]);
        record << '\n';
      }
    }
  }

  ~CpuDrawDiagnostic() {
    record << "submitted=" << submitted << '\n';
    session.Write(record.str());
  }
};

std::unique_ptr<CpuDrawDiagnostic> BeginCpuDrawDiagnostic(D3DDevice* device,
    u32 primitive, bool indexed, u32 start, u32 count, i32 base_vertex) {
  static DrawDiagnosticSession session([] {
    char* value = nullptr; size_t length = 0;
    _dupenv_s(&value, &length, "LEGO_NATIVE_DRAW_TRACE_TRIGGER");
    std::filesystem::path result(value ? value : REXCVAR_GET(gpu_native_draw_trace_trigger).c_str());
    std::free(value);
    return result;
  }(), REXCVAR_GET(gpu_native_draw_trace_geometry));
  static const bool enabled = !session.output().empty();
  if (!enabled) return {};
  const u32 id = session.Begin(g_probe_frame.load(), BoundShaderHash(ShaderStage::kPixel));
  if (!id) return {};
  if (id == 1) REXLOG_INFO("Native CPU draw trace: capturing one frame, up to 64 draws, output={}",
                           session.output().string());
  return std::make_unique<CpuDrawDiagnostic>(session, id, device, primitive,
                                            indexed, start, count, base_vertex);
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
      read(10556), float(*reinterpret_cast<const be_f32*>(bytes + 10500)));
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
#if defined(LEGODIMENSIONS_TT_LOCK_TRACE)
  legodimensions::tt_lock_trace::NativeDrawScope native_draw_trace;
#endif
  DrawTimer timing;
  auto cpu_trace = BeginCpuDrawDiagnostic(device, primitive_type, indexed, start, count, base_vertex);
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
  if (!NativeViewportValid(device->viewport.x, device->viewport.y,
      device->viewport.width, device->viewport.height,
      device->viewport.min_z, device->viewport.max_z)) return false;
  if (float(device->viewport.width) == 0.0f ||
      float(device->viewport.height) == 0.0f) return true;
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
  RefreshBoundShaderResources();
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
  const bool polygonal = topology == plume::RenderPrimitiveTopology::TRIANGLE_LIST ||
                         topology == plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
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
  const auto declaration = RecordedMultiRateDraw() && RecordedMultiRateDraw()->multi_rate_input
      ? RecordedMultiRateDraw()->declaration : t_recording_draw ? t_recording_draw->declaration :
      ResolveVertexDeclaration(bindings.vertex_declaration);
  if (cpu_trace) cpu_trace->Layout(declaration);
  if(!RetryNativeCompositionTargets()) return false;
  std::optional<TextureResourceView> composition_input;
  if(NativeFsr3Enabled() && NativePrecompositionEnabled() && !TemporalSceneComposed(g_probe_frame.load()) &&
      declaration.supported && declaration.elements &&
      IsTemporalCompositionShader(BoundShaderHash(ShaderStage::kVertex),BoundShaderHash(ShaderStage::kPixel))) {
    bool vertex_failed=false,pixel_failed=false;
    const auto vertex_address=BoundShaderAddress(ShaderStage::kVertex,&vertex_failed);
    const auto pixel_address=BoundShaderAddress(ShaderStage::kPixel,&pixel_failed);
    if(vertex_failed || pixel_failed || !vertex_address || !pixel_address) return false;
    const auto* bytes=reinterpret_cast<const u8*>(device);
    const u32 control=*reinterpret_cast<const be_u32*>(bytes+10548);
    TemporalCompositionDraw candidate;
    candidate.vertex=BoundShaderHash(ShaderStage::kVertex);
    candidate.pixel=BoundShaderHash(ShaderStage::kPixel);
    candidate.source=NativeTextureExtent(bindings.textures[0]);
    candidate.target=NativeTextureExtent(bindings.render_targets[0]);
    candidate.depth=NativeTextureExtent(bindings.depth_stencil);
    candidate.count=count;candidate.start=start;candidate.indexed=indexed;
    candidate.vertex_textures=BoundShaderTextureMask(ShaderStage::kVertex);
    candidate.pixel_textures=BoundShaderTextureMask(ShaderStage::kPixel);
    candidate.color_mask=*reinterpret_cast<const be_u32*>(bytes+12292)&15u;
    candidate.extra_targets=std::any_of(bindings.render_targets.begin()+1,bindings.render_targets.end(),[](u32 address){return address!=0;});
    candidate.depth_test=DecodeDepthState(control,true).enabled;
    candidate.stencil_test=(control&1u)!=0;
    candidate.blend=DecodeColorBlend(*reinterpret_cast<const be_u32*>(bytes+10552)).enabled;
    candidate.alpha_test=DecodeNativeAlphaState(*reinterpret_cast<const be_u32*>(bytes+10556),
        *reinterpret_cast<const be_f32*>(bytes+10500)).enabled;
    candidate.full_viewport=float(device->viewport.x)==0 && float(device->viewport.y)==0 &&
        float(device->viewport.width)==1280 && float(device->viewport.height)==720;
    const auto scissor=DecodeNativeScissor(float(device->viewport.x),float(device->viewport.y),
        float(device->viewport.width),float(device->viewport.height),
        {i32(device->scissor.left),i32(device->scissor.top),i32(device->scissor.right),i32(device->scissor.bottom)},
        u32(*reinterpret_cast<const be_u32*>(bytes+kNativeScissorEnableOffset))!=0,1280,720);
    candidate.full_scissor=scissor==NativeScissorRect{0,0,1280,720};
    candidate.same_resource=bindings.textures[0]==bindings.render_targets[0];
    if(topology==plume::RenderPrimitiveTopology::TRIANGLE_LIST && CanComposeTemporalScene(candidate)) {
      const auto frame=g_probe_frame.load();
      auto* precompose_commands=HostDevice::BeginFrameCommands();
      TextureResourceView source;
      if(precompose_commands && PrepareSurfaceDepthAlias(bindings.depth_stencil) &&
          PrepareTextureResource(bindings.textures[0],precompose_commands,false,source)==TexturePreparationResult::kReady &&
          source.width==candidate.source.width && source.height==candidate.source.height &&
          NativeTextureExtent(bindings.textures[0]).logical_width==1280 &&
          NativeTextureExtent(bindings.textures[0]).logical_height==720) {
        auto reconstructed=PresentTemporalFrame(source,g_temporal_scene_depth,frame);
        if(reconstructed.texture && reconstructed.texture!=source.texture &&
            BeginNativeCompositionTargets(bindings.render_targets[0],bindings.depth_stencil)) {
          if(CompleteTemporalComposition(frame)) {
            composition_input=reconstructed;composition_input->has_sampling_fetch=false;
            composition_input->content_hash_valid=false;
          } else EndNativeCompositionTargets(false);
        }
      }
      if(!composition_input) FailTemporalComposition(frame);
    } else {
      static u32 rejected_composition_logs=0;
      if(rejected_composition_logs++<8)
        REXLOG_INFO("Native FSR composition candidate rejected: count={} start={} indexed={} textures={:X}/{:X} write={:X} depth={} stencil={} blend={} alpha={} full_viewport={} full_scissor={} source={}x{} target={}x{} ds={}x{}",
            count,start,indexed,candidate.vertex_textures,candidate.pixel_textures,candidate.color_mask,
            candidate.depth_test,candidate.stencil_test,candidate.blend,candidate.alpha_test,
            candidate.full_viewport,candidate.full_scissor,candidate.source.width,candidate.source.height,
            candidate.target.width,candidate.target.height,candidate.depth.width,candidate.depth.height);
    }
  }
  if(TemporalSceneComposed(g_probe_frame.load())) {
    const auto promote=[&](u32 address,bool depth_target) {
      const auto extent=NativeTextureExtent(address);
      if(extent.logical_width!=1280 || extent.logical_height!=720) return true;
      if(extent.width==1920 && extent.height==1080) return true;
      return depth_target?BeginNativeCompositionTargets(0,address):BeginNativeCompositionTargets(address,0);
    };
    for(u32 target:bindings.render_targets) if(target && !promote(target,false)) return false;
    if(bindings.depth_stencil && !promote(bindings.depth_stencil,true)) return false;
  }
  struct CompositionDrawGuard {
    bool* submitted;bool started;u64 frame;
    ~CompositionDrawGuard() {
      if(started && !*submitted) {
        FailTemporalComposition(frame);EndNativeCompositionTargets(false);
      }
    }
  } composition_guard{&mesh_result.submitted,composition_input.has_value(),g_probe_frame.load()};
  if (auto* prepared = CurrentDrawPreparation(); prepared && prepared->alias_decided) {
    // Full preparation enqueued its initialization before draw validation.
  } else if (!ApplySurfaceColorAliases(DecideSurfaceColorAliases(bindings.render_targets))) return false;
  std::array<TextureResourceView, kNativeRenderTargets> colors;
  for (u32 i = 0; i < colors.size(); ++i)
    colors[i] = ResolveTextureResource(bindings.render_targets[i]);
  const auto& color = colors[0];
  if (auto* prepared = CurrentDrawPreparation(); prepared && prepared->alias_decided) {
    if (!ApplySurfaceDepthAlias(prepared->alias)) return false;
  } else if (!PrepareSurfaceDepthAlias(bindings.depth_stencil)) return false;
  const auto depth = ResolveTextureResource(bindings.depth_stencil);
  // Shadow maps and depth prepasses may have no color attachment. Raster
  // bounds must come from the depth target in that case, not an empty RT0.
  const auto& raster_target = color.texture ? color : depth;
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
  BufferResourceView index_metadata;
  IndexDrawWindow index_window;
  if (indexed) {
    index_metadata = RecordedMultiRateDraw() && RecordedMultiRateDraw()->multi_rate_input
        ? RecordedMultiRateDraw()->multi_rate_input->index_metadata :
        t_recording_draw && t_recording_draw->geometry_ready ? t_recording_draw->index_input.metadata :
        InspectBufferResource(bindings.index_buffer, BufferKind::kIndex);
    index_window = DrawIndexWindow(start, count,
        index_metadata.guest_format == 1 ? 2 : 4, index_metadata.length);
    // An invalid window must reject the draw, rather than disabling range
    // uploads and falling through to an unchecked whole-buffer submission.
    if (!index_metadata.mirror_address || !index_window) return false;
  }
  timing.Next();
  auto* commands = CurrentDrawPreparation() ? CurrentDrawPreparation()->view.commands : HostDevice::BeginFrameCommands();
  if (!commands) {
    return false;
  }
  const auto draw_device = CurrentDrawPreparation() ? CurrentDrawPreparation()->view : HostDevice::CurrentDrawDeviceView();
  if (draw_device.commands != commands || !draw_device.recording_serial ||
      !draw_device.pipeline_layout || !draw_device.texture_descriptors ||
      !draw_device.sampler_descriptors || !draw_device.null_vertex_buffer) return false;
  StateCallLedger* state_calls = nullptr;
  if (auto* capture = NativeStateCalls())
    state_calls = capture->BeginDraw(commands, draw_device.recording_serial);
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
  bool vertex_binding_failed = false;
  key.vertex_shader = BoundShaderAddress(ShaderStage::kVertex, &vertex_binding_failed);
  bool pixel_binding_failed = false;
  key.pixel_shader = BoundShaderAddress(ShaderStage::kPixel, &pixel_binding_failed);
  // Failed nonnull binds must not reuse a cached PSO or turn a requested
  // pixel shader into a depth-only pass. Check before shader resolution.
  if (vertex_binding_failed) {
    LongProbeEvent("draw_missing_vertex_shader", true, "guest_vs=", key.vertex_shader);
    return false;
  }
  if (pixel_binding_failed) {
    LongProbeEvent("draw_missing_pixel_shader", true, "guest_ps=", key.pixel_shader);
    return false;
  }
  key.vertex_shader_hash = BoundShaderHash(ShaderStage::kVertex);
  key.pixel_shader_hash = BoundShaderHash(ShaderStage::kPixel);
  key.declaration_hash = declaration.content_hash;
  key.declaration = bindings.vertex_declaration;
  const auto* state_bytes = reinterpret_cast<const u8*>(device);
  const u32 guest_raster = *reinterpret_cast<const be_u32*>(state_bytes + 10568);
  static const bool cull_candidate = std::getenv("LEGO_NATIVE_NO_CULL") == nullptr;
  if (cull_candidate)
    key.raster_control = kNativeCullCandidate | (guest_raster & (polygonal ? 7u : 4u));
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
  // Stencil is ordinary guest render state, including shadow-volume masks.
  // Keep a restart-only depth-only path for controlled regression comparisons.
  static const bool enable_stencil = std::getenv("LEGO_NATIVE_NO_STENCIL") == nullptr;
  if (enable_stencil) {
    key.depth_control = *reinterpret_cast<const be_u32*>(
        reinterpret_cast<const u8*>(device) + 10548);
    if ((key.depth_control & 1u) && plume::RenderFormatIsStencil(depth.format)) {
      // Select back masks when the candidate culls only front faces; with
      // both faces surviving, per-face mask differences remain unsupported.
      const u32 word_offset = NativeStencilWordOffset(key.raster_control,
          key.depth_control, polygonal);
      key.stencil_masks = *reinterpret_cast<const be_u32*>(
          state_bytes + word_offset) & 0x00FFFFFFu;
      key.stencil_face = (guest_raster >> 2) & 1u;
      static thread_local std::unordered_set<u64> logged_stencil;
      const u64 identity = (u64(key.depth_control) << 32) | key.stencil_masks;
      if (logged_stencil.size() < 32 && logged_stencil.insert(identity).second)
        REXLOG_INFO("Native GPU: stencil control={:08X} masks/ref={:06X} ref={} VS={:016X} PS={:016X}",
            key.depth_control, key.stencil_masks, state_bytes[word_offset + 3],
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
  const auto alpha = DecodeNativeAlphaState(
      *reinterpret_cast<const be_u32*>(state_bytes + 10556),
      *reinterpret_cast<const be_f32*>(state_bytes + 10500));
  if (alpha.enabled) key.spec_constants |= 2u;
  // Polygon offset. TU23 D3DRS_SLOPESCALEDEPTHBIAS (83FB88A8) stores the
  // scale, already in 1/16 subpixel units, at +10832 (front) / +10840 (back);
  // D3DRS_DEPTHBIAS (83FB8970) stores the offset at +10836 / +10844; both set
  // PA_SU_SC_MODE_CNTL (+10568) poly_offset_front/back_enable (bits 11/12).
  // Convert the stored offset for the host rasterizer. Face selection follows
  // SDK's GetPreferredFacePolygonOffset/GetD3D10IntegerPolygonOffset: ignore
  // culled faces in the candidate; non-triangles use para_enable.
  // Unorm24 and float24 guests both come to ~offset * 2^24 host D32 units.
  // LEGO_NATIVE_NO_DEPTH_BIAS=1 restores the old behaviour for A/B checks.
  static const bool depth_bias_disabled = std::getenv("LEGO_NATIVE_NO_DEPTH_BIAS") != nullptr;
  if (!depth_bias_disabled && depth.format != plume::RenderFormat::UNKNOWN) {
    const u32 mode = CullPolygonOffsetMode(guest_raster, key.raster_control, polygonal);
    const auto poly = [&](u32 offset) {
      return float(*reinterpret_cast<const be_f32*>(state_bytes + offset));
    };
    const auto bias = DecodePolygonOffset(mode, polygonal,
        poly(10832), poly(10836), poly(10840), poly(10844));
    key.depth_bias = bias.depth_bias;
    key.slope_scaled_depth_bias = bias.slope_bits;
  }
  for (u32 i = 0; i < kNativeVertexStreams; ++i) {
    key.strides[i] = bindings.vertex_streams[i].stride;
  }
  GatheredMultiRateDraw multi_rate_draw;
  key.multi_rate_fetch=MultiRateFetchShader(key.vertex_shader_hash) &&
      (topology==plume::RenderPrimitiveTopology::TRIANGLE_LIST ||
       topology==plume::RenderPrimitiveTopology::LINE_LIST ||
       topology==plume::RenderPrimitiveTopology::POINT_LIST);
  if (key.multi_rate_fetch) {
    // Lists preserve their order directly; QuadList expands to two triangles.
    // Strips retain the existing path pending their restart-state proof.
    const bool quads=primitive_type==static_cast<u32>(rex::graphics::xenos::PrimitiveType::kQuadList);
    const auto* captured_draw=RecordedMultiRateDraw();
    if (captured_draw) {
      if (!captured_draw->multi_rate_input ||
          !MatchesNativeMultiRateInput(*captured_draw->multi_rate_input,device,declaration,
              indexed,start,count,base_vertex,quads) ||
          !UploadNativeMultiRateInput(*captured_draw->multi_rate_input,draw_device.null_vertex_buffer,
              multi_rate_draw,captured_draw->multi_rate_upload,captured_draw->multi_rate_upload_capacity)) return false;
    } else {
      if (CurrentDrawPreparation()) return false; // missing coordinator capture
      if (!PrepareNativeMultiRateGather(device,bindings,declaration,index_metadata,
              indexed,start,count,base_vertex,quads,draw_device.null_vertex_buffer,multi_rate_draw)) return false;
    }
    key.strides.fill(16);
  }
  InstancedFetchPlan instanced_fetch;
  if (InstancedFetchShader(key.vertex_shader_hash) &&
      topology == plume::RenderPrimitiveTopology::TRIANGLE_LIST &&
      primitive_type != static_cast<u32>(rex::graphics::xenos::PrimitiveType::kQuadList)) {
    u32 first = start;
    bool sequential = !indexed;
    if (indexed) {
      const auto* index_data = REX_KERNEL_MEMORY()->TranslateVirtual<const u8*>(
          index_metadata.mirror_address + index_window.offset);
      sequential = SequentialGuestIndices(index_data, count,
          index_metadata.guest_format == 1 ? 2 : 4, first) &&
          i64(first) + base_vertex >= 0;
      first = u32(i64(first) + base_vertex);
    }
    const float vertices_per_instance = float(*reinterpret_cast<const be_f32*>(
        reinterpret_cast<const u8*>(device) + 0x780 + kInstancedFetchCountRegister * 16));
    if (sequential)
      instanced_fetch = PlanInstancedFetch(vertices_per_instance, first, count,
          std::span(declaration.elements, declaration.element_count));
    if (!instanced_fetch) {
      // Any other index order: de-index and gather the streams on the CPU.
      thread_local std::vector<u32> expand_vertices;
      expand_vertices.resize(count);
      bool valid = true;
      const u32 element_size = index_metadata.guest_format == 1 ? 2 : 4;
      const auto* index_data = indexed ? REX_KERNEL_MEMORY()->TranslateVirtual<const u8*>(
          index_metadata.mirror_address + index_window.offset) : nullptr;
      for (u32 j = 0; j < count && valid; ++j) {
        i64 vertex = i64(start) + j;
        if (indexed) {
          const u8* p = index_data + size_t(j) * element_size;
          vertex = (element_size == 2 ? i64(u32(p[0]) << 8 | p[1])
                                      : i64(u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3])) +
              base_vertex;
        }
        valid = vertex >= 0 && vertex <= i64(UINT32_MAX);
        expand_vertices[j] = u32(vertex);
      }
      if (valid)
        instanced_fetch = PlanInstancedFetchExpand(vertices_per_instance, expand_vertices.data(),
            count, std::span(declaration.elements, declaration.element_count));
    }
    static std::atomic<u32> instanced_logs{0};
    if (instanced_logs.fetch_add(1) < 16)
      REXLOG_INFO("Native GPU: instanced fetch VS={:016X} N={} first={} count={} sequential={} -> {}",
          key.vertex_shader_hash, vertices_per_instance, first, count, sequential,
          !instanced_fetch ? "unchanged" : instanced_fetch.expand ? "expanded" : "instanced");
    key.instance_slots = instanced_fetch.expand ? 0 : instanced_fetch.instance_slots;
  }
  if (t_recording_draw) t_recording_draw->pipeline_recorded = false;
  const auto prepare_pipeline = [&] { return GetPipeline(key, declaration, topology, depth.format); };
  auto* pipeline = RetainedDrawPreparationEnabled()
      ? g_prepared_draw.pipeline.Resolve(key, prepare_pipeline) : prepare_pipeline();
  if (!pipeline) {
    g_prepared_draw.pipeline.Reset();
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

  {
    ConstantTimer constant_timing(ConstantCost::kBindings);
    BindNativeDrawLayout(commands, draw_device, state_calls);
    BindNativeDrawDescriptorSets(commands, draw_device, state_calls);
  }
  const auto raster_extent=NativeTextureExtent(color.texture ? bindings.render_targets[0] : bindings.depth_stencil);
  const float raster_scale_x=raster_extent.ScaleX(), raster_scale_y=raster_extent.ScaleY();
  const float viewport_width = device->viewport.width;
  const float viewport_height = device->viewport.height;
  const bool screen_space = viewport_candidate && u32(*reinterpret_cast<const be_u32*>(
      reinterpret_cast<const u8*>(device) + 10572)) == 0x400;
  std::array<u8, 0x2000 + 624> temporal_snapshot;
  std::optional<TemporalTextureHistory> temporal_texture_snapshot;
  const bool capture_temporal_constants = TemporalInputsCandidate() &&
      !TemporalSceneComposed(g_probe_frame.load()) &&
      (HasVerifiedTemporalCamera(key.vertex_shader_hash) ||
       HasTemporalCameraConstants(key.vertex_shader_hash,key.spec_constants));
  if (capture_temporal_constants) temporal_texture_snapshot.emplace();
  auto constants = BindConstants(commands, device, bindings, declaration, count<=6,
      draw_device.recording_serial, key.vertex_shader_hash, key.pixel_shader_hash,
      capture_temporal_constants ? &temporal_snapshot : nullptr,
      capture_temporal_constants ? &*temporal_texture_snapshot : nullptr,
      composition_input ? &*composition_input : nullptr,
      TemporalSceneComposed(g_probe_frame.load()));
  if (!constants) {
    return false;
  }
  const bool temporal_scene = NativeFsr3Enabled() && !TemporalSceneComposed(g_probe_frame.load()) && depth.texture &&
      DecodeDepthState(key.depth_control,true).enabled && !screen_space &&
      raster_extent.logical_width == 1280 && raster_extent.logical_height == 720 &&
      viewport_width == 1280 && viewport_height == 720 &&
      float(device->viewport.x) == 0 && float(device->viewport.y) == 0;
  float temporal_jitter_x = 0, temporal_jitter_y = 0;
  if (temporal_scene) {
    std::optional<TemporalCamera> camera;
    // Matrix use alone does not identify the main view: the menu also renders
    // a separate 3D UI camera (near=.2, far=500). Only profiles authorized to
    // supply the independent scene-depth snapshot may configure its camera.
    // Other geometry retains jitter and reactive coverage, not camera ownership.
    if (HasVerifiedTemporalCamera(key.vertex_shader_hash) ||
        HasTemporalCameraConstants(key.vertex_shader_hash,key.spec_constants)) {
      std::array<float,16> matrix;
      std::memcpy(matrix.data(),temporal_snapshot.data(),sizeof(matrix));
      camera = DecodeTemporalCamera(matrix);
      if (camera && HasVerifiedTemporalCamera(key.vertex_shader_hash))
        g_temporal_camera_authority.Observe(g_probe_frame.load(),matrix);
      static const bool trace_camera = std::getenv("LEGO_NATIVE_CAMERA_TRACE") != nullptr;
      if (camera && trace_camera) {
        static std::set<std::tuple<uint64_t,u32,u32,int>> seen;
        const auto identity = std::make_tuple(key.vertex_shader_hash,
            bindings.render_targets[0],bindings.depth_stencil,
            int(std::lround(camera->near_plane*100)));
        if (seen.size() < 96 && seen.insert(identity).second)
          REXLOG_INFO("Native camera pass frame={} vs={:016X} ps={:016X} rt={:08X} ds={:08X} near={} far={} fov={} verified={}",
              g_probe_frame.load(),key.vertex_shader_hash,key.pixel_shader_hash,
              bindings.render_targets[0],bindings.depth_stencil,camera->near_plane,
              camera->far_plane,camera->vertical_fov,HasVerifiedTemporalCamera(key.vertex_shader_hash));
      }
    }
    PrepareTemporalDraw(commands,g_probe_frame.load(),raster_extent.width,raster_extent.height,
        HasVerifiedTemporalCamera(key.vertex_shader_hash) ? camera : std::nullopt,
        temporal_jitter_x,temporal_jitter_y);
  }
  // A raw depth texture view may need a conversion draw while binding textures.
  // Restore the guest framebuffer after that helper's temporary target.
  {
    ConstantTimer constant_timing(ConstantCost::kRestore);
    commands->barriers(plume::RenderBarrierStage::GRAPHICS, texture_barriers.data(), barrier_count);
    commands->setFramebuffer(framebuffer);
  }
  timing.Next();
  BindNativeDrawPipeline(commands, pipeline, state_calls);
  // 83FB82F0 writes normalized R,G,B,A blend constants at +10464..10476.
  const auto* guest_blend_factor = reinterpret_cast<const be_f32*>(
      reinterpret_cast<const u8*>(device) + 10464);
  const float blend_factor[] = {guest_blend_factor[0], guest_blend_factor[1],
                               guest_blend_factor[2], guest_blend_factor[3]};
  const std::array<float, 4> recorded_blend{blend_factor[0], blend_factor[1], blend_factor[2], blend_factor[3]};
  SetNativeBlendFactor(commands, recorded_blend);

  commands->setViewports(plume::RenderViewport(
      screen_space ? 0.0f : float(device->viewport.x)*raster_scale_x + temporal_jitter_x,
      screen_space ? 0.0f : float(device->viewport.y)*raster_scale_y + temporal_jitter_y,
      screen_space ? float(raster_target.width) : viewport_width*raster_scale_x,
      screen_space ? float(raster_target.height) : viewport_height*raster_scale_y,
      screen_space ? 0.0f : static_cast<float>(device->viewport.min_z),
      screen_space ? 1.0f : static_cast<float>(device->viewport.max_z)));
  auto effective_scissor = DecodeNativeScissor(
      float(device->viewport.x), float(device->viewport.y),
      float(device->viewport.width), float(device->viewport.height),
      {i32(device->scissor.left), i32(device->scissor.top),
       i32(device->scissor.right), i32(device->scissor.bottom)},
      u32(*reinterpret_cast<const be_u32*>(state_bytes +
          kNativeScissorEnableOffset)) != 0, raster_extent.logical_width, raster_extent.logical_height);
  effective_scissor = {
      NativeRenderEdge(effective_scissor.left,raster_extent.logical_width,raster_extent.width),
      NativeRenderEdge(effective_scissor.top,raster_extent.logical_height,raster_extent.height),
      NativeRenderEdge(effective_scissor.right,raster_extent.logical_width,raster_extent.width),
      NativeRenderEdge(effective_scissor.bottom,raster_extent.logical_height,raster_extent.height)};
  const i32 scissor_right = effective_scissor.right;
  const i32 scissor_bottom = effective_scissor.bottom;
  commands->setScissors(CullDrawScissor(plume::RenderRect(
      effective_scissor.left, effective_scissor.top,
      scissor_right, scissor_bottom),
      key.raster_control, polygonal));
  std::array<plume::RenderVertexBufferView, kNativeVertexStreams> views;
  std::array<plume::RenderInputSlot, kNativeVertexStreams> slots;
  if (!color.texture) {
    static thread_local u32 depth_only_logs = 0;
    if (depth_only_logs++ < 16)
      REXLOG_INFO("Native GPU: depth-only draw DS={:08X} host={}x{} scissor={},{},{},{} VS={:016X} PS={:016X}",
          bindings.depth_stencil, raster_target.width, raster_target.height,
          effective_scissor.left, effective_scissor.top, scissor_right, scissor_bottom,
          key.vertex_shader_hash, key.pixel_shader_hash);
  }
  auto* null_buffer = draw_device.null_vertex_buffer;
  std::array<BufferResourceView, kNativeVertexStreams> vertex_buffers;
  static const bool window_uploads = [] {
    const char* value = std::getenv("LEGO_NATIVE_BUFFER_WINDOWS");
    const bool enabled = value ? std::strcmp(value, "0") != 0 : REXCVAR_GET(gpu_native_buffer_windows);
    REXLOG_INFO("Native buffer range cache: {} ({})", enabled,
        value ? "environment override" : "saved configuration");
    return enabled;
  }();
  std::array<VertexBufferWindow, kNativeVertexStreams> vertex_windows{};
  const auto prepare_layout = [&] {
    return PrepareVertexLayout(declaration, [](auto format) { return plume::RenderFormatSize(format); }, kNativeVertexStreams);
  };
  VertexLayoutPlan legacy_layout;
  const auto& layout = t_recording_draw && t_recording_draw->geometry_ready ? *t_recording_draw->layout :
      RetainedDrawPreparationEnabled() && declaration.content_hash
      ? g_prepared_draw.vertex_layout.Resolve({declaration.content_hash, declaration.element_count,
          declaration.reversed_byte_elements, declaration.half_swapped_elements,
          declaration.half_swapped_wide_elements}, prepare_layout)
      : (legacy_layout = prepare_layout());
  if (!layout.valid) return false;
  std::array<bool, kNativeVertexStreams> active_vertex_streams{};
  for (u32 i = 0; i < kNativeVertexStreams; ++i) active_vertex_streams[i] = layout.streams[i].active;
  // Range validation and window planning share one metadata snapshot per
  // stream within this draw. Every following draw inspects again, and the
  // actual upload still revalidates its resource/header generation.
  std::array<BufferResourceView, kNativeVertexStreams> vertex_metadata{};
  std::array<bool, kNativeVertexStreams> vertex_metadata_inspected{};
  const auto inspect_vertex_stream = [&](u32 i) -> const BufferResourceView& {
    if (!vertex_metadata_inspected[i]) {
      vertex_metadata[i] = InspectBufferResource(bindings.vertex_streams[i].buffer,BufferKind::kVertex);
      vertex_metadata_inspected[i] = true;
    }
    return vertex_metadata[i];
  };
  bool use_windows = false;
  u32 min_index = 0;
  const u32 index_offset = index_window.offset, index_bytes = index_window.length;
  if (key.multi_rate_fetch) {
    // Every actual per-attribute source address was checked during gathering.
    // Keep the ordinary whole-index guard unchanged for every other program.
    use_windows=false;
  } else if (t_recording_draw && t_recording_draw->geometry_ready) {
    if (!t_recording_draw->geometry_valid) return false;
    use_windows = t_recording_draw->use_windows;
    min_index = t_recording_draw->min_index;
    active_vertex_streams = t_recording_draw->active_streams;
    vertex_windows = t_recording_draw->windows;
  } else if (count) {
    u32 max_index = 0;
    if (indexed) {
      const auto& index = index_metadata;
      const u32 element_size = index.guest_format == 1 ? 2 : 4;
      const auto* source = REX_KERNEL_MEMORY()->TranslateVirtual<const u8*>(index.mirror_address + index_offset);
      const auto bounds = ScanGuestIndexBounds({source, index_bytes}, element_size);
      min_index = bounds.min;
      max_index = bounds.max;
      // D3D12's signed base vertex must represent -min_index exactly.
      use_windows = window_uploads && min_index <= INT32_MAX;
    } else {
      min_index = start;
      if (!NonIndexedVertexRange(start, count, max_index)) return false;
      use_windows = window_uploads;
    }
    auto shader = decltype(FindDxil(key.vertex_shader_hash,key.spec_constants)){};
    bool shader_looked_up=false;
    static const bool input_usage_cache_enabled=[] {
      const char* value=std::getenv("LEGO_NATIVE_INPUT_SIGNATURE_CACHE");
      return !value || !*value || value[0]!='0';
    }();
    const auto required_inputs=g_vertex_input_usage.Get(
        {key.vertex_shader_hash,input_usage_cache_enabled?declaration.content_hash:0,
         key.spec_constants,declaration.element_count},
        [&]() -> std::optional<u64> {
          if(declaration.element_count>64)return {};
          shader=FindDxil(key.vertex_shader_hash,key.spec_constants);shader_looked_up=true;
          if(!shader.data || !shader.size)return {};
          u64 used=0;
          for(u32 e=0;e<declaration.element_count;++e) {
            const auto& element=declaration.elements[e];
            const auto reads=ShaderReadsVertexInput({shader.data,shader.size},
                element.semanticName,element.semanticIndex);
            if(!reads || *reads)used|=u64{1}<<e;
          }
          return used;
        });
    if(!required_inputs && !shader_looked_up)
      shader=FindDxil(key.vertex_shader_hash,key.spec_constants);
    // A declaration may retain a secondary stream that this specialization
    // never reads (captured position-only draws retain normal/UV streams).
    // Use the already cached signature, with no retained buffer metadata.
    // Temporal replays and alternate VS banks can have different inputs;
    // keep their existing declaration-based upload path conservative.
    static const bool skip_unused_streams=[] {
      const char* value=std::getenv("LEGO_NATIVE_SKIP_UNUSED_VERTEX_STREAMS");
      return !value || std::strcmp(value,"0")!=0;
    }();
    if (skip_unused_streams && required_inputs && !NativeFsr3Enabled()) {
      std::array<bool, kNativeVertexStreams> shader_streams{};
      for (u32 e=0; e<declaration.element_count; ++e)
        if (*required_inputs & (u64{1}<<e))
          shader_streams[declaration.elements[e].slotIndex]=true;
      if (shader_streams!=active_vertex_streams &&
          !(FloatBlendIndicesInput({declaration.elements,declaration.element_count}) &&
            FindFloatBlendIndicesDxil(key.vertex_shader_hash,key.spec_constants)) &&
          !FindPositionPrecisionDxil(key.vertex_shader_hash,key.spec_constants))
        active_vertex_streams=shader_streams;
    }
    // A failed window optimization must not authorize an out-of-range fetch
    // from the whole buffer. LOTR's captured bad draw reached vertex 254 in
    // a secondary stream with only 102 records, and wrote giant black triangles.
    for (u32 e=0; e<declaration.element_count; ++e) {
      const auto& element=declaration.elements[e];
      if(required_inputs) {
        if(!(*required_inputs & (u64{1}<<e)))continue;
      } else {
        const auto reads=ShaderReadsVertexInput({shader.data,shader.size},
            element.semanticName,element.semanticIndex);
        if(reads && !*reads)continue;
      }
      const auto& binding=bindings.vertex_streams[element.slotIndex];
      if (!binding.buffer) continue; // Deliberate synthetic zero attributes.
      const auto& metadata=inspect_vertex_stream(element.slotIndex);
      u32 fetch_min=min_index, fetch_max=max_index;
      i32 fetch_base=indexed?base_vertex:0;
      // Gathered streams check every record themselves.
      if (instanced_fetch.expand && (((instanced_fetch.template_slots |
          instanced_fetch.instance_slots) >> element.slotIndex) & 1)) continue;
      if (instanced_fetch) {
        // Template records 0..N-1; per-instance records follow the instance id.
        const bool per_instance=(instanced_fetch.instance_slots>>element.slotIndex)&1;
        fetch_min=per_instance?instanced_fetch.first_instance:0;
        fetch_max=per_instance?instanced_fetch.last_instance():instanced_fetch.vertices-1;
        fetch_base=0;
      }
      if (!VertexFetchRangeFits(fetch_min,fetch_max,fetch_base,
          binding.stride?binding.stride:16,binding.offset,element.alignedByteOffset,
          plume::RenderFormatSize(element.format),metadata.length)) {
        static thread_local u32 invalid_fetch_logs=0;
        if (invalid_fetch_logs++<32) REXLOG_WARN(
            "Native GPU: rejected vertex fetch VS={:016X} PS={:016X} stream={} "
            "buffer={:08X} bytes={} stride={} offset={} range={}..{} base={}",
            key.vertex_shader_hash,key.pixel_shader_hash,element.slotIndex,
            binding.buffer,metadata.length,binding.stride,binding.offset,
            min_index,max_index,base_vertex);
        return false;
      }
    }
    // Instanced draws bind whole streams; windows follow the plain index range.
    if (instanced_fetch) use_windows = false;
    for (u32 i = 0; use_windows && i < kNativeVertexStreams; ++i) {
      const auto& binding = bindings.vertex_streams[i];
      if (!binding.buffer) continue;  // Synthetic/optional zero stream.
      if (!active_vertex_streams[i]) continue;
      if (!layout.streams[i].widths_valid || layout.streams[i].minimum_stride > binding.stride) use_windows = false;
      if (!use_windows) break;
      const auto& metadata = inspect_vertex_stream(i);
      vertex_windows[i] = DrawVertexWindow(min_index, max_index, indexed ? base_vertex : 0,
          binding.stride, binding.offset, metadata.length);
      if (!vertex_windows[i].length) use_windows = false;
    }
  }
  if (key.multi_rate_fetch) {
    views=multi_rate_draw.views;slots=multi_rate_draw.slots;
  } else for (u32 i = 0; i < kNativeVertexStreams; ++i) {
    const auto& binding = bindings.vertex_streams[i];
    // D3D keeps old stream bindings across declaration changes. A stream with
    // no input element cannot be fetched by this draw. Do not scan/upload a
    // previously bound world mesh for a fullscreen quad using another stream.
    slots[i] = plume::RenderInputSlot(i, binding.stride ? binding.stride : 16);
    if (!active_vertex_streams[i]) {
      views[i] = plume::RenderVertexBufferView(null_buffer->at(0), 256);
      continue;
    }
    // Conversion fields belong to the prepared declaration.
    const auto reversed_elements = layout.streams[i].Reversal();
    if (instanced_fetch.expand && binding.buffer &&
        (((instanced_fetch.template_slots | instanced_fetch.instance_slots) >> i) & 1)) {
      const auto& metadata = inspect_vertex_stream(i);
      const auto expanded = ExpandInstancedFetchStream(metadata.mirror_address, metadata.length,
          binding.stride, binding.offset, instanced_fetch, (instanced_fetch.instance_slots >> i) & 1,
          reversed_elements);
      if (!expanded.buffer) return false;
      views[i] = plume::RenderVertexBufferView(expanded.buffer->at(expanded.offset), expanded.length);
      continue;
    }
    const auto window = vertex_windows[i];
    const auto buffer = t_recording_draw && t_recording_draw->geometry_ready && t_recording_draw->vertices[i].view.buffer ?
        t_recording_draw->vertices[i].view : use_windows && window.length ?
        ResolveBufferStreamWindow(i, binding.buffer,
            window.offset, window.length, {binding.stride, 0, reversed_elements}) :
        ResolveBufferResourceView(binding.buffer,
            BufferKind::kVertex, {binding.stride, binding.offset, reversed_elements});
    // Only an explicitly null stream may synthesize zero attributes. A
    // failed upload of a bound active stream must fail the original draw.
    if (binding.buffer && !buffer.buffer) return false;
    vertex_buffers[i] = buffer;
    const u32 offset = use_windows && window.length ? 0 : std::min(binding.offset, buffer.length);
    views[i] = buffer.buffer
                   ? plume::RenderVertexBufferView(buffer.buffer->at(offset),
                                                   buffer.length - offset)
                   : plume::RenderVertexBufferView(null_buffer->at(0), 256);
  }
  BindNativeDrawVertexBuffers(commands,draw_device,views,slots);
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
  static thread_local u32 color_traces = 0;
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

  // Fullscreen image processing carries scene motion/depth through to final
  // color. Replaying tone mapping/opaque scene-image copies as unknown geometry
  // would replace the entire motion field with zero and reject all history.
  const bool temporal_image_domain = NativeFsr3Enabled() && !temporal_scene &&
      !DecodeDepthState(key.depth_control,depth.texture != nullptr).enabled &&
      raster_extent.logical_width == 1280 && raster_extent.logical_height == 720 &&
      viewport_width == 1280 && viewport_height == 720 &&
      float(device->viewport.x) == 0 && float(device->viewport.y) == 0;
  bool temporal_image_pass = temporal_image_domain && count <= 6 &&
      !DecodeColorBlend(key.blend_controls[0]).enabled;
  if (temporal_image_pass) {
    bool scene_input = key.pixel_shader_hash == 0x3A47E5DDE66B42C6ull;
    const u32 sampled = BoundShaderTextureMask(ShaderStage::kPixel);
    for (u32 slot=0; slot<kNativeTextureSlots && !scene_input; ++slot) {
      if (!(sampled & (1u<<slot)) || !bindings.textures[slot]) continue;
      const auto input_extent = NativeTextureExtent(bindings.textures[slot]);
      scene_input = input_extent.logical_width == 1280 && input_extent.logical_height == 720;
    }
    temporal_image_pass = scene_input;
  }
  // Deferred volume lights and fog can blend and use far more than six indices.
  // Their original color draw must still run; only the fallback temporal draw
  // would incorrectly wipe the scene's motion/rejection field (capture 16298).
  static const bool scene_pass_history = [] {
    const char* value = std::getenv("LEGO_NATIVE_SCENE_PASS_HISTORY");
    return !value || std::strcmp(value,"0") != 0;
  }();
  if (!temporal_image_pass && temporal_image_domain && scene_pass_history &&
      TemporalSceneInputMask(key.pixel_shader_hash)) {
    temporal_image_pass = PreservesTemporalSceneHistory(key.pixel_shader_hash,
        BoundShaderTextureMask(ShaderStage::kPixel), raster_extent, [&](u32 slot) {
          return bindings.textures[slot] ? NativeTextureExtent(bindings.textures[slot])
                                        : NativeRenderExtent{};
        });
  }
  bool temporal_previous_valid = false;
  bool temporal_replayed = false;
  const auto replay_temporal = [&](bool indexed_draw, u32 vertices, u32 first, i32 base) {
    if (!NativeFsr3Enabled() || TemporalSceneComposed(g_probe_frame.load()) || !color.texture || !(key.color_write_mask & 15u) || temporal_image_pass ||
        raster_extent.logical_width != 1280 || raster_extent.logical_height != 720) return;
    auto* motion_fb = TemporalFramebuffer(temporal_scene ? depth.texture : nullptr);
    if (!motion_fb) return;
    temporal_replayed = true;
    bool trace_motion = (count <= 6 || temporal_previous_valid) &&
        viewport_width == 1280 && viewport_height == 720 &&
        TemporalMotionSnapshotActive();
    std::string trace_label;
    if (trace_motion) {
      static u64 trace_frame = ~u64{0};
      static u32 trace_quads = 0;
      static u32 trace_meshes = 0, trace_fullscreen = 0;
      const auto frame = g_probe_frame.load();
      if (trace_frame != frame) {
        trace_frame=frame;trace_quads=trace_meshes=trace_fullscreen=0;
      }
      // Reserve captures for late image/overlay passes. An early batch of
      // valid scene meshes must not consume the entire diagnostic budget.
      trace_motion = count<=6 ? trace_fullscreen++<16 : trace_meshes++<8;
      if (trace_motion) {
        ++trace_quads;
        trace_label = std::to_string(trace_quads)+"-vs-"+std::to_string(key.vertex_shader_hash)+
            "-ps-"+std::to_string(key.pixel_shader_hash)+"-history-"+
            std::to_string(temporal_previous_valid);
        SnapshotTemporalMotion("before-"+trace_label);
      }
    }
    // Overlay passes use their original VS and material contribution when
    // available. Unknown passes retain the conservative full rejection mask.
    auto motion_key = key;
    if (!temporal_scene) {
      const auto mode = TemporalCoverageForBlend(DecodeColorBlend(key.blend_controls[0]));
      if (!TemporalMaterialReactiveEnabled() || !HasTemporalCoverageShader(key.pixel_shader_hash,mode,key.spec_constants))
        motion_key.pixel_shader_hash = 0;
      motion_key.depth_control = 0;
      motion_key.depth_format = u32(plume::RenderFormat::UNKNOWN);
    }
    auto* motion_pipeline = GetTemporalPipeline(motion_key,declaration,topology,
        temporal_scene ? depth.format : plume::RenderFormat::UNKNOWN);
    if (!motion_pipeline) {
      static u32 failed_replay_logs=0;
      if (failed_replay_logs++ < 16) REXLOG_WARN(
          "Native temporal replay unavailable frame={} VS={:016X} PS={:016X} spec={} declaration={:016X}",
          g_probe_frame.load(),key.vertex_shader_hash,key.pixel_shader_hash,key.spec_constants,key.declaration_hash);
      RejectTemporalMotion(commands); commands->setFramebuffer(framebuffer);
      return;
    }
    commands->setFramebuffer(motion_fb); commands->setPipeline(motion_pipeline);
    if (indexed_draw) commands->drawIndexedInstanced(vertices,1,first,base,0);
    else commands->drawInstanced(vertices,1,first,0);
    if (trace_motion) SnapshotTemporalMotion("after-"+trace_label);
    commands->setFramebuffer(framebuffer); commands->setPipeline(pipeline);
  };

  const auto bind_temporal_previous = [&](const BufferResourceView& index) {
    if (!TemporalInputsCandidate()) return true;
    if (!capture_temporal_constants) return true;
    // Reactive fallback shaders and depth-only prepasses do not consume b4.
    if (NativeFsr3Enabled() && (!temporal_scene || !color.texture || !(key.color_write_mask & 15u) || !key.pixel_shader ||
        !(HasVerifiedTemporalCamera(key.vertex_shader_hash) ||
          HasTemporalCameraConstants(key.vertex_shader_hash,key.spec_constants)) ||
        (FloatBlendIndicesInput({declaration.elements,declaration.element_count}) &&
         FindFloatBlendIndicesDxil(key.vertex_shader_hash,key.spec_constants)) ||
        FindPositionPrecisionDxil(key.vertex_shader_hash,key.spec_constants) ||
        !HasTemporalShader(key.vertex_shader_hash,false,key.spec_constants) ||
        !HasTemporalShader(key.pixel_shader_hash,true,key.spec_constants))) return true;
    const auto& temporal_textures = *temporal_texture_snapshot;
    auto previous = TemporalPreviousConstants{};
    TemporalCameraAuthority::Matrix current_camera;
    std::memcpy(current_camera.data(),temporal_snapshot.data(),sizeof(current_camera));
    const bool verified_world_transform = HasVerifiedTemporalTransform(key.vertex_shader_hash);
    const bool scene_camera = HasVerifiedTemporalCamera(key.vertex_shader_hash) ||
        g_temporal_camera_authority.Matches(g_probe_frame.load(),current_camera);
    static const bool vertex_texture_history = [] {
      const char* value = std::getenv("LEGO_NATIVE_VERTEX_TEXTURE_HISTORY");
      return !value || std::strcmp(value,"0") != 0;
    }();
    bool eligible = !(key.depth_control & 1u) &&
        !DecodeColorBlend(key.blend_controls[0]).enabled &&
        scene_camera &&
        temporal_textures.Complete() &&
        (!temporal_textures.required || vertex_texture_history) &&
        color.texture && depth.texture && key.pixel_shader &&
        raster_extent.logical_width == 1280 && raster_extent.logical_height == 720 &&
        float(device->viewport.x) == 0 && float(device->viewport.y) == 0 &&
        viewport_width == 1280 && viewport_height == 720 && !screen_space;
    TemporalDrawKey identity{};
    size_t used = 0;
    const auto add = [&](u64 value) { identity[used++] = value; };
    add(key.vertex_shader_hash); add(key.pixel_shader_hash); add(key.declaration_hash);
    add(key.spec_constants); add(primitive_type); add(indexed); add(start); add(count);
    add(u32(base_vertex)); add(raster_extent.width); add(raster_extent.height);
    add(std::bit_cast<u32>(float(device->viewport.min_z)));
    add(std::bit_cast<u32>(float(device->viewport.max_z)));
    // c48..51 are the object transform in the verified world profiles.
    // Camera/lighting/wind constants change while the same world mesh remains
    // visible; hashing them here would reject every camera-moving frame.
    add(XXH3_64bits(temporal_snapshot.data() + 48*16, 4*16));
    add(XXH3_64bits(temporal_snapshot.data() + 8192, 512));
    // Boolean/attribute-conversion state from the immutable upload, rather
    // than reading a possibly changed guest shadow a second time.
    add(XXH3_64bits(temporal_snapshot.data() + 8192 + 512, 624 - 512));
    // Vertex uploads already hash every use, including when memory watches
    // report unchanged bytes. Reuse previous transforms only with the exact
    // same immutable contents and bindings; animation writes reject history.
    add(temporal_textures.required);
    add(temporal_textures.required ?
        XXH3_64bits(temporal_textures.content.data(), sizeof(temporal_textures.content)) : 0);
    add(index.content_hash); add(index.byte_order_hash); add(index.length);
    add(index.guest_format); add(indexed ? bindings.index_buffer : 0);
    add(use_windows); add(min_index);
    if (key.multi_rate_fetch) {
      add(0xF77D99A4104EA49Bull);add(multi_rate_draw.content_hash);add(multi_rate_draw.count);
    } else for (u32 i = 0; i < vertex_buffers.size(); ++i) {
      const auto& buffer = vertex_buffers[i];
      const auto& binding = bindings.vertex_streams[i];
      if (!buffer.buffer) {
        if (active_vertex_streams[i] && binding.buffer) eligible = false;
      }
      add(buffer.buffer ? binding.buffer : 0); add(buffer.content_hash);
      add(buffer.byte_order_hash); add(buffer.length);
      add(buffer.buffer ? binding.offset : 0); add(buffer.buffer ? binding.stride : 0);
      add(use_windows && buffer.buffer ? vertex_windows[i].offset : 0);
    }
    if (eligible) {
      TemporalVertexConstants current;
      std::memcpy(current.data(), temporal_snapshot.data(), sizeof(current));
      // Newly recognized programs retain the strict comparison of every
      // noncamera VS constant. Only inspected transform profiles may relax it
      // for wind/lighting changes. Matching projection parameters alone never
      // authorizes a separate menu camera.
      previous = g_temporal_draw_history.Record(identity, current, verified_world_transform);
    }
    RecordTemporalHistoryResult(previous.valid != 0);
    temporal_previous_valid = previous.valid != 0;
    auto upload = HostDevice::AllocateDrawUpload(sizeof(previous));
    if (!upload) return false;
    std::memcpy(upload.mapped, &previous, sizeof(previous));
    commands->setGraphicsRootDescriptor(upload.buffer->at(upload.offset), 3);
    return true;
  };

  // Occlusion counts are reported at guest resolution whatever the host scene size.
  const double query_sample_scale = raster_extent.width && raster_extent.height
      ? double(raster_extent.logical_width) * raster_extent.logical_height /
            (double(raster_extent.width) * raster_extent.height)
      : 1.0;
  if (key.multi_rate_fetch) {
    if (!bind_temporal_previous({})) return false;
    {
      QueryDrawScope query_scope(commands,query_sample_scale);
      commands->drawInstanced(multi_rate_draw.count,1,0,0);
    }
    replay_temporal(false,multi_rate_draw.count,0,0);
    if (t_recording_draw && !temporal_replayed) t_recording_draw->RecordPipeline(key,topology,depth.format);
  } else if (instanced_fetch) {
    // The sequential index list is implied by the instanced draw itself.
    if (!bind_temporal_previous({})) return false;
    QueryDrawScope query_scope(commands, query_sample_scale);
    if (instanced_fetch.expand) {
      commands->drawInstanced(instanced_fetch.expand_count, 1, 0, 0);
    } else {
    u32 instance = instanced_fetch.first_instance;
    if (instanced_fetch.head_count)
      commands->drawInstanced(instanced_fetch.head_count, 1, instanced_fetch.head_start, instance++);
    if (instanced_fetch.instances)
      commands->drawInstanced(instanced_fetch.vertices, instanced_fetch.instances, 0, instance);
    instance += instanced_fetch.instances;
    if (instanced_fetch.tail_count)
      commands->drawInstanced(instanced_fetch.tail_count, 1, 0, instance);
    }
  } else if (primitive_type == static_cast<u32>(rex::graphics::xenos::PrimitiveType::kQuadList)) {
    const u32 expanded_count = ExpandedQuadIndexCount(count);
    u32 nonindexed_last = 0;
    if (!expanded_count || (!indexed && !NonIndexedVertexRange(start, count, nonindexed_last)))
      return false;
    const auto source = indexed
        ? (use_windows ?
            ResolveBufferResourceWindow(bindings.index_buffer, BufferKind::kIndex, index_offset, index_bytes) :
            ResolveBufferResourceView(bindings.index_buffer, BufferKind::kIndex))
        : BufferResourceView{};
    const u32 source_start = use_windows ? 0 : start;
    const u32 element_size = source.guest_format == 1 ? 2 : 4;
    if (indexed && (!source.buffer || !DrawIndexWindow(source_start, count, element_size, source.length)))
      return false;
    auto expanded = indexed ? nullptr : g_quad_indices.Find(source_start,count);
    if (!expanded) {
      expanded = std::shared_ptr<plume::RenderBuffer>(HostDevice::Device()->createBuffer(
          plume::RenderBufferDesc::IndexBuffer(u64(expanded_count) * sizeof(u32),
                                              plume::RenderHeapType::UPLOAD)).release());
      if (!expanded) return false;
      auto* output = static_cast<u32*>(expanded->map());
      const auto* input = indexed ? static_cast<const u8*>(source.buffer->map()) : nullptr;
      if (!output || (indexed && !input)) {
        if (output) expanded->unmap();
        if (input) source.buffer->unmap();
        return false;
      }
      constexpr u32 order[] = {0, 1, 2, 0, 2, 3};
      for (u32 quad = 0; quad < count / 4; ++quad) {
        for (u32 corner = 0; corner < 6; ++corner) {
          const u32 index = source_start + quad * 4 + order[corner];
          output[quad * 6 + corner] = !indexed ? index
              : element_size == 2 ? reinterpret_cast<const u16*>(input)[index]
                                  : reinterpret_cast<const u32*>(input)[index];
        }
      }
      if (indexed) source.buffer->unmap();
      expanded->unmap();
      if (!indexed) g_quad_indices.Remember(source_start,count,
          u64(expanded_count)*sizeof(u32),expanded,
          [](auto old) {HostDevice::RetireResource(std::move(old));});
    }
    const plume::RenderIndexBufferView index_view(expanded->at(0),
        u64(expanded_count) * sizeof(u32), plume::RenderFormat::R32_UINT);
    commands->setIndexBuffer(&index_view);
    if (!bind_temporal_previous(source)) return false;
    {
      QueryDrawScope query_scope(commands, query_sample_scale);
      commands->drawIndexedInstanced(expanded_count, 1, 0,
          use_windows ? (indexed ? -i32(min_index) : 0) : (indexed ? base_vertex : 0), 0);
    }
    replay_temporal(true,expanded_count,0,
        use_windows ? (indexed ? -i32(min_index) : 0) : (indexed ? base_vertex : 0));
    HostDevice::RetireResource(std::move(expanded));
  } else if (indexed) {
    const auto index = t_recording_draw && t_recording_draw->geometry_ready && t_recording_draw->indices.view.buffer ?
        t_recording_draw->indices.view : use_windows ?
        ResolveBufferResourceWindow(bindings.index_buffer, BufferKind::kIndex, index_offset, index_bytes) :
        ResolveBufferResourceView(bindings.index_buffer, BufferKind::kIndex);
    // Header adoption can replace metadata; check the view actually bound to
    // this draw too. A range upload starts at zero in its own index buffer.
    if (!index.buffer || !DrawIndexWindow(use_windows ? 0 : start, count,
        index.guest_format == 1 ? 2 : 4, index.length)) {
      return false;
    }
    const plume::RenderFormat index_format =
        index.guest_format == 1 ? plume::RenderFormat::R16_UINT
                                : plume::RenderFormat::R32_UINT;
    const plume::RenderIndexBufferView index_view(index.buffer->at(0),
                                                  index.length, index_format);
    commands->setIndexBuffer(&index_view);
    if (!bind_temporal_previous(index)) return false;
    {
    QueryDrawScope query_scope(commands, query_sample_scale);
    commands->drawIndexedInstanced(count, 1, use_windows ? 0 : start,
        use_windows ? -i32(min_index) : base_vertex, 0);
    }
    replay_temporal(true,count,use_windows ? 0 : start,use_windows ? -i32(min_index) : base_vertex);
    if (t_recording_draw && !temporal_replayed) t_recording_draw->RecordPipeline(key, topology, depth.format);
  } else {
    if (!bind_temporal_previous({})) return false;
    {
    QueryDrawScope query_scope(commands, query_sample_scale);
    // Current native vertex recompilation consumes input attributes, not
    // SV_VertexID. Each used stream starts at the original first vertex.
    commands->drawInstanced(count, 1, use_windows ? 0 : start, 0);
    }
    replay_temporal(false,count,use_windows ? 0 : start,0);
    if (t_recording_draw && !temporal_replayed) t_recording_draw->RecordPipeline(key, topology, depth.format);
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
    static thread_local std::unordered_set<PipelineKey, PipelineKeyHash> traced;
    if (traced.size() < 100 && traced.insert(key).second) {
      REXLOG_INFO("Native stage: VS={:08X} PS={:08X} rt={:08X} fmt={} ds={:08X} z={:02X} "
          "view={},{},{},{} scissor={},{},{},{} tex={:08X},{:08X},{:08X},{:08X} "
          "tex4-7={:08X},{:08X},{:08X},{:08X} mask={:X} blend={:08X}",
          key.vertex_shader, key.pixel_shader, bindings.render_targets[0], key.render_target_formats[0],
          bindings.depth_stencil, key.depth_control, float(device->viewport.x), float(device->viewport.y),
          float(device->viewport.width), float(device->viewport.height), effective_scissor.left,
          effective_scissor.top, scissor_right, scissor_bottom,
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
  if(composition_input) {
    const auto scene=NativeTextureExtent(bindings.textures[0]);
    static std::set<std::pair<u32,u32>> reported_composition_extents;
    if(reported_composition_extents.emplace(scene.width,scene.height).second)
      REXLOG_INFO("Native FSR pre-composition submitted: frame={} source={:08X} scene={}x{} target={:08X} native=1920x1080",
          g_probe_frame.load(),bindings.textures[0],scene.width,scene.height,bindings.render_targets[0]);
  }
  if (TemporalInputsCandidate() && HasVerifiedTemporalCamera(key.vertex_shader_hash) &&
      !TemporalSceneComposed(g_probe_frame.load()) &&
      color.texture && depth.texture && raster_extent.logical_width == 1280 &&
      raster_extent.logical_height == 720 && viewport_width == 1280 && viewport_height == 720 &&
      float(device->viewport.x) == 0 && float(device->viewport.y) == 0 && !screen_space &&
      DecodeDepthState(key.depth_control,true).write) {
    g_temporal_scene_depth_surface = bindings.depth_stencil;
    g_temporal_scene_depth_dirty = true;
  }
  if (cpu_trace) cpu_trace->submitted = true;
  const auto mark_writes = [targets = bindings.render_targets, depth_target = bindings.depth_stencil,
      mask = key.color_write_mask, writes_depth = DecodeDepthState(key.depth_control, depth.texture != nullptr).write] {
    for (u32 i = 0; i < kNativeRenderTargets; ++i)
      if ((mask >> (i * 4)) & 15u) MarkSurfaceWritten(targets[i]);
    if (writes_depth) MarkSurfaceWritten(depth_target);
  };
  if (auto* prepared = CurrentDrawPreparation()) {
    if (!prepared->writes_marked) RecordNativeCommand(commands, [mark_writes](auto*) { mark_writes(); });
  } else mark_writes();
  return true;
}

u32 ClearRecorded(u32 flags, u32 color, f64 depth, u32 stencil,
                  const std::vector<plume::RenderRect>& rectangles,
                  const DrawBindings* captured = nullptr) {
  auto recording = HostDevice::LockRecording();
  if(!RetryNativeCompositionTargets()) return 0x80004005u;
  const DrawBindings bindings = captured ? *captured : SnapshotDrawBindings();
  auto* commands = HostDevice::BeginFrameCommands();
  if (!commands) return 0x80004005u;
  if (TemporalInputsCandidate() && (flags & 2u))
    SnapshotTemporalSceneDepth(bindings.depth_stencil);
  static std::atomic<u32> clear_sequence{0};
  const u32 clear_id = clear_sequence.fetch_add(1);
  static u32 tiled_clear_logs = 0;
  const bool trace_clear = clear_id < 128 || (g_seen_tiled_pass && tiled_clear_logs++ < 24);
  if (trace_clear)
    REXLOG_INFO("Native GPU: clear #{} flags={:X} rt={:08X},{:08X},{:08X},{:08X} ds={:08X} color={:08X} z={}",
        clear_id, flags, bindings.render_targets[0], bindings.render_targets[1],
        bindings.render_targets[2], bindings.render_targets[3], bindings.depth_stencil, color, depth);

  std::vector<plume::RenderRect> physical_rectangles;
  const auto clear_regions = [&](u32 address) -> const plume::RenderRect* {
    const auto extent=NativeTextureExtent(address);
    physical_rectangles.clear();
    for (const auto& r : rectangles) physical_rectangles.emplace_back(
        NativeRenderEdge(r.left,extent.logical_width,extent.width),
        NativeRenderEdge(r.top,extent.logical_height,extent.height),
        NativeRenderEdge(r.right,extent.logical_width,extent.width),
        NativeRenderEdge(r.bottom,extent.logical_height,extent.height));
    return physical_rectangles.empty() ? nullptr : physical_rectangles.data();
  };
  const u32 clear_rectangle_count = static_cast<u32>(rectangles.size());
  const float scale = 1.0f / 255.0f;
  // X360 TARGET0..3 are independent bits 1/2/4/8. Bind each selected
  // attachment alone for clear; its shader output slot is unchanged for draws.
  for (u32 i = 0; i < kNativeRenderTargets; ++i) {
    if (!(flags & (1u << i))) continue;
    const auto target = ResolveTextureResource(bindings.render_targets[i]);
    if (!rectangles.empty() && !PrepareSurfaceColorAlias(bindings.render_targets[i])) return 0x80004005u;
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
    commands->clearColor(0, encoded_color, clear_regions(bindings.render_targets[i]), clear_rectangle_count);
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
        static_cast<float>(depth), stencil, clear_regions(bindings.depth_stencil), clear_rectangle_count);
    if (flags & 0x10) MarkSurfaceWritten(bindings.depth_stencil);
  }
  if (trace_clear) REXLOG_INFO("Native GPU: clear #{} complete", clear_id);
  return 0;
}

// The rectangle list is guest memory the caller may reuse, so it is read here;
// the clear itself queues behind the draws it must follow.
u32 ClearHook(D3DDevice* device, u32 rectangle_count,
              u32 rectangles_address, u32 flags, u32 color, f64 depth,
              u32 /*depth_gpr_slot*/, u32 stencil, u32 /*edram_clear*/) {
  const bool snapshot = device && SnapshotStateBindingsEnabled();
  const auto bindings = snapshot ? ReadDeviceBindings(device) : DrawBindings{};
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
  if (NativeWorkerShouldQueue() && NativeQueuedResolveEnabled()) {
    NativeWorkerSubmit([flags, color, depth, stencil, snapshot, bindings, rectangles = std::move(rectangles)] {
      ClearRecorded(flags, color, depth, stencil, rectangles, snapshot ? &bindings : nullptr);
    });
    return 0;
  }
  return ClearRecorded(flags, color, depth, stencil, rectangles, snapshot ? &bindings : nullptr);
}

// F10 GPU pass profile bracket (see gpu_pass_profile.h). Inactive = two loads.
struct PassProfileScope {
  const char* kind;
  const D3DDevice* device;
  u32 index;
  PassProfileScope(const char* k, const D3DDevice* d) : kind(k), device(d),
      index(pass_profile::Get().capturing ? pass_profile::Begin(HostDevice::BeginFrameCommands()) : ~0u) {}
  ~PassProfileScope() {
    if (index == ~0u) return;
    pass_profile::Key key{kind, 0, 0, 0, 0};
    if (device) {
      key.vs = BoundShaderHash(ShaderStage::kVertex);
      key.ps = BoundShaderHash(ShaderStage::kPixel);
      key.width = u32(device->viewport.width);
      key.height = u32(device->viewport.height);
    }
    pass_profile::End(HostDevice::BeginFrameCommands(), index, key);
  }
};

// Threaded recording: the draw reads the guest device shadow (constants, fetch
// and render state) later on the worker, so it gets an immutable copy. Pooled,
// since a frame issues over a thousand of them.
constexpr size_t kSnapshotBytes = (offsetof(D3DDevice, vertex_shader) + sizeof(be_u32) + 63) & ~size_t{63};
// The guest's vertex shader float constant bank inside the device.
constexpr size_t kVertexConstantsBegin = 0x780, kVertexConstantsEnd = 0x1780;

// A pooled snapshot; a delta's own bytes are valid only in the vertex constants while base holds the rest.
struct alignas(64) DeviceBlock {
  D3DDevice device;
  std::shared_ptr<D3DDevice> base;
};
static_assert(std::is_standard_layout_v<DeviceBlock> && offsetof(DeviceBlock, device) == 0);
// Only for devices from AcquireDeviceSnapshot.
DeviceBlock& BlockOf(D3DDevice* device) { return *reinterpret_cast<DeviceBlock*>(device); }

// Recycles blocks together with their shared_ptr control block, so a warm pool allocates nothing.
template <class T>
struct SnapshotBlockAllocator {
  using value_type = T;
  struct FreeList {
    std::mutex mutex;
    std::vector<T*> blocks;
  };
  static FreeList& Pool() {
    static FreeList pool;
    return pool;
  }
  SnapshotBlockAllocator() = default;
  template <class U>
  SnapshotBlockAllocator(const SnapshotBlockAllocator<U>&) noexcept {}
  T* allocate(size_t count) {
    if (count == 1) {
      auto& pool = Pool();
      std::lock_guard lock(pool.mutex);
      if (!pool.blocks.empty()) {
        T* block = pool.blocks.back();
        pool.blocks.pop_back();
        return block;
      }
    }
    void* block = ::operator new(count * sizeof(T), std::align_val_t{alignof(T)});
    std::memset(block, 0, count * sizeof(T));
    return static_cast<T*>(block);
  }
  void deallocate(T* block, size_t count) noexcept {
    if (count != 1) {
      ::operator delete(block, std::align_val_t{alignof(T)});
      return;
    }
    auto& pool = Pool();
    std::lock_guard lock(pool.mutex);
    pool.blocks.push_back(block);
  }
  template <class U>
  bool operator==(const SnapshotBlockAllocator<U>&) const noexcept { return true; }
};

std::shared_ptr<D3DDevice> AcquireDeviceSnapshot() {
  // The renderer reads nothing past vertex_shader; the tail is left as the pooled copy had it.
  static_assert(kSnapshotBytes <= sizeof(D3DDevice) && std::is_trivially_default_constructible_v<D3DDevice>);
  auto block = std::allocate_shared_for_overwrite<DeviceBlock>(SnapshotBlockAllocator<DeviceBlock>{});
  auto* device = &block->device;
  return std::shared_ptr<D3DDevice>(std::move(block), device);
}

std::shared_ptr<D3DDevice> SnapshotDevice(const D3DDevice* device) {
  if (RendererOptimizationEnabled(kDeviceSnapshotPool)) {
    static thread_local DeviceSnapshotPool<D3DDevice, kSnapshotBytes> pool;
    return pool.Capture(device);
  }
  auto copy = AcquireDeviceSnapshot();
  // Streaming stores: the copy is read on another core, so filling this core's cache first is wasted.
  static_assert(kSnapshotBytes % 32 == 0);
  auto* to = reinterpret_cast<__m256i*>(copy.get());
  const auto* from = reinterpret_cast<const __m256i*>(device);
  for (size_t i = 0; i < kSnapshotBytes / 32; ++i) _mm256_stream_si256(to + i, _mm256_loadu_si256(from + i));
  _mm_sfence();
  return copy;
}

bool DeltaSnapshotsEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_DELTA_SNAPSHOTS");
    return !value || value[0] != '0';
  }();
  return enabled;
}

bool ChainedMergeEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_CHAINED_MERGE");
    return !value || value[0] != '0';
  }();
  return enabled;
}

bool DrawMergeEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_DRAW_MERGE");
    return !value || value[0] != '0';
  }();
  return enabled;
}

struct DrawSnapshot {
  // With same_as set, a block from AcquireDeviceSnapshot that may be a delta until completed.
  std::shared_ptr<D3DDevice> device;
  // Order of draw snapshots; same_as names this thread's previous one when every byte outside the
  // vertex constants equals it, else 0.
  u64 sequence = 0, same_as = 0;
};
// Keeps the draw hook captures within MSVC std::function's inline buffer.
static_assert(sizeof(DrawSnapshot) <= 32);

// A draw hook's snapshot; a repeat of this thread's previous one outside the vertex constants copies
// only those and refers to the last full snapshot for the rest.
DrawSnapshot SnapshotDrawDevice(const D3DDevice* device) {
  if (!DrawMergeEnabled() || RendererOptimizationEnabled(kDeviceSnapshotPool)) return {SnapshotDevice(device)};
  static std::atomic<u64> sequences{0};
  // The last full snapshot and its bytes, kept in cache; repeats leave both unchanged.
  alignas(64) static thread_local std::array<u8, kSnapshotBytes> kept;
  static thread_local std::shared_ptr<D3DDevice> kept_full;
  static thread_local u64 kept_sequence = 0;
  static_assert(kVertexConstantsBegin % 64 == 0 && kVertexConstantsEnd % 64 == 0 && kSnapshotBytes % 64 == 0);
  const auto* from = reinterpret_cast<const __m256i*>(device);
  auto* previous = reinterpret_cast<__m256i*>(kept.data());
  // Loads only, back to front, so the bindings and shaders at the end are compared first.
  const auto equal = [&](size_t begin, size_t end) {
    for (size_t i = end; i > begin; i -= 2) {
      const __m256i differs =
          _mm256_or_si256(_mm256_xor_si256(_mm256_loadu_si256(from + i - 2), _mm256_loadu_si256(previous + i - 2)),
                          _mm256_xor_si256(_mm256_loadu_si256(from + i - 1), _mm256_loadu_si256(previous + i - 1)));
      if (!_mm256_testz_si256(differs, differs)) return false;
    }
    return true;
  };
  const bool same = kept_sequence && equal(kVertexConstantsEnd / 32, kSnapshotBytes / 32) &&
      equal(0, kVertexConstantsBegin / 32);
  DrawSnapshot result{AcquireDeviceSnapshot(), sequences.fetch_add(1, std::memory_order_relaxed) + 1};
  // Streaming stores: the copy is read on another core.
  auto* to = reinterpret_cast<__m256i*>(result.device.get());
  const auto copy = [&](size_t begin, size_t end, bool keep) {
    for (size_t i = begin; i < end; ++i) {
      const __m256i value = _mm256_loadu_si256(from + i);
      if (keep) _mm256_storeu_si256(previous + i, value);
      _mm256_stream_si256(to + i, value);
    }
  };
  copy(kVertexConstantsBegin / 32, kVertexConstantsEnd / 32, false);
  if (same && kept_full && DeltaSnapshotsEnabled()) {
    BlockOf(result.device.get()).base = kept_full;
  } else {
    copy(0, kVertexConstantsBegin / 32, true);
    copy(kVertexConstantsEnd / 32, kSnapshotBytes / 32, true);
    kept_full = result.device;
  }
  _mm_sfence();
  if (same) result.same_as = kept_sequence;
  kept_sequence = result.sequence;
  return result;
}

// The device holding a snapshot's bytes outside the vertex constants, without completing a delta.
const D3DDevice* SnapshotStateDevice(const DrawSnapshot& snapshot) {
  const D3DDevice* base = snapshot.same_as ? BlockOf(snapshot.device.get()).base.get() : nullptr;
  return base ? base : snapshot.device.get();
}

bool OverlapDrawBatches() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_OVERLAP_DRAW_BATCHES");
    return !value || value[0] != '0';
  }();
  return enabled;
}

bool PooledDrawWorkEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_DRAW_WORK_POOL");
    return !value || value[0] != '0';
  }();
  const auto mode = g_draw_benchmark_mode.load(std::memory_order_relaxed);
  return mode == 1 ? false : mode == 2 ? true : enabled;
}

std::atomic<u32>& FullDrawBatchLimit() {
  static std::atomic<u32> limit{[] {
    const char* value = std::getenv("LEGO_NATIVE_FULL_DRAW_BATCH_SIZE");
    const int parsed = value ? std::atoi(value) : 64;
    return u32(parsed >= 1 && parsed <= 256 ? parsed : 64);
  }()};
  return limit;
}

void PollFullDrawBatchLimit() {
  static const std::string path = [] {
    const char* value = std::getenv("LEGO_NATIVE_FULL_DRAW_BATCH_FILE");
    return value ? std::string(value) : std::string{};
  }();
  static u32 polls = 0;
  if (path.empty() || polls++ % 60) return;
  FILE* input = nullptr;
  if (fopen_s(&input, path.c_str(), "rb") || !input) return;
  char value[16]{};
  const bool read = std::fgets(value, sizeof(value), input) != nullptr;
  std::fclose(input);
  const int parsed = read ? std::atoi(value) : 0;
  if (parsed < 1 || parsed > 256) return;
  if (FullDrawBatchLimit().exchange(u32(parsed), std::memory_order_relaxed) != u32(parsed))
    REXLOG_INFO("Native full draw batch size: {} frame={}", parsed, g_probe_frame.load());
}

void PollDrawBenchmarkMode() {
  static const std::string path = [] {
    const char* value = std::getenv("LEGO_NATIVE_DRAW_MODE_FILE");
    return value ? std::string(value) : std::string{};
  }();
  static u32 polls = 0;
  if (path.empty() || polls++ % 60) return;
  FILE* input = nullptr;
  if (fopen_s(&input, path.c_str(), "rb") || !input) return;
  const int mode = std::fgetc(input) - '0';
  std::fclose(input);
  if (mode < 0 || mode > 2 || mode == g_draw_benchmark_mode.load()) return;
  g_draw_benchmark_mode.store(mode);
  g_prepared_draw.Reset();
  constexpr const char* labels[]{"serial preparation", "parallel heap", "parallel pool"};
  REXLOG_INFO("Native draw benchmark mode: {} frame={}", labels[mode], g_probe_frame.load());
}

std::shared_ptr<ParallelDrawWork> AllocateParallelDrawWork() {
  if (!PooledDrawWorkEnabled()) return std::make_shared<ParallelDrawWork>();
  static thread_local DrawWorkPool<ParallelDrawWork> pool;
  return pool.Acquire();
}

std::shared_ptr<void> RetainDrawBatchResources(
    const std::shared_ptr<std::vector<std::shared_ptr<ParallelDrawWork>>>& batch) {
  if (!PooledDrawWorkEnabled()) return batch;
  DrawResourceLease::Builder lease;
  for (const auto& work : *batch) {
    for (const auto& owner : work->shaders.owners) lease.Add(owner);
    for (const auto& texture : work->texture_inputs) lease.Add(texture.owner);
    for (const auto& input : work->vertex_inputs) lease.Add(input.owner);
    for (const auto& vertex : work->vertices) {
      lease.Add(vertex.owner);
      lease.Add(vertex.storage);
    }
    lease.Add(work->index_input.owner);
    lease.Add(work->indices.owner);
    lease.Add(work->indices.storage);
  }
  return lease.Finish();
}

struct UploadArena {
  std::shared_ptr<plume::RenderBuffer> buffer;
  u8* mapped = nullptr;
  u32 capacity = 0;
};
constexpr u32 kDrawSliceBytes = 8960;
constexpr u32 kDrawUploadBytes = 2 * kDrawSliceBytes;
std::mutex g_arena_mutex;
// Size classes of 8 << i works; a retired arena returns here after its last fence.
std::array<std::vector<std::unique_ptr<UploadArena>>, 8> g_arena_pool;

std::shared_ptr<UploadArena> AcquireUploadArena(u32 works) {
  u32 cls = 0;
  while ((8u << cls) < works && cls < 7) ++cls;
  const u64 required=u64(works)*kDrawUploadBytes;
  if (!works || required>UINT32_MAX) return {};
  const u32 pooled_capacity=(8u<<cls)*kDrawUploadBytes;
  const bool pooled=required<=pooled_capacity;
  const u32 capacity=pooled?pooled_capacity:u32(required);
  std::unique_ptr<UploadArena> arena;
  {
    std::lock_guard lock(g_arena_mutex);
    auto& free = g_arena_pool[cls];
    if (pooled && !free.empty()) { arena = std::move(free.back()); free.pop_back(); }
  }
  if (!arena) {
    auto* device = HostDevice::Device();
    if (!device) return {};
    arena = std::make_unique<UploadArena>();
    arena->buffer.reset(device->createBuffer(plume::RenderBufferDesc::UploadBuffer(capacity)).release());
    if (!arena->buffer) return {};
    arena->mapped = static_cast<u8*>(arena->buffer->map());
    if (!arena->mapped) return {};
    arena->capacity = capacity;
  }
  return std::shared_ptr<UploadArena>(arena.release(), [cls,pooled](UploadArena* released) {
    if (!pooled) { delete released;return; }
    std::lock_guard lock(g_arena_mutex);
    g_arena_pool[cls].emplace_back(released);
  });
}

u32 PrepareChunkSize() {
  static const u32 size = [] {
    const char* value = std::getenv("LEGO_NATIVE_PREPARE_CHUNK");
    const int parsed = value ? std::atoi(value) : 8;
    return u32(parsed >= 1 && parsed <= 64 ? parsed : 8);
  }();
  return size;
}

bool ChunkDependencyWaitsEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_CHUNK_DEPENDENCY_WAITS");
    return value && value[0] != '0';
  }();
  return enabled;
}

struct FullDrawWork {
  std::shared_ptr<D3DDevice> device;
  DrawBindings bindings;
  ShaderBindingsSnapshot shaders;
  // Versions, windows and cached views chosen on the coordinator in guest order.
  ParallelDrawWork draw;
  u32 primitive = 0, start = 0, count = 0;
  i32 base_vertex = 0;
  bool indexed = false, submitted = false, deferred = false, captured = false;
  u32 chunk = ~0u;
  u64 sequence = 0;
  std::unique_ptr<NativeDrawCommands> commands, uploads;
  DrawPreparationContext preparation;
  DrawTiming timing;
  std::function<void()> replay_task;
  // Draws merged into this one, in order: snapshots equal to this draw's outside the vertex constants,
  // each a pooled block that may hold only its vertex constants until completed.
  std::vector<std::shared_ptr<D3DDevice>> followers;
  // The run's last snapshot; the next follower must name it as its equal predecessor.
  u64 last_snapshot = 0;
  // Arena slot of the first follower.
  u32 follower_slot = 0;
  bool clean = false;
  void Reset() {
    if (clean) return;
    clean = true;
    followers.clear(); last_snapshot = 0;
    device.reset(); shaders = {}; preparation.retired.clear(); preparation.unregister.clear();
    preparation.pinned.clear(); replay_task = nullptr;
    preparation.used = 0; preparation.alias = {}; preparation.color_aliases = {};
    preparation.alias_decided = preparation.writes_marked = false;
    submitted = deferred = captured = false; chunk = ~0u; timing = {};
    draw.Reset(); draw.bindings = {};
    if (!commands) commands = CreateNativeDrawCommands();
    if (!uploads) uploads = CreateNativeDrawCommands();
    commands->Reset(); uploads->Reset();
  }
};
struct FullDrawBatch {
  std::vector<std::shared_ptr<FullDrawWork>> works;
  std::vector<PreparationDependencyJob> jobs;
  // Owners each chunk's draws need until the GPU is done, gathered by the worker that ran it.
  std::vector<std::shared_ptr<DrawResourceLease>> chunk_leases;
  std::shared_ptr<UploadArena> arena, multi_rate_arena;
  PreparationDependencyFilter bloom;
  std::atomic<u32> pending_jobs{0};
  u64 sequence = 0, barrier = 0;
};
// Completes a delta in place from its base, exact since a delta is taken only when its other bytes
// equal the base; one thread per block.
void CompleteDevice(D3DDevice* device) {
  auto& block = BlockOf(device);
  if (!block.base) return;
  auto* to = reinterpret_cast<u8*>(device);
  const auto* from = reinterpret_cast<const u8*>(block.base.get());
  std::memcpy(to, from, kVertexConstantsBegin);
  std::memcpy(to + kVertexConstantsEnd, from + kVertexConstantsEnd, kSnapshotBytes - kVertexConstantsEnd);
  block.base.reset();
}
// A snapshot's full device; a delta, which only a snapshot with same_as can be, is completed first.
std::shared_ptr<D3DDevice> SnapshotFullDevice(DrawSnapshot& snapshot) {
  if (snapshot.same_as) CompleteDevice(snapshot.device.get());
  return snapshot.device;
}
// A delta is stored as is; a snapshot without same_as may not be a pooled block and is copied into one.
void AddFollower(FullDrawWork& head, DrawSnapshot&& snapshot) {
  if (!snapshot.same_as) {
    auto copy = AcquireDeviceSnapshot();
    std::memcpy(copy.get(), snapshot.device.get(), kSnapshotBytes);
    snapshot.device = std::move(copy);
  }
  head.followers.push_back(std::move(snapshot.device));
}
// Follower k's guest vertex constant bank, big-endian, kVertexConstantsEnd - kVertexConstantsBegin bytes.
const u8* FollowerVertexConstants(const FullDrawWork& head, size_t k) {
  return reinterpret_cast<const u8*>(head.followers[k].get()) + kVertexConstantsBegin;
}
// Follower k's full device, for preparing it as its own draw; completed in place, so it lives until Reset.
std::shared_ptr<D3DDevice> FollowerDevice(FullDrawWork& head, size_t k) {
  CompleteDevice(head.followers[k].get());
  return head.followers[k];
}

std::vector<std::shared_ptr<FullDrawWork>> g_full_draw_batch;
// Draws in the open batch, followers included; one arena slot each.
u32 g_full_draw_batch_draws = 0;
constexpr u32 kMergedBatchDraws = 512;
PreparationDependencyFilter g_full_draw_batch_bloom;
std::deque<std::shared_ptr<FullDrawBatch>> g_inflight_batches;
thread_local bool t_on_coordinator = false;
u64 g_full_draw_sequence = 0, g_full_draw_batches = 0;
thread_local DrawWorkPool<FullDrawWork> t_full_draw_pool;

void AddFullDrawPhysicalDependencies(PreparationPhysicalFilter& filter, const FullDrawWork& work) {
  if (!work.captured) { filter.unknown = true; return; }
  for (const auto& input : work.draw.texture_inputs) filter.Add(input.footprint);
}

// Moves every owner a replayed draw needs into the lease; duplicates are only released.
template <class Builder>
void TakeDrawOwners(Builder& lease, FullDrawWork& work) {
  for (auto& shader : work.shaders.owners) lease.Take(shader);
  for (auto& retired : work.preparation.retired) lease.Take(retired);
  for (auto& pin : work.preparation.pinned) lease.Take(pin.owner);
  if (work.captured) {
    for (auto& texture : work.draw.texture_inputs) lease.Take(texture.owner);
    for (auto& input : work.draw.vertex_inputs) lease.Take(input.owner);
    for (auto& vertex : work.draw.vertices) { lease.Take(vertex.owner); lease.Take(vertex.storage); }
    lease.Take(work.draw.index_input.owner);
    lease.Take(work.draw.indices.owner);
    lease.Take(work.draw.indices.storage);
  }
}

bool ReplayAfterPreparationEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_REPLAY_AFTER_PREPARATION");
    return !value || value[0] != '0';
  }();
  return enabled;
}

bool FullDrawReclaimEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_FULL_DRAW_RECLAIM");
    return !value || value[0] != '0';
  }();
  return enabled;
}

bool PreparationStealEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_PREPARATION_STEAL");
    return !value || value[0] != '0';
  }();
  return enabled;
}

// Resets replayed draw works off the recorder; the pool reuses a work only after this thread drops
// the batch, so a reset never races a reuse.
class FullDrawReclaimer {
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::shared_ptr<FullDrawBatch>> batches_;
  void Run() {
    NativeNameCurrentThread(L"Native GPU reclaim");
    for (;;) {
      std::shared_ptr<FullDrawBatch> batch;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] { return !batches_.empty(); });
        batch = std::move(batches_.front());
        batches_.pop_front();
      }
      for (const auto& work : batch->works) work->Reset();
    }
  }
  FullDrawReclaimer() { std::thread([this] { Run(); }).detach(); }
 public:
  // Never destroyed: the thread lives as long as the process.
  static FullDrawReclaimer& Instance() {
    static auto* reclaimer = new FullDrawReclaimer();
    return *reclaimer;
  }
  // False when the backlog is long; the caller resets inline instead.
  bool Post(std::shared_ptr<FullDrawBatch> batch) {
    {
      std::lock_guard lock(mutex_);
      if (batches_.size() >= 64) return false;
      batches_.push_back(std::move(batch));
    }
    ready_.notify_one();
    return true;
  }
};

void PrepareFullDrawChunk(FullDrawBatch& batch, size_t first, size_t end, u32 index);

// Runs a chunk on this thread if no worker has claimed it yet, exactly as a worker would.
void RunPreparationChunkHere(const PreparationDependencyJob& job) {
  if (!t_on_coordinator || !job.task || !PreparationStealEnabled() ||
      job.ready.wait_for(std::chrono::seconds(0)) == std::future_status::ready) return;
  const bool on_coordinator = std::exchange(t_on_coordinator, false);
  const bool running_task = NativeWorkerExchangeRunningTask(false);
  job.task->TryRun();
  NativeWorkerExchangeRunningTask(running_task);
  t_on_coordinator = on_coordinator;
}

void RepointDeclaration(ParallelDrawWork& draw, const ParallelDrawWork& from);
// The pipeline of draw's last recorded draw with its vertex shader's instanced-constants variant, or null.
plume::RenderPipeline* InstancedConstantsPipeline(const ParallelDrawWork& draw);

// A follower's constants: its own vertex bank in its slice; the pixel and shared banks are unchanged
// since the head's preparation on this worker, so they resolve to the head's uploads.
bool BindMergedConstants(plume::RenderCommandList* commands, const u8* vertex_constants, u64 serial,
                         DrawUploadSlice slice) {
  auto& banks = WorkerConstantBanks();
  banks.UpdateFloatBank(0, {vertex_constants, kVertexConstantsEnd - kVertexConstantsBegin}, 0, CopyBigEndianDwords);
  std::array<DrawUploadSlice, 3> uploads;
  if (!banks.ResolveUploads(serial, [&](u32 size) { return size <= kDrawSliceBytes ? slice : DrawUploadSlice{}; }, uploads))
    return false;
  for (u32 i = 0; i < uploads.size(); ++i)
    commands->setGraphicsRootDescriptor(uploads[i].buffer->at(uploads[i].offset), i);
  return true;
}

// A follower prepared exactly as its own draw work would be, recorded after the head's commands.
bool PrepareFollowerFull(FullDrawWork& head, const ParallelDrawWork& pristine, const std::shared_ptr<D3DDevice>& device,
                         DrawUploadSlice preparation, DrawUploadSlice constants, NativeDrawCommands* commands) {
  static thread_local ParallelDrawWork draw;
  draw = pristine;
  RepointDeclaration(draw, pristine);
  draw.device = device;
  draw.serial = head.draw.serial;
  draw.allocation = constants;
  DrawPreparationContext context;
  context.view = head.preparation.view;
  context.view.commands = commands;
  // A separate work replays its uploads right before its commands; here they record in that order.
  context.uploads = commands;
  context.allocation = preparation;
  context.capacity = kDrawSliceBytes;
  context.pinned = head.preparation.pinned;
  // Its alias decision was empty and its writes were marked at capture.
  context.alias_decided = context.writes_marked = true;
  bool submitted;
  {
    DrawPreparationScope scope(context);
    ShaderBindingsScope shaders(head.shaders);
    DrawBindingsScope bindings(head.bindings);
    PrepareDrawWork(draw);
    t_recording_draw = &draw;
    submitted = DispatchDraw(device.get(), head.primitive, head.indexed, head.start, head.count, head.base_vertex);
    t_recording_draw = nullptr;
  }
  for (auto& retired : context.retired) head.preparation.retired.push_back(std::move(retired));
  head.preparation.unregister.insert(head.preparation.unregister.end(), context.unregister.begin(), context.unregister.end());
  draw.Reset(); draw.textures = {}; draw.layout.reset();
  return submitted;
}

bool DrawInstancingEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_DRAW_INSTANCING");
    return !value || value[0] != '0';
  }();
  return enabled;
}

// Followers after their head: when the head recorded only state settings and then draws, each
// follower repeats those draws with its own vertex constants, otherwise it is prepared in full.
void PrepareFollowers(FullDrawBatch& batch, FullDrawWork& head, const ParallelDrawWork& pristine) {
  const bool repeatable = head.captured && head.submitted && head.draw.constants_ready &&
      !head.draw.missing_textures && head.uploads->Empty() && head.commands->FreezeDrawTail();
  auto* buffer = batch.arena->buffer.get();
  auto* mapped = batch.arena->mapped;
  // A repeatable head of one draw issues the whole run as instances, banks in the unused follower slots.
  if (repeatable && DrawInstancingEnabled() && !head.followers.empty()) {
    const u64 base = u64(head.follower_slot) * kDrawUploadBytes;
    const u32 instances = u32(1 + head.followers.size());
    if (auto* pipeline = InstancedConstantsPipeline(head.draw);
        pipeline && head.commands->InstanceDrawTail(pipeline, buffer->at(base), instances)) {
      constexpr u32 kBankBytes = kVertexConstantsEnd - kVertexConstantsBegin;
      CopyBigEndianDwords(mapped + base, reinterpret_cast<const u8*>(head.device.get()) + kVertexConstantsBegin,
                          kBankBytes);
      for (size_t k = 0; k < head.followers.size(); ++k)
        CopyBigEndianDwords(mapped + base + (k + 1) * kBankBytes, FollowerVertexConstants(head, k), kBankBytes);
      return;
    }
  }
  for (size_t k = 0; k < head.followers.size(); ++k) {
    const u64 offset = (u64(head.follower_slot) + k) * kDrawUploadBytes;
    const DrawUploadSlice preparation{buffer, mapped + offset, offset};
    const DrawUploadSlice constants{buffer, mapped + offset + kDrawSliceBytes, offset + kDrawSliceBytes};
    if (repeatable && BindMergedConstants(head.commands.get(), FollowerVertexConstants(head, k), head.draw.serial, constants)) {
      head.commands->RepeatDrawTail();
      continue;
    }
    head.submitted = PrepareFollowerFull(head, pristine, FollowerDevice(head, k), preparation, constants,
        head.commands.get()) && head.submitted;
  }
}

// One chunk of a batch on a preparation worker: whole DispatchDraw into the work's tapes.
void PrepareFullDrawChunk(FullDrawBatch& batch, size_t first, size_t end, u32 index) {
  NativeRecorderWaitExecuted(batch.barrier);
  static thread_local ParallelDrawWork pristine;
  for (size_t i = first; i < end; ++i) {
    auto& work = *batch.works[i];
    // Followers start from the head's captured decisions as they were before its preparation.
    const bool followers = !work.followers.empty();
    if (followers) {
      pristine = work.draw;
      RepointDeclaration(pristine, work.draw);
    }
    u64 calls = 0;
    {
      DrawPreparationScope preparation(work.preparation);
      // A rejected mesh still preserves prior eDRAM contents for consumers.
      if (!ApplySurfaceColorAliases(work.preparation.color_aliases))
        throw std::runtime_error("Cannot initialize captured color alias");
      ShaderBindingsScope shaders(work.shaders);
      DrawBindingsScope bindings(work.bindings);
      if (work.captured) {
        PrepareDrawWork(work.draw);
        t_recording_draw = &work.draw;
      }
      MultiRateDrawScope multi_rate_scope(work.draw);
      work.submitted = DispatchDraw(work.device.get(), work.primitive, work.indexed,
          work.start, work.count, work.base_vertex);
      t_recording_draw = nullptr;
      calls = g_draw_timing.calls;
    }
    if (followers) {
      PrepareFollowers(batch, work, pristine);
      pristine.Reset(); pristine.textures = {}; pristine.layout.reset();
    }
    DrawPreparationScope preparation(work.preparation);
    // Each follower is one draw call however it was prepared.
    if (followers) g_draw_timing.calls = calls + work.followers.size();
    work.timing = std::exchange(g_draw_timing, {});
  }
  // Nothing reads these owners after DispatchDraw; the recorder retires the lease after replay.
  DrawResourceLease::ChunkBuilder lease;
  for (size_t i = first; i < end; ++i) TakeDrawOwners(lease, *batch.works[i]);
  batch.chunk_leases[index] = lease.Finish();
  _mm_sfence();
  batch.pending_jobs.fetch_sub(1, std::memory_order_release);
}

// Coordinator: gives every draw its arena slice and posts the chunks; never waits.
void PrepareFullDrawBatch(const std::shared_ptr<FullDrawBatch>& batch) {
  batch->sequence = ++g_full_draw_batches;
  batch->barrier = NativeRecorderBarrier();
  auto view = HostDevice::ImmutableDrawDeviceView();
  u32 draws = 0;
  for (const auto& work : batch->works) draws += work->deferred ? 0 : 1 + u32(work->followers.size());
  batch->arena = draws ? AcquireUploadArena(draws) : nullptr;
  if (draws && !batch->arena) return;
  u64 multi_bytes=0;
  for (const auto& work:batch->works)
    if (!work->deferred && work->draw.multi_rate_input)
      multi_bytes+=(u64(NativeMultiRateUploadBytes(*work->draw.multi_rate_input))+255)&~u64{255};
  if (multi_bytes>UINT32_MAX-u64(kDrawUploadBytes)+1) return;
  if (multi_bytes) {
    batch->multi_rate_arena=AcquireUploadArena(u32((multi_bytes+kDrawUploadBytes-1)/kDrawUploadBytes));
    if (!batch->multi_rate_arena) return;
  }
  u64 multi_offset=0;
  u32 slot = 0;
  for (auto& work : batch->works) {
    if (work->deferred) continue;
    view.commands = work->commands.get();
    view.recording_serial = batch->sequence;
    work->preparation.view = view;
    work->preparation.uploads = work->uploads.get();
    const u64 offset = u64(slot++) * kDrawUploadBytes;
    work->preparation.allocation = {batch->arena->buffer.get(), batch->arena->mapped + offset, offset};
    work->preparation.capacity = kDrawSliceBytes;
    work->preparation.used = 0;
    work->draw.serial = batch->sequence;
    work->draw.allocation = {batch->arena->buffer.get(), batch->arena->mapped + offset + kDrawSliceBytes,
                             offset + kDrawSliceBytes};
    work->draw.multi_rate_upload={};work->draw.multi_rate_upload_capacity=0;
    if (work->draw.multi_rate_input) {
      const auto bytes=NativeMultiRateUploadBytes(*work->draw.multi_rate_input);
      work->draw.multi_rate_upload={batch->multi_rate_arena->buffer.get(),
          batch->multi_rate_arena->mapped+multi_offset,multi_offset};
      work->draw.multi_rate_upload_capacity=bytes;
      multi_offset+=(u64(bytes)+255)&~u64{255};
    }
    work->follower_slot = slot;
    slot += u32(work->followers.size());
  }
  const size_t chunk = PrepareChunkSize();
  const bool per_chunk = ChunkDependencyWaitsEnabled();
  // A chunk holds `chunk` draws' worth of preparation; a follower weighs an eighth of a draw.
  const auto chunk_end = [&](size_t i) {
    size_t end = i, load = 0;
    while (end < batch->works.size() && load < chunk * 8 && !batch->works[end]->deferred)
      load += 8 + batch->works[end++]->followers.size();
    return end;
  };
  // Sized before any chunk is posted: workers write their slot concurrently.
  size_t chunks = 0;
  for (size_t i = 0; i < batch->works.size();) {
    if (batch->works[i]->deferred) { ++i; continue; }
    ++chunks;
    i = chunk_end(i);
  }
  batch->chunk_leases.resize(chunks);
  batch->jobs.reserve(chunks);
  for (size_t i = 0; i < batch->works.size();) {
    if (batch->works[i]->deferred) { ++i; continue; }
    const size_t end = chunk_end(i);
    const u32 index = u32(batch->jobs.size());
    PreparationDependencyFilter dependencies;
    for (size_t k = i; k < end; ++k) {
      batch->works[k]->chunk = index;
      if (per_chunk) {
        dependencies.AddBindings(batch->works[k]->bindings);
        for (const auto& alias : batch->works[k]->preparation.color_aliases)
          for (const auto address : alias.source_dependencies)
            if (address) dependencies.Add(address); else dependencies.overflow = true;
        AddFullDrawPhysicalDependencies(dependencies.physical, *batch->works[k]);
      }
    }
    batch->pending_jobs.fetch_add(1, std::memory_order_relaxed);
    // Weak: the recorder's task keeps the batch alive until every chunk is replayed,
    // and a strong capture here would cycle through the task's shared state.
    auto task = std::make_shared<PreparationChunkTask>();
    task->run = [weak = std::weak_ptr<FullDrawBatch>(batch), i, end, index] {
      if (const auto batch = weak.lock()) PrepareFullDrawChunk(*batch, i, end, index);
    };
    auto ready = task->done.get_future().share();
    batch->jobs.push_back({std::move(ready), dependencies, task});
    PostNativePreparationDetached([task] { task->TryRun(); }, (end - i) * sizeof(FullDrawWork));
    i = end;
  }
}

// Recorder: plays the tapes in capture order, waiting per chunk for preparation.
void ReplayFullDrawBatch(const std::shared_ptr<FullDrawBatch>& batch) {
  // Preparation needs no replay lock: wait for every chunk first, so the lock is held for the
  // replay alone and resolves on the coordinator no longer queue behind the workers.
  if (ReplayAfterPreparationEnabled())
    for (const auto& job : batch->jobs) job.ready.wait();
  NativeReplayLock replay;
  auto* commands = HostDevice::BeginFrameCommands();
  const auto view = HostDevice::CurrentDrawDeviceView();
  const bool usable = commands && view.commands == commands && view.recording_serial;
  std::exception_ptr failure;
  u32 waited = ~0u;
  DrawResourceLease::Builder lease;
  if (batch->arena) lease.Add(batch->arena);
  if (batch->multi_rate_arena) lease.Add(batch->multi_rate_arena);
  const bool reclaim = FullDrawReclaimEnabled();
  // State applied by earlier tapes is reused only inside this batch.
  if (usable) ResetNativeReplayState(commands);
  for (const auto& owner : batch->works) {
    auto& work = *owner;
    if (work.chunk != ~0u && work.chunk != waited) {
      try { batch->jobs[work.chunk].ready.get(); } catch (...) { if (!failure) failure = std::current_exception(); }
      waited = work.chunk;
    }
    if (usable) {
      work.uploads->Replay(commands);
      work.commands->Replay(commands);
    }
    if (work.replay_task) {
      work.replay_task();
      if (usable) ResetNativeReplayState(commands);
    }
    if (!usable || !work.submitted) FailActiveQueries();
    const bool leased = work.chunk < batch->chunk_leases.size() && batch->chunk_leases[work.chunk];
    if (!leased) TakeDrawOwners(lease, work);
    for (const u32 index : work.preparation.unregister) HostDevice::UnregisterTexture(index);
    // The lease owns what the GPU needs; the work must not keep records or tapes alive.
    DrawTiming timing = work.timing;
    const bool deferred = work.deferred;
    // A deferred task's captures may hold texture records: they stay released on the recorder.
    if (deferred || !reclaim) work.Reset();
    if (deferred) continue;
    g_draw_timing.calls += timing.calls;
    g_draw_timing.pipeline_creations += timing.pipeline_creations;
    g_draw_timing.pipeline_creation_ms += timing.pipeline_creation_ms;
    for (size_t i = 0; i < g_draw_timing.stages_ms.size(); ++i) g_draw_timing.stages_ms[i] += timing.stages_ms[i];
    for (size_t i = 0; i < g_draw_timing.constants_ms.size(); ++i) g_draw_timing.constants_ms[i] += timing.constants_ms[i];
  }
  if (usable) ResetNativeReplayState(commands);
  HostDevice::RetireResource(lease.Finish());
  for (auto& chunk_lease : batch->chunk_leases)
    if (chunk_lease) HostDevice::RetireResource(std::move(chunk_lease));
  // Only clearing is left: draw works release nothing the GPU still needs.
  if (reclaim && !FullDrawReclaimer::Instance().Post(batch))
    for (const auto& owner : batch->works) owner->Reset();
  if (failure) std::rethrow_exception(failure);
}

// Coordinator, in guest order: the range-dependent half of a capture (buffer inputs, index window
// and bounds, then versions and windows), called under a ShaderBindingsScope for work.shaders.
void CaptureDrawGeometry(ParallelDrawWork& work) {
  std::array<bool, kNativeVertexStreams> streams{};
  for (const auto& element : work.elements)
    if (element.slotIndex < streams.size()) streams[element.slotIndex] = true;
  for (u32 i = 0; i < kNativeVertexStreams; ++i)
    if (streams[i]) work.vertex_inputs[i] = CaptureBufferPreparation(work.bindings.vertex_streams[i].buffer, BufferKind::kVertex);
  if (work.indexed) {
    work.index_input = CaptureBufferPreparation(work.bindings.index_buffer, BufferKind::kIndex);
    const auto& index = work.index_input.metadata;
    work.index_window = DrawIndexWindow(work.start, work.count, index.guest_format == 1 ? 2 : 4, index.length);
    if (index.mirror_address && work.index_window)
      work.index_bounds = CaptureIndexBounds(work.index_input, work.index_window.offset, work.index_window.length);
  }
  if (MultiRateFetchShader(BoundShaderHash(ShaderStage::kVertex)) && MultiRateListPrimitive(work.primitive)) {
    work.multi_rate_capture_attempted=true;work.multi_rate_input.reset();
    CaptureNativeMultiRateInput(work.device.get(),work.bindings,work.declaration,work.index_input.metadata,
        work.indexed,work.start,work.count,work.base_vertex,
        work.primitive==u32(rex::graphics::xenos::PrimitiveType::kQuadList),work.multi_rate_input);
  }
  // Versions and windows read guest memory, so they are chosen here, in order.
  work.geometry_valid = PrepareDrawGeometry(work, true);
}

// Everything CaptureDrawGeometry and both geometry planners decide, as a fresh capture starts.
void ClearDrawGeometry(ParallelDrawWork& draw) {
  draw.vertex_inputs = {}; draw.index_input = {}; draw.index_window = {}; draw.index_bounds.reset();
  draw.layout.reset(); draw.active_streams = {}; draw.windows = {}; draw.vertices = {}; draw.indices = {};
  draw.use_windows = draw.geometry_ready = draw.geometry_valid = false;
  draw.min_index = 0;
  draw.multi_rate_input.reset();draw.multi_rate_capture_attempted=false;
  draw.multi_rate_upload={};draw.multi_rate_upload_capacity=0;
}

bool CaptureOrderedDraw(ParallelDrawWork& work, const DrawBindings& captured, u32 primitive,
    bool indexed, u32 start, u32 count, i32 base_vertex) {
  if (!count || TemporalInputsCandidate() || LongProbeEnabled() || pass_profile::Get().capturing) return false;
  work.bindings = captured;
  work.bindings.vertex_declaration = work.device->vertex_declaration;
  work.primitive = primitive; work.indexed = indexed; work.start = start;
  work.count = count; work.base_vertex = base_vertex;
  {
    // Placement declarations are rehashed on every resolve; once per frame is enough here.
    struct Memo { u32 frame = ~0u; VertexDeclarationView view; std::shared_ptr<void> owner; };
    static thread_local std::unordered_map<u32, Memo> memos;
    const u32 frame = g_probe_frame.load(std::memory_order_relaxed);
    if (memos.size() > 4096) memos.clear();
    auto& memo = memos[work.bindings.vertex_declaration];
    if (memo.frame != frame) {
      memo.view = ResolveVertexDeclaration(work.bindings.vertex_declaration);
      memo.owner = FindDeclarationOwner(work.bindings.vertex_declaration);
      memo.frame = frame;
    }
    work.declaration = memo.view;
  }
  if (!work.declaration.supported || !work.declaration.elements) return false;
  work.elements.assign(work.declaration.elements, work.declaration.elements + work.declaration.element_count);
  if (work.declaration.guest_types)
    work.guest_types.assign(work.declaration.guest_types, work.declaration.guest_types + work.declaration.element_count);
  work.declaration.elements = work.elements.data();
  work.declaration.guest_types = work.guest_types.empty() ? nullptr : work.guest_types.data();
  work.raster_extent = NativeTextureExtent(work.bindings.render_targets[0] ?
      work.bindings.render_targets[0] : work.bindings.depth_stencil);
  if (!work.raster_extent.width || !work.raster_extent.height) return false;
  for (u32 i = 0; i < 4; ++i) work.output_scales[i] = SurfaceColorOutputScale(work.bindings.render_targets[i]);
  ShaderBindingsScope shaders(work.shaders);
  const u32 mask = BoundShaderTextureMask(ShaderStage::kVertex) | BoundShaderTextureMask(ShaderStage::kPixel);
  auto& pinned = CurrentDrawPreparation()->pinned;
  for (u32 i = 0; i < kNativeTextureSlots; ++i) {
    if (!(mask & (1u << i)) || !work.bindings.textures[i]) continue;
    // One lookup serves both steps when the header is unchanged, the common case per draw.
    auto resolved = RefreshTextureHeaderOwner(work.bindings.textures[i]);
    work.texture_inputs[i] = CaptureTexturePreparation(work.bindings.textures[i], std::move(resolved));
    if (work.texture_inputs[i].owner)
      pinned.push_back({kPinTexture, work.bindings.textures[i], work.texture_inputs[i].owner});
  }
  CaptureDrawGeometry(work);
  return true;
}

bool RangeRepeatEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_RANGE_REPEAT");
    return !value || value[0] != '0';
  }();
  return enabled;
}

bool RepeatCaptureEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_REPEAT_CAPTURE");
    return !value || value[0] != '0';
  }();
  return enabled;
}

// Everything a capture's decisions depend on besides its own bindings and the guest headers.
struct CaptureStamps {
  u64 batch = ~u64{0}, tasks = 0, epoch = 0, footprint = 0, textures = 0, buffers = 0, declarations = 0, shaders = 0;
  bool operator==(const CaptureStamps&) const = default;
};
CaptureStamps CurrentCaptureStamps() {
  return {g_full_draw_batches, NativeNonCaptureTasks(), CpuMemoryWriteEpoch(), TextureFootprintEpoch(),
          TextureRegistryGeneration(), BufferRegistryGeneration(), DeclarationRegistryGeneration(),
          ShaderRegistryGeneration()};
}

// The previous draw's capture, kept on the coordinator before any worker touched it, whose
// decisions a draw with the same inputs and nothing changed since copies.
struct CaptureTemplate {
  const FullDrawWork* work = nullptr;
  CaptureStamps stamps;
  DrawBindings key;
  std::array<u32, 2> shaders{};
  std::array<u32, 5> range{};
  ShaderBindingsSnapshot snapshot;
  std::vector<DrawPreparationContext::PinnedOwner> pinned;
  ParallelDrawWork draw;
};
thread_local CaptureTemplate t_capture_template;

// The copied declaration points into the copied element vectors, not the template's.
void RepointDeclaration(ParallelDrawWork& draw, const ParallelDrawWork& from) {
  draw.declaration.elements = draw.elements.data();
  draw.declaration.guest_types = from.declaration.guest_types ? draw.guest_types.data() : nullptr;
}

u32 DrawMergeRun() {
  static const u32 run = [] {
    const char* value = std::getenv("LEGO_NATIVE_DRAW_MERGE_RUN");
    const int parsed = value ? std::atoi(value) : 63;
    return u32(std::clamp(parsed, 0, 1023));
  }();
  return run;
}

// Per-draw traces and probes expect to see every draw.
bool DrawMergeBlocked() {
  static const bool traces = std::getenv("LEGO_NATIVE_MESH_TRACE") || std::getenv("LEGO_GPU_SNAPSHOT_DIR");
  return traces || g_logo_capture_api || PortraitProbeEnabled();
}

void MarkDrawWrites(const u8* bytes, const DrawBindings& bindings) {
  for (u32 i = 0; i < kNativeRenderTargets; ++i)
    if (u32(*reinterpret_cast<const be_u32*>(bytes + 12292 + 4 * i)) & 15u) MarkSurfaceWritten(bindings.render_targets[i], true);
  if (bindings.depth_stencil &&
      DecodeDepthState(u32(*reinterpret_cast<const be_u32*>(bytes + 10548)), true).write)
    MarkSurfaceWritten(bindings.depth_stencil, true);
}

// Whether the guest headers of the template's captured textures still read as captured.
bool TemplateHeadersRepeat(const CaptureTemplate& capture_template, const std::array<u32, kNativeTextureSlots>& textures) {
  bool repeat = true;
  for (u32 i = 0; repeat && i < kNativeTextureSlots; ++i) {
    const auto& input = capture_template.draw.texture_inputs[i];
    if (!input.owner) continue;
    D3DTexture metadata{};
    if (!CopyNativeGuestMetadata(*REX_KERNEL_MEMORY(),textures[i],&metadata,sizeof(metadata))) return false;
    const auto* header = &metadata;
    std::array<u32, 7> words;
    words[0] = u32(header->resource.common) & 0x1Fu;
    for (u32 w = 0; w < 6; ++w) words[w + 1] = header->format.dword[w];
    repeat = words == input.header;
  }
  return repeat;
}

// The repeat conditions a chained draw's device bytes do not settle: streams, range, stamps and guest headers.
bool ChainedDrawRepeats(const CaptureTemplate& capture_template, const FullDrawWork& head,
    const std::array<VertexStreamBinding, kNativeVertexStreams>& streams, const std::array<u32, 5>& range) {
  return std::memcmp(capture_template.key.vertex_streams.data(), streams.data(), sizeof(streams)) == 0 &&
      capture_template.range == range && capture_template.stamps == CurrentCaptureStamps() &&
      TemplateHeadersRepeat(capture_template, head.bindings.textures);
}

bool HeadDrawsRange(const FullDrawWork& head, const std::array<u32, 5>& range) {
  return head.primitive == range[0] && u32(head.indexed) == range[1] && head.start == range[2] &&
      head.count == range[3] && u32(head.base_vertex) == range[4];
}

void CaptureFullDraw(DrawSnapshot drawn, u32 primitive, bool indexed, u32 start, u32 count, i32 base_vertex) {
  const u64 snapshot = drawn.sequence, same_as = drawn.same_as;
  const auto streams = SnapshotDrawBindings().vertex_streams;
  const std::array<u32, 5> range{primitive, u32(indexed), start, count, u32(base_vertex)};
  auto& capture_template = t_capture_template;
  std::optional<DepthAliasDecision> decided_alias;
  // A draw chained to the template's run equals the head outside the vertex constants, so it joins
  // on the head's device bytes and bindings without a full snapshot.
  if (same_as && ChainedMergeEnabled() && DrawMergeEnabled() && RepeatCaptureEnabled() &&
      !g_full_draw_batch.empty() &&
      g_full_draw_batch.back().get() == capture_template.work && g_full_draw_batch.back()->last_snapshot == same_as &&
      g_full_draw_batch.back()->followers.size() < DrawMergeRun() && !DrawMergeBlocked()) {
    auto& head = *g_full_draw_batch.back();
    if (HeadDrawsRange(head, range) && ChainedDrawRepeats(capture_template, head, streams, range)) {
      auto alias = DecideSurfaceDepthAlias(head.bindings.depth_stencil);
      if (!alias.source && !alias.destination) {
        MarkDrawWrites(reinterpret_cast<const u8*>(head.device.get()), head.bindings);
        AddFollower(head, std::move(drawn));
        head.last_snapshot = snapshot;
        if (++g_full_draw_batch_draws >= kMergedBatchDraws) FlushFullDrawBatch();
        return;
      }
      // Decided once; the full path takes it as an alias refusal.
      decided_alias = std::move(alias);
    }
  }
  auto device = SnapshotFullDevice(drawn);
  auto bindings = ReadDeviceBindings(device.get());
  bindings.vertex_streams = streams;
  auto key = bindings;
  key.vertex_declaration = device->vertex_declaration;
  const std::array<u32, 2> shader_pair{device->vertex_shader, device->pixel_shader};
  bool repeat = false;
  // A draw of another range with the same primitive reuses every input decision and captures only its geometry.
  bool range_repeat = false;
  if (RepeatCaptureEnabled() && capture_template.work && !g_full_draw_batch.empty() &&
      g_full_draw_batch.back().get() == capture_template.work &&
      std::memcmp(&capture_template.key, &key, sizeof(key)) == 0 &&
      capture_template.shaders == shader_pair && capture_template.stamps == CurrentCaptureStamps()) {
    if (capture_template.range == range)
      repeat = TemplateHeadersRepeat(capture_template, bindings.textures);
    else if (RangeRepeatEnabled() && count && capture_template.range[0] == range[0] && capture_template.range[1] == range[1] &&
             !TemporalInputsCandidate() && !LongProbeEnabled() && !pass_profile::Get().capturing)
      range_repeat = TemplateHeadersRepeat(capture_template, bindings.textures);
  }
  // A same-range template may still have a different index clamp.
  // Recapture instead of dropping the new draw or reusing old gathered bytes.
  if (repeat && capture_template.draw.multi_rate_input &&
      !MatchesNativeMultiRateInput(*capture_template.draw.multi_rate_input,device.get(),
          capture_template.draw.declaration,indexed,start,count,base_vertex,
          primitive==u32(rex::graphics::xenos::PrimitiveType::kQuadList))) repeat=false;
  // A repeat whose snapshot equals the run head's outside the vertex constants joins the head: same
  // captured decisions, same device state, nothing between them on this thread.
  if (repeat && same_as && DrawMergeEnabled()) {
    auto& head = *g_full_draw_batch.back();
    if (head.last_snapshot == same_as && head.followers.size() < DrawMergeRun() && !DrawMergeBlocked() &&
        !decided_alias && HeadDrawsRange(head, range)) {
      auto alias = DecideSurfaceDepthAlias(bindings.depth_stencil);
      if (!alias.source && !alias.destination) {
        MarkDrawWrites(reinterpret_cast<const u8*>(device.get()), bindings);
        device.reset();
        AddFollower(head, std::move(drawn));
        head.last_snapshot = snapshot;
        if (++g_full_draw_batch_draws >= kMergedBatchDraws) FlushFullDrawBatch();
        return;
      }
      decided_alias = std::move(alias);
    }
  }
  auto work = t_full_draw_pool.Acquire();
  work->clean = false;
  work->device = std::move(device); work->bindings = bindings;
  work->primitive = primitive; work->indexed = indexed; work->start = start;
  work->count = count; work->base_vertex = base_vertex; work->sequence = ++g_full_draw_sequence;
  {
    static thread_local DrawDeviceView immutable;
    if (!immutable.device || (work->sequence & 63) == 0) immutable = HostDevice::ImmutableDrawDeviceView();
    auto view = immutable;
    view.commands = work->commands.get();
    view.recording_serial = work->sequence;
    work->preparation.view = view;
    work->preparation.uploads = work->uploads.get();
    work->preparation.allocation = {};
    work->preparation.capacity = work->preparation.used = 0;
  }
  {
    // Resolve every bound record now, in capture order, so a later release or
    // re-placement at the same address cannot change what this draw prepares.
    DrawPreparationScope scope(work->preparation);
    // Passes alternate between a handful of programs; keep the recent pairs.
    struct ShaderPair { u32 frame = ~u32{0}; ShaderBindingsSnapshot shaders; };
    static thread_local std::unordered_map<u64, ShaderPair> captured_pairs;
    const ShaderBindingsSnapshot* captured_shaders = nullptr;
    // A reused capture takes the template's pins and shaders; the lookups run only for a full capture.
    if (!(repeat || range_repeat)) {
      auto& pinned = work->preparation.pinned;
      const auto pin = [&](unsigned char kind, u32 address, std::shared_ptr<void> owner) {
        if (address && owner) pinned.push_back({kind, address, std::move(owner)});
      };
      for (const u32 target : bindings.render_targets) if (target) pin(kPinTexture, target, FindTextureOwner(target));
      if (bindings.depth_stencil) pin(kPinTexture, bindings.depth_stencil, FindTextureOwner(bindings.depth_stencil));
      for (const auto& stream : bindings.vertex_streams) if (stream.buffer) pin(kPinBuffer, stream.buffer, FindBufferOwner(stream.buffer));
      if (indexed && bindings.index_buffer) pin(kPinBuffer, bindings.index_buffer, FindBufferOwner(bindings.index_buffer));
      if (work->device->vertex_declaration)
        pin(kPinDeclaration, work->device->vertex_declaration, FindDeclarationOwner(work->device->vertex_declaration));
      // Placement programs are revalidated once per presented frame.
      const u32 frame = g_probe_frame.load(std::memory_order_relaxed);
      const u32 vertex = work->device->vertex_shader, pixel = work->device->pixel_shader;
      if (captured_pairs.size() > 4096) captured_pairs.clear();
      auto& found = captured_pairs[(u64(vertex) << 32) | pixel];
      if (found.frame != frame) {
        found.shaders = CaptureShaderBindings(vertex, pixel);
        found.frame = frame;
      }
      captured_shaders = &found.shaders;
    }
    if (repeat || range_repeat) {
      // Pins and shaders exactly as the template's capture took them.
      work->preparation.pinned = capture_template.pinned;
      work->shaders = capture_template.snapshot;
      work->draw = capture_template.draw;
      RepointDeclaration(work->draw, capture_template.draw);
      work->draw.device = work->device;
      work->draw.shaders = work->shaders;
      work->captured = true;
      if (range_repeat) {
        work->draw.start = start; work->draw.count = count; work->draw.base_vertex = base_vertex;
        ClearDrawGeometry(work->draw);
        ShaderBindingsScope shaders(work->draw.shaders);
        CaptureDrawGeometry(work->draw);
      }
    } else {
      work->shaders = *captured_shaders;
      work->draw.device = work->device;
      work->draw.shaders = work->shaders;
      work->captured = CaptureOrderedDraw(work->draw, bindings, primitive, indexed, start, count, base_vertex);
    }
  }
  // Diagnostic/legacy capture can decline the general prepared path;
  // this input family still snapshots geometry here, before workers run.
  if (!work->draw.multi_rate_capture_attempted) {
    ShaderBindingsScope shaders(work->shaders);
    if (MultiRateFetchShader(BoundShaderHash(ShaderStage::kVertex)) && MultiRateListPrimitive(primitive)) {
      if (!work->captured) {
        auto& draw=work->draw;draw.device=work->device;draw.bindings=bindings;draw.shaders=work->shaders;
        draw.primitive=primitive;draw.indexed=indexed;draw.start=start;draw.count=count;draw.base_vertex=base_vertex;
        draw.declaration=ResolveVertexDeclaration(work->device->vertex_declaration);
        if (draw.declaration.supported && draw.declaration.elements) {
          draw.elements.assign(draw.declaration.elements,draw.declaration.elements+draw.declaration.element_count);
          if (draw.declaration.guest_types)
            draw.guest_types.assign(draw.declaration.guest_types,draw.declaration.guest_types+draw.declaration.element_count);
          RepointDeclaration(draw,draw);
        }
      }
      if (work->draw.declaration.supported && work->draw.declaration.elements) CaptureDrawGeometry(work->draw);
      else work->draw.multi_rate_capture_attempted=true;
    }
  }
  // A capture qualifies as a template when it recorded nothing of its own, retired nothing, and every
  // buffer it reads is held by an armed write watch.
  bool qualifies = RepeatCaptureEnabled() && work->captured && !repeat && !range_repeat;
  if (qualifies) {
    bool failed = !work->commands->Empty() || !work->uploads->Empty() ||
        !work->preparation.retired.empty() || !work->preparation.unregister.empty();
    // A windowed index range is covered by the bounds snapshot's own armed watch over exactly that range;
    // a whole-buffer index upload needs the buffer's watch.
    if (!failed && work->draw.indexed)
      failed = work->draw.use_windows
          ? !work->draw.index_bounds || work->draw.index_bounds->stamp.pages.empty() ||
                !CpuMemoryUnchanged(work->draw.index_bounds->stamp)
          : !BufferInputWatched(work->draw.index_input.owner);
    // A windowed stream reads its window's copy, which the window cache trusts only under an armed watch.
    for (u32 i = 0; !failed && i < kNativeVertexStreams; ++i) {
      // Every stream with bound storage is checked, whatever the layout marks active.
      if (!work->draw.vertices[i].view.buffer && !work->draw.vertex_inputs[i].owner) continue;
      const auto& window = work->draw.windows[i];
      if (work->draw.use_windows && window.length
              ? !BufferWindowWatched(work->draw.vertex_inputs[i].owner, window.offset, window.length,
                    work->draw.vertices[i].view.buffer)
              : !BufferInputWatched(work->draw.vertex_inputs[i].owner)) failed = true;
    }
    qualifies = !failed;
  }
  if (qualifies) {
    capture_template.work = work.get();
    capture_template.stamps = CurrentCaptureStamps();
    capture_template.key = key;
    capture_template.shaders = shader_pair;
    capture_template.range = range;
    capture_template.snapshot = work->shaders;
    capture_template.pinned = work->preparation.pinned;
    capture_template.draw = work->draw;
    RepointDeclaration(capture_template.draw, work->draw);
    capture_template.draw.device.reset();
    capture_template.draw.shaders = {};
  } else if (repeat || range_repeat) {
    capture_template.work = work.get();
  } else {
    capture_template.work = nullptr;
  }
  // Alias restores and write marks order passes against each other, so they
  // are settled here in capture order; the transfer itself records at preparation.
  const auto* bytes = reinterpret_cast<const u8*>(work->device.get());
  {
    static thread_local std::array<u32, kNativeTextureSlots + kNativeRenderTargets + 1> added{};
    static thread_local u64 added_batch = ~u64{0};
    std::array<u32, kNativeTextureSlots + kNativeRenderTargets + 1> addresses;
    std::copy(bindings.textures.begin(), bindings.textures.end(), addresses.begin());
    std::copy(bindings.render_targets.begin(), bindings.render_targets.end(),
        addresses.begin() + kNativeTextureSlots);
    addresses.back() = bindings.depth_stencil;
    if (added_batch != g_full_draw_batches || added != addresses) {
      g_full_draw_batch_bloom.AddBindings(bindings);
      added = addresses;
      added_batch = g_full_draw_batches;
    }
  }
  // A placement may move without changing its header address. Use the
  // immutable capture, even when the address-only memo still matches.
  work->preparation.color_aliases = DecideSurfaceColorAliases(bindings.render_targets);
  for (const auto& alias : work->preparation.color_aliases)
    for (const auto address : alias.source_dependencies)
      if (address) g_full_draw_batch_bloom.Add(address); else g_full_draw_batch_bloom.overflow = true;
  AddFullDrawPhysicalDependencies(g_full_draw_batch_bloom.physical, *work);
  work->preparation.alias = decided_alias ? std::move(*decided_alias) : DecideSurfaceDepthAlias(bindings.depth_stencil);
  work->preparation.alias_decided = true;
  MarkDrawWrites(bytes, bindings);
  work->preparation.writes_marked = true;
  work->last_snapshot = snapshot;
  const u32 limit = FullDrawBatchLimit().load(std::memory_order_relaxed);
  if (g_full_draw_batch.empty()) g_full_draw_batch.reserve(limit);
  g_full_draw_batch.push_back(std::move(work));
  g_draw_batch_pending.store(true, std::memory_order_release);
  if (g_full_draw_batch.size() >= limit || ++g_full_draw_batch_draws >= kMergedBatchDraws) FlushFullDrawBatch();
  t_full_draw_pool.PrefetchNext();
}

void QueueNativeDrawBatch() {
  if (g_draw_batch.empty()) return;
  auto batch = std::make_shared<std::vector<std::shared_ptr<ParallelDrawWork>>>(std::move(g_draw_batch));
  g_draw_batch.clear();
  const auto allocation = HostDevice::AllocateDrawUpload(u32(batch->size()) * 8960);
  const auto serial = HostDevice::CurrentDrawDeviceView().recording_serial;
  std::vector<std::future<void>> jobs;
  jobs.reserve(NativePreparationWorkerCount());
  if (allocation && serial) {
    for (size_t i = 0; i < batch->size(); ++i) {
      auto& work = *(*batch)[i];
      work.serial = serial; work.allocation = allocation;
      work.allocation.offset += i * 8960;
      work.allocation.mapped = static_cast<u8*>(allocation.mapped) + i * 8960;
    }
    const size_t chunk = std::max<size_t>(1, (batch->size() + NativePreparationWorkerCount() - 1) / NativePreparationWorkerCount());
    for (size_t first = 0; first < batch->size(); first += chunk) {
      const auto end = std::min(batch->size(), first + chunk);
      jobs.push_back(PostNativePreparation([batch, first, end] {
        for (size_t i = first; i < end; ++i) PrepareDrawWork(*(*batch)[i]);
        _mm_sfence();
      }, (end - first) * (sizeof(ParallelDrawWork) + sizeof(D3DDevice))));
    }
  }
  g_pending_draw_batches.push_back({std::move(batch), std::move(jobs)});
}

bool CaptureParallelDraw(std::shared_ptr<D3DDevice> device, u32 primitive,
    bool indexed, u32 start, u32 count, i32 base_vertex) {
  if (!NativeParallelDrawsEnabled() || !device || !count || TemporalInputsCandidate() ||
      LongProbeEnabled() || pass_profile::Get().capturing) return false;
  if (SnapshotStateBindingsEnabled()) PublishDeviceBindings(ReadDeviceBindings(device.get()));
  auto work = AllocateParallelDrawWork();
  work->device = std::move(device);
  work->bindings = SnapshotDrawBindings();
  work->bindings.vertex_declaration = work->device->vertex_declaration;
  work->shaders = CaptureShaderBindings();
  work->primitive = primitive; work->indexed = indexed; work->start = start;
  work->count = count; work->base_vertex = base_vertex;
  work->declaration = ResolveVertexDeclaration(work->bindings.vertex_declaration);
  if (!work->declaration.supported || !work->declaration.elements) return false;
  work->elements.assign(work->declaration.elements, work->declaration.elements + work->declaration.element_count);
  if (work->declaration.guest_types)
    work->guest_types.assign(work->declaration.guest_types, work->declaration.guest_types + work->declaration.element_count);
  work->declaration.elements = work->elements.data();
  work->declaration.guest_types = work->guest_types.empty() ? nullptr : work->guest_types.data();
  work->raster_extent = NativeTextureExtent(work->bindings.render_targets[0] ?
      work->bindings.render_targets[0] : work->bindings.depth_stencil);
  if (!work->raster_extent.width || !work->raster_extent.height) return false;
  for (u32 i = 0; i < 4; ++i) work->output_scales[i] = SurfaceColorOutputScale(work->bindings.render_targets[i]);
  ShaderBindingsScope shaders(work->shaders);
  u32 mask = BoundShaderTextureMask(ShaderStage::kVertex) | BoundShaderTextureMask(ShaderStage::kPixel);
  for (u32 i = 0; i < kNativeTextureSlots; ++i)
    if (mask & (1u << i)) work->texture_inputs[i] = CaptureTexturePreparation(work->bindings.textures[i]);
  std::array<bool, kNativeVertexStreams> streams{};
  for (const auto& element : work->elements)
    if (element.slotIndex < streams.size()) streams[element.slotIndex] = true;
  for (u32 i = 0; i < kNativeVertexStreams; ++i)
    if (streams[i]) work->vertex_inputs[i] = CaptureBufferPreparation(work->bindings.vertex_streams[i].buffer, BufferKind::kVertex);
  if (indexed) {
    work->index_input = CaptureBufferPreparation(work->bindings.index_buffer, BufferKind::kIndex);
    const auto& index = work->index_input.metadata;
    work->index_window = DrawIndexWindow(start, count, index.guest_format == 1 ? 2 : 4, index.length);
    if (index.mirror_address && work->index_window)
      work->index_bounds = CaptureIndexBounds(work->index_input, work->index_window.offset, work->index_window.length);
  }
  if (MultiRateFetchShader(BoundShaderHash(ShaderStage::kVertex)) && MultiRateListPrimitive(primitive))
    CaptureDrawGeometry(*work);
  if (g_draw_batch.empty()) g_draw_batch.reserve(32);
  g_draw_batch.push_back(std::move(work));
  g_draw_batch_pending.store(true, std::memory_order_release);
  if (g_draw_batch.size() == 32) {
    QueueNativeDrawBatch();
    if (!OverlapDrawBatches() || g_pending_draw_batches.size() == 4) FlushNativeDrawBatch();
  }
  return true;
}

u32 DrawVerticesHook(D3DDevice* device, u32 primitive_type, u32 start_vertex,
                     u32 vertex_count) {
  auto recording = HostDevice::LockRecording();
  if (device && SnapshotStateBindingsEnabled()) PublishDeviceBindings(ReadDeviceBindings(device));
  if (ConsumeCpuPoolCopyDraw(primitive_type, start_vertex, vertex_count)) return 0;
  PassProfileScope profile("draw", device);
  if (!DispatchDraw(device, primitive_type, false, start_vertex, vertex_count, 0))
    FailActiveQueries();
  return 0;
}

u32 DrawIndexedVerticesHook(D3DDevice* device, u32 primitive_type,
                            u32 base_vertex, u32 start_index, u32 index_count) {
  auto recording = HostDevice::LockRecording();
  if (device && SnapshotStateBindingsEnabled()) PublishDeviceBindings(ReadDeviceBindings(device));
  PassProfileScope profile("drawidx", device);
  if (!DispatchDraw(device, primitive_type, true, start_index, index_count,
                    static_cast<i32>(base_vertex))) FailActiveQueries();
  return 0;
}

// Threaded recording entry points (LEGO_NATIVE_THREADED=1); otherwise the
// original hooks above run inline.
u32 DrawVerticesHookQueued(D3DDevice* device, u32 primitive_type, u32 start_vertex,
                           u32 vertex_count) {
  if (ConsumeCpuPoolCopyDraw(primitive_type, start_vertex, vertex_count)) return 0;
  if (!device || !NativeWorkerShouldQueue())
    return DrawVerticesHook(device, primitive_type, start_vertex, vertex_count);
  NativeWorkerSubmitCapture([snapshot = SnapshotDrawDevice(device), primitive_type, start_vertex, vertex_count]() mutable {
    if (NativeFullDrawPreparationEnabled() && !NativeFsr3Enabled()) {
      CaptureFullDraw(std::move(snapshot), primitive_type, false, start_vertex, vertex_count, 0);
      return;
    }
    const auto copy = SnapshotFullDevice(snapshot);
    if (NativeFsr3Enabled()) {
      // Full-preparation SetShader deliberately skips global binding. This
      // ordered temporal draw still needs the shaders from its own snapshot.
      const auto programs = CaptureShaderBindings(copy->vertex_shader, copy->pixel_shader);
      ShaderBindingsScope shaders(programs);
      DrawVerticesHook(copy.get(), primitive_type, start_vertex, vertex_count);
      return;
    }
    if (NativeParallelDrawsEnabled()) {
      if (ConsumeCpuPoolCopyDraw(primitive_type, start_vertex, vertex_count)) return;
      if (CaptureParallelDraw(copy, primitive_type, false, start_vertex, vertex_count, 0)) return;
    }
    FlushNativeDrawBatch();
    DrawVerticesHook(copy.get(), primitive_type, start_vertex, vertex_count);
  });
  return 0;
}

u32 DrawIndexedVerticesHookQueued(D3DDevice* device, u32 primitive_type,
                                  u32 base_vertex, u32 start_index, u32 index_count) {
  if (!device || !NativeWorkerShouldQueue())
    return DrawIndexedVerticesHook(device, primitive_type, base_vertex, start_index, index_count);
  NativeWorkerSubmitCapture([snapshot = SnapshotDrawDevice(device), primitive_type, base_vertex, start_index,
                      index_count]() mutable {
    if (NativeFullDrawPreparationEnabled() && !NativeFsr3Enabled()) {
      CaptureFullDraw(std::move(snapshot), primitive_type, true, start_index, index_count, static_cast<i32>(base_vertex));
      return;
    }
    const auto copy = SnapshotFullDevice(snapshot);
    if (NativeFsr3Enabled()) {
      const auto programs = CaptureShaderBindings(copy->vertex_shader, copy->pixel_shader);
      ShaderBindingsScope shaders(programs);
      DrawIndexedVerticesHook(copy.get(), primitive_type, base_vertex, start_index, index_count);
      return;
    }
    if (CaptureParallelDraw(copy, primitive_type, true, start_index, index_count, static_cast<i32>(base_vertex))) return;
    FlushNativeDrawBatch();
    DrawIndexedVerticesHook(copy.get(), primitive_type, base_vertex, start_index, index_count);
  });
  return 0;
}

bool ReadResolveWords(u32 address, i32* output, u32 count) {
  if (!address || !output || !count || count > 4 || u64(address) + count * 4 > 0x100000000ull) return false;
  auto* memory = REX_KERNEL_MEMORY();
  std::array<be_u32,4> words{};
  if (!CopyNativeGuestMetadata(*memory,address,words.data(),count*4)) return false;
  for (u32 i = 0; i < count; ++i) output[i] = i32(u32(words[i]));
  return true;
}

// Guest words a resolve needs, read on the guest thread before it returns.
struct ResolveRequest {
  u32 flags, source_rect, destination_texture, destination_point, destination_level,
      destination_slice, clear_color, lr, caller_lr;
  f64 clear_z;
  ResolveRect rect;
  ResolvePoint point;
  float clear_rgba[4];
  bool clear_readable;
  DrawBindings bindings{};
  bool snapshot = false;
};
std::atomic<bool> g_resolve_trace_full{false};

u32 ResolveRecorded(const ResolveRequest& r) {
  // Before the registry lock: the awaited workers need it.
  WaitPendingPreparationFor({r.destination_texture});
  auto recording = HostDevice::LockRecording();
  if(!RetryNativeCompositionTargets()) return 0x80004005u;
  PassProfileScope profile("resolve", nullptr);
  const u32 flags = r.flags, destination_texture = r.destination_texture,
      destination_level = r.destination_level, destination_slice = r.destination_slice,
      source_rect = r.source_rect, destination_point = r.destination_point;
  const ResolveRect& rect = r.rect;
  const ResolvePoint& point = r.point;
  // Log distinct transfers, not the same loading-screen depth copy hundreds
  // of times before the first shadow atlas appears.
  using Transfer = std::array<u32, 10>;
  static std::vector<Transfer> traced_transfers;
  const Transfer transfer{flags, destination_texture, destination_level, destination_slice,
      u32(rect.left), u32(rect.top), u32(rect.right), u32(rect.bottom), u32(point.x), u32(point.y)};
  if (traced_transfers.size() < 128 &&
      std::find(traced_transfers.begin(), traced_transfers.end(), transfer) == traced_transfers.end()) {
    traced_transfers.push_back(transfer);
    if (traced_transfers.size() >= 128) g_resolve_trace_full.store(true, std::memory_order_relaxed);
    REXLOG_INFO("Native GPU: Resolve ABI lr={:08X} caller={:08X} flags={:08X} "
        "rect@{:08X}=({},{},{},{}) point@{:08X}=({},{}) clear@{:08X} z={}",
        r.lr, r.caller_lr, flags, source_rect, rect.left, rect.top, rect.right, rect.bottom,
        destination_point, point.x, point.y, r.clear_color, r.clear_z);
  }
  // TU23 sub_83FBDF80 selects +12832 for index 4, or +12816+4*index for 0..3.
  // Depth conversion remains explicit
  // unsupported work; never feed it unrelated color data.
  const u32 source = r.snapshot ? ((flags & 4u) ? r.bindings.depth_stencil : r.bindings.render_targets[flags & 3u])
      : (flags & 4u) ? BoundDepthStencil() : BoundRenderTarget(flags & 3u);
  const bool resolved = ResolveTextureFromSurface(destination_texture, source,
                                 destination_level, destination_slice, flags,
                                 source_rect ? &rect : nullptr,
                                 destination_point ? &point : nullptr);
  if (TemporalInputsCandidate() && resolved && !(flags & 4u) && !(flags & 3u)) {
    const auto extent = NativeTextureExtent(source);
    if (extent.logical_width == 1280 && extent.logical_height == 720)
      SnapshotTemporalSceneDepth(r.snapshot ? r.bindings.depth_stencil : BoundDepthStencil());
  }
  if (!resolved &&
      g_unsupported_draw_logs++ < 20) {
    REXLOG_WARN("Native GPU: skipped resolve src=0x{:08X} dst=0x{:08X} "
                "flags=0x{:08X} level={} slice={}", source,
                destination_texture, flags, destination_level, destination_slice);
  }
  if (!resolved) {
    static const u64 trace_from=[] {
      const char* value=std::getenv("LEGO_NATIVE_RESOLVE_FAILURE_TRACE");
      return value ? std::strtoull(value,nullptr,10) : ~u64{0};
    }();
    static std::set<std::tuple<u32,u32,u32>> failures;
    if (g_probe_frame.load()>=trace_from && failures.size()<128 &&
        failures.emplace(source,destination_texture,flags).second) {
      const auto src=NativeTextureExtent(source),dst=NativeTextureExtent(destination_texture);
      REXLOG_WARN("Native resolve failure frame={} src={:08X} {}x{} logical={}x{} dst={:08X} {}x{} logical={}x{} flags={:08X} level={} slice={}",
          g_probe_frame.load(),source,src.width,src.height,src.logical_width,src.logical_height,
          destination_texture,dst.width,dst.height,dst.logical_width,dst.logical_height,
          flags,destination_level,destination_slice);
    }
  }
  // RESOLVE_CLEARRENDERTARGET takes float RGBA, unlike Clear's packed ARGB.
  // Clear only after the copy, so accumulated intermediate passes start clean.
  if (resolved && (flags & 0x100u) && !(flags & 4u) && r.clear_color) {
    if (!r.clear_readable) return 0x80070057u;
    const float* rgba = r.clear_rgba;
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
    const auto extent=NativeTextureExtent(source);
    const plume::RenderRect area(
        NativeRenderEdge(rect.left,extent.logical_width,extent.width),
        NativeRenderEdge(rect.top,extent.logical_height,extent.height),
        NativeRenderEdge(rect.right,extent.logical_width,extent.width),
        NativeRenderEdge(rect.bottom,extent.logical_height,extent.height));
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

// Rect, point and clear words sit on the caller's stack, so they are read
// here; the copy queues behind the draws that produced its source.
u32 ResolveHook(D3DDevice* device, u32 flags, u32 source_rect,
                u32 destination_texture, u32 destination_point,
                u32 destination_level, u32 destination_slice,
                u32 clear_color, f64 clear_z, u32 lr, u32 sp) {
  ResolveRequest r{flags, source_rect, destination_texture, destination_point,
      destination_level, destination_slice, clear_color, lr, 0, clear_z, {}, {}, {}, false};
  r.snapshot = device && SnapshotStateBindingsEnabled();
  if (r.snapshot) r.bindings = ReadDeviceBindings(device);
  i32 rect_words[4]{}, point_words[2]{};
  if ((source_rect && !ReadResolveWords(source_rect, rect_words, 4)) ||
      (destination_point && !ReadResolveWords(destination_point, point_words, 2)))
    return 0x80070057u;
  r.rect = {rect_words[0], rect_words[1], rect_words[2], rect_words[3]};
  r.point = {point_words[0], point_words[1]};
  if ((flags & 0x100u) && !(flags & 4u) && clear_color) {
    i32 clear_words[4];
    r.clear_readable = ReadResolveWords(clear_color, clear_words, 4);
    if (r.clear_readable) std::memcpy(r.clear_rgba, clear_words, sizeof(r.clear_rgba));
  }
  if (!g_resolve_trace_full.load(std::memory_order_relaxed)) {
    i32 backchain = 0, caller_lr = 0;
    if (ReadResolveWords(sp, &backchain, 1) && u32(backchain) >= 8)
      ReadResolveWords(u32(backchain) - 8, &caller_lr, 1);
    r.caller_lr = u32(caller_lr);
  }
  if (NativeWorkerShouldQueue() && NativeQueuedResolveEnabled()) {
    NativeWorkerSubmit([r] { ResolveRecorded(r); });
    return 0;
  }
  return ResolveRecorded(r);
}

u32 SwapHook(D3DDevice* /*device*/, u32 front_buffer, u32 /*parameters*/) {
  HostDevice::PollCompletionCallbacks();
  auto recording = HostDevice::LockRecording();
  PollDrawBenchmarkMode();
  PollFullDrawBatchLimit();
  pass_profile::OnSwap();
  auto texture = ResolveTextureResource(front_buffer);
  HostDevice::SnapshotTexture(texture.texture,"pre-fsr-color",false);
  texture = PresentTemporalFrame(texture,g_temporal_scene_depth,g_probe_frame.load());
  if (auto* capture = NativeStateCalls()) capture->Present(g_probe_frame.load());
  if (texture.texture) {
    static std::unordered_set<u64> reported_extents;
    const u64 key=(u64(texture.width)<<32)|texture.height;
    if (reported_extents.size()<16 && reported_extents.insert(key).second)
      REXLOG_INFO("Native GPU: presented internal texture {}x{} (before swap-chain copy)",texture.width,texture.height);
  }
  const auto sample_offset = TemporalPresentationSampleOffset();
  const bool presented = HostDevice::PresentTexture(texture.texture, texture.descriptor_index,
      sample_offset[0],sample_offset[1]);
  EndTemporalFrame(presented);
  if (TemporalInputsCandidate()) g_temporal_draw_history.EndFrame(presented);
  if (TemporalInputsCandidate()) {
    // Keep the allocation, never its validity. The next frame must copy depth
    // after scene geometry before presentation can consume it.
    g_temporal_scene_depth.frame = ~uint64_t{0};
    g_temporal_scene_depth_surface = 0;
    g_temporal_scene_depth_dirty = false;
  }
  if(!EndNativeCompositionTargets()) return 0;
  if (!presented &&
      g_unsupported_draw_logs++ < 20) {
    LogUnknownTexture(front_buffer, "present");
    REXLOG_WARN("Native GPU: skipped present for front buffer 0x{:08X}",
                front_buffer);
  }
  // No guest draw may see half a mode change. Old attachments and descriptors
  // remain owned by their submission fences while the next frame gets new ones.
  const auto settings_runtime = AcquireNativeSettingsRuntime();
  const auto settings_result = settings_runtime ? settings_runtime->CommitFrameBoundary() :
      TryApplyNativeFsr3Settings(ReconfigureNativeSceneSurfaces);
  if (settings_result == NativeFsr3ApplyResult::kApplied) {
    g_temporal_draw_history.Reset();
    g_temporal_camera_authority.Reset();
    g_temporal_scene_depth.frame = ~uint64_t{0};
    g_temporal_scene_depth_surface = 0;
    g_temporal_scene_depth_dirty = false;
    const auto size = NativeSceneRenderSize();
    REXLOG_INFO("Native GPU: live upscaling mode {} scale={}% scene={}x{}",
        NativeFsr3Enabled()?"FSR 3":"off",NativeResolutionScale(),size.width,size.height);
  }
  if (settings_result == NativeFsr3ApplyResult::kDeferred) {
    static unsigned deferred_logs = 0;
    if (deferred_logs++ < 8)
      REXLOG_WARN("Native GPU: scene storage change deferred; retaining the active render settings");
  }
  recording.unlock();
  HostDevice::PollCompletionCallbacks();
  return 0;
}

u32 SwapHookQueued(D3DDevice* device, u32 front_buffer, u32 parameters) {
  if (PollRendererOptimizations())
    REXLOG_INFO("Native renderer optimizations: mask={}", RendererOptimizationMask().load(std::memory_order_relaxed));
  if (const auto changed = PollVertexFetchPlan())
    REXLOG_INFO("Native vertex fetch plan: enabled={}", *changed);
  if (const auto changed = PollVertexFetchPlanCacheLru())
    REXLOG_INFO("Native vertex fetch plan cache: lru={}", *changed);
  if (NativeWorkerShouldQueue()) {
    NativeWorkerSubmit([front_buffer] { SwapHook(nullptr, front_buffer, 0); }, true, false, false, NativeTaskStage::kRecorderBarrier);
    // The worker may finish the previous frame while the guest builds the next
    // one, but never lag further: the game's own CPU/GPU pacing assumes that.
    NativeWorkerThrottleFrames(1);
    static const bool batched_callbacks = [] {
      const char* value = std::getenv("LEGO_NATIVE_BATCH_COMPLETION_CALLBACKS");
      return value && value[0] == '1';
    }();
    HostDevice::PollCompletionCallbacks(batched_callbacks);
    static u64 presents = 0;
    if (++presents % 600 == 0) {
      const auto stats = ConsumeNativeWorkerStats();
      REXLOG_INFO("Native worker: last 600 frames tasks={} inline={} drains={} drain_wait={:.1f}ms throttle_wait={:.1f}ms",
          stats.executed, stats.inline_runs, stats.drains, stats.wait_us / 1000.0,
          stats.throttle_us / 1000.0);
    }
    return 0;
  }
  return SwapHook(device, front_buffer, parameters);
}

u32 BeginTilingRecorded(u32 flags, u32 count, NativeTileExtent extent,
    u32 color, f64 clear_z, u32 clear_stencil, const DrawBindings* captured,
    float viewport_width, float viewport_height, u32 guest_count) {
  if (captured)
    WaitPendingPreparationFor({captured->render_targets[0], captured->render_targets[1], captured->render_targets[2],
        captured->render_targets[3], captured->depth_stencil});
  auto recording = HostDevice::LockRecording();
  const auto bindings = captured ? *captured : SnapshotDrawBindings();
  // Promotion precedes the pass clear and geometry in queue order.
  for (u32 surface : bindings.render_targets)
    if (!PromoteTiledSurface(surface, extent.width, extent.height)) return 0x80004001u;
  if (!PromoteTiledSurface(bindings.depth_stencil, extent.width, extent.height))
    return 0x80004001u;
  g_seen_tiled_pass = true;
  HostDevice::BeginTiledPassSnapshot();
  static std::atomic<u32> logs{0};
  if (logs.fetch_add(1) < 8)
    REXLOG_INFO("Native GPU: BeginTiling flags={:X} tiles={} host={}x{} viewport={}x{} guest_count={}",
        flags, count, extent.width, extent.height, viewport_width,
        viewport_height, guest_count);
  // The TU23 implementation branches past its initial clear when bit 0 is set.
  if (flags & 1) return 0;
  return ClearRecorded(0x31, color, clear_z, clear_stencil, {}, &bindings);
}


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
  const bool snapshot = device && SnapshotStateBindingsEnabled();
  const auto bindings = snapshot ? ReadDeviceBindings(device) : DrawBindings{};
  const float viewport_width = device ? float(device->viewport.width) : 0;
  const float viewport_height = device ? float(device->viewport.height) : 0;
  const u32 guest_count = device ? u32(*reinterpret_cast<const be_u32*>(
      reinterpret_cast<const u8*>(device) + 13124)) : 0;
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_QUEUED_BEGIN_TILING");
    return !value || value[0] != '0';
  }();
  if (enabled && NativeWorkerShouldQueue()) {
    NativeWorkerSubmit([flags, count, extent, color, clear_z, clear_stencil, snapshot,
        bindings, viewport_width, viewport_height, guest_count] {
      const auto status = BeginTilingRecorded(flags, count, extent, color, clear_z, clear_stencil,
          snapshot ? &bindings : nullptr, viewport_width, viewport_height, guest_count);
      if (status) REXLOG_ERROR("Native GPU: queued BeginTiling failed status={:08X}", status);
    });
    return 0;
  }
  return BeginTilingRecorded(flags, count, extent, color, clear_z, clear_stencil,
      snapshot ? &bindings : nullptr, viewport_width, viewport_height, guest_count);
}

u32 EndTilingRecorded(u32 flags, u32 destination, const DrawBindings* captured = nullptr) {
  if (captured)
    WaitPendingPreparationFor({destination});
  auto recording = HostDevice::LockRecording();
  const u32 source = captured ? ((flags & 4) ? captured->depth_stencil : captured->render_targets[flags & 3])
      : (flags & 4) ? BoundDepthStencil() : BoundRenderTarget(flags & 3);
  const bool resolved = ResolveTextureFromSurface(destination, source, 0, 0, flags);
  static std::atomic<u32> logs{0};
  if (logs.fetch_add(1) < 8)
    REXLOG_INFO("Native GPU: EndTiling source=0x{:08X} destination=0x{:08X} resolved={}",
                 source, destination, resolved);
  return resolved ? 0 : 0x80004001u;
}

// The copy must follow the queued draws of the tiled pass, so it queues like Resolve.
u32 EndTilingHook(D3DDevice* device, u32 flags, u32 /*rectangles*/,
                  u32 destination, u32 /*clear_color*/, f64 /*clear_z*/,
                  u32 /*z_gpr_slot*/, u32 /*clear_stencil*/, u32 /*parameters*/) {
  if (!destination) return 0;
  const bool snapshot = device && SnapshotStateBindingsEnabled();
  const auto bindings = snapshot ? ReadDeviceBindings(device) : DrawBindings{};
  if (NativeWorkerShouldQueue() && NativeQueuedResolveEnabled()) {
    NativeWorkerSubmit([flags, destination, snapshot, bindings] { EndTilingRecorded(flags, destination, snapshot ? &bindings : nullptr); });
    return 0;
  }
  return EndTilingRecorded(flags, destination, snapshot ? &bindings : nullptr);
}

}  // namespace

bool NativeParallelDrawsEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_PARALLEL_DRAWS");
    const bool selected = NativePrepareEnabled() && (!value || value[0] != '0') &&
        RetainedDrawPreparationEnabled() && SnapshotStateBindingsEnabled() &&
        !std::getenv("LEGO_NATIVE_NO_DRAW_ARENA") && !std::getenv("LEGO_NATIVE_VIEWPORT") &&
        !std::getenv("LEGO_NATIVE_ALL_TEXTURES") && !std::getenv("LEGO_NATIVE_MESH_TRACE") &&
        !std::getenv("LEGO_NATIVE_COLOR_TRACE") && !std::getenv("LEGO_NATIVE_DRAW_TRACE") &&
        !PortraitProbeEnabled();
    REXLOG_INFO("Native draw batches: {} workers={}", selected ? "parallel" : "serial", NativePreparationWorkerCount());
    return selected;
  }();
  return enabled && !NativeFsr3Enabled() && g_draw_benchmark_mode.load(std::memory_order_relaxed) != 0;
}

bool NativeDrawBatchPending() { return g_draw_batch_pending.load(std::memory_order_acquire); }

void FlushFullDrawBatch() {
  if (g_full_draw_batch.empty()) return;
  auto batch = std::make_shared<FullDrawBatch>();
  batch->works = std::move(g_full_draw_batch);
  batch->bloom = g_full_draw_batch_bloom;
  g_full_draw_batch.clear();
  g_full_draw_batch_draws = 0;
  g_full_draw_batch_bloom = {};
  PrepareFullDrawBatch(batch);
  while (!g_inflight_batches.empty() && !g_inflight_batches.front()->pending_jobs.load(std::memory_order_acquire))
    g_inflight_batches.pop_front();
  if (!batch->jobs.empty()) g_inflight_batches.push_back(batch);
  NativeRecorderSubmit([batch] { ReplayFullDrawBatch(batch); });
  g_draw_batch_pending.store(false, std::memory_order_release);
}

void WaitPendingPreparationFor(std::initializer_list<unsigned> addresses) {
  if (!t_on_coordinator) return;
  if (std::none_of(addresses.begin(),addresses.end(),[](unsigned address){return address != 0;})) return;
  PreparationPhysicalFilter physical;
  for (const auto address : addresses) if (address) physical.Add(CaptureTexturePreparationFootprint(address));
  const auto matches = [&](const PreparationDependencyFilter& filter) {
    return filter.Matches(addresses) || filter.physical.Matches(physical);
  };
  if (matches(g_full_draw_batch_bloom)) FlushFullDrawBatch();
  const bool per_chunk = ChunkDependencyWaitsEnabled();
  for (const auto& batch : g_inflight_batches) {
    if (!batch->pending_jobs.load(std::memory_order_acquire)) continue;
    if (!matches(batch->bloom)) continue;
    for (const auto& job : batch->jobs) {
      if (per_chunk && !matches(job.dependencies)) continue;
      RunPreparationChunkHere(job);
      job.ready.wait();
    }
  }
}

void QueueNativeReplayTask(std::function<void()> task) {
  if (OnNativeRecorderThread()) { task(); return; }
  auto work = t_full_draw_pool.Acquire();
  work->clean = false;
  work->deferred = true; work->submitted = true; work->sequence = ++g_full_draw_sequence;
  work->replay_task = std::move(task);
  const u32 limit = FullDrawBatchLimit().load(std::memory_order_relaxed);
  g_full_draw_batch.push_back(std::move(work));
  g_draw_batch_pending.store(true, std::memory_order_release);
  if (g_full_draw_batch.size() >= limit) FlushFullDrawBatch();
}

void WaitAllPreparation() {
  if (OnNativeRecorderThread()) return;
  FlushFullDrawBatch();
  for (const auto& batch : g_inflight_batches) {
    if (!batch->pending_jobs.load(std::memory_order_acquire)) continue;
    for (const auto& job : batch->jobs) {
      RunPreparationChunkHere(job);
      job.ready.wait();
    }
  }
}

void FlushFullDrawBatchIdle() {
  const u32 limit = std::max(PrepareChunkSize(), FullDrawBatchLimit().load(std::memory_order_relaxed) / 2);
  if (std::max<size_t>(g_full_draw_batch.size(), g_full_draw_batch_draws) >= limit) FlushFullDrawBatch();
}

void RecordDeferredNativeTask(std::function<void()>& task) {
  static thread_local DrawDeviceView immutable;
  static thread_local u32 refresh = 0;
  if (!immutable.device || (++refresh & 63) == 0) immutable = HostDevice::ImmutableDrawDeviceView();
  auto view = immutable;
  if (!view.device) { task(); return; }
  auto work = t_full_draw_pool.Acquire();
  work->clean = false;
  work->deferred = true; work->submitted = true; work->sequence = ++g_full_draw_sequence;
  view.commands = work->commands.get();
  view.recording_serial = work->sequence;
  work->preparation.view = view;
  work->preparation.uploads = work->commands.get();
  work->preparation.allocation = {};
  work->preparation.capacity = work->preparation.used = 0;
  {
    DrawPreparationScope scope(work->preparation);
    t_on_coordinator = true;
    task();
    t_on_coordinator = false;
  }
  const u32 limit = FullDrawBatchLimit().load(std::memory_order_relaxed);
  g_full_draw_batch.push_back(std::move(work));
  g_draw_batch_pending.store(true, std::memory_order_release);
  if (g_full_draw_batch.size() >= limit) FlushFullDrawBatch();
}

void FlushNativeDrawBatch() {
  QueueNativeDrawBatch();
  if (g_pending_draw_batches.empty()) return;
  auto batches = std::move(g_pending_draw_batches);
  g_pending_draw_batches.clear();
  std::exception_ptr failure;
  for (auto& batch : batches) {
    for (auto& job : batch.jobs) {
      try { job.get(); } catch (...) { if (!failure) failure = std::current_exception(); }
    }
  }
  if (failure) std::rethrow_exception(failure);
  for (auto& batch : batches) {
    for (const auto& work : *batch.draws) {
      DrawBindingsScope bindings(work->bindings);
      ShaderBindingsScope shaders(work->shaders);
      t_recording_draw = work.get();
      if (!DispatchDraw(work->device.get(), work->primitive, work->indexed, work->start, work->count, work->base_vertex))
        FailActiveQueries();
      t_recording_draw = nullptr;
    }
    HostDevice::RetireResource(RetainDrawBatchResources(batch.draws));
  }
  g_draw_batch_pending.store(false, std::memory_order_release);
}

DrawTiming ConsumeDrawTiming() {
  auto recording = HostDevice::LockRecording();
  return std::exchange(g_draw_timing, {});
}

void ResetDrawResources() {
  std::lock_guard lock(g_pipeline_mutex);
  g_prepared_draw.Reset();
  pass_profile::Reset();
  g_temporal_draw_history.Reset();
  g_temporal_camera_authority.Reset();
  g_vertex_bindings.Reset();
  g_vertex_input_usage.Clear();
  g_vertex_binding_backend_ready=false;
  g_temporal_scene_depth = {};
  g_temporal_scene_depth_surface = 0;
  g_temporal_scene_depth_dirty = false;
  g_pipelines.clear();
  g_pipeline_creation.ClearAfterDrain();
  g_quad_indices.Clear();
  g_temporal_pipelines.clear();
  g_temporal_pipeline_creation.ClearAfterDrain();
  ResetTemporalPresentation();
  ResetTemporalShaders();
  ResetFloatBlendIndicesShaders();
  ResetInstancedConstantsShaders();
  ResetIntegerInputShaders();
  g_seen_tiled_pass = false;
}

}  // namespace legodimensions::gpu_native

REX_HOOK(sub_83FC6640, legodimensions::gpu_native::DrawVerticesHookQueued);
REX_HOOK(sub_83FC6A58, legodimensions::gpu_native::DrawIndexedVerticesHookQueued);
REX_HOOK_RAW(sub_83FBF0D8) {
  ctx.r3.u64 = legodimensions::gpu_native::ResolveHook(
      reinterpret_cast<legodimensions::gpu_native::D3DDevice*>(base + ctx.r3.u32), ctx.r4.u32, ctx.r5.u32,
      ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32,
      ctx.f1.f64, ctx.lr, ctx.r1.u32);
}
REX_HOOK(sub_83FB33E0, legodimensions::gpu_native::SwapHookQueued);
REX_HOOK(sub_83FBC9D8, legodimensions::gpu_native::ClearHook);
REX_HOOK(sub_83FBCD28, legodimensions::gpu_native::BeginTilingHook);
REX_HOOK(sub_83FBD098, legodimensions::gpu_native::EndTilingHook);
