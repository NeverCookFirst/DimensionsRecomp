#include "gpu_native/fsr3_upscaler.h"
#include "gpu_native/d3d12_support.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <climits>
#include <utility>
#include <vector>
#include <d3d12.h>
#include <wrl/client.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>

namespace legodimensions::gpu_native {
using Microsoft::WRL::ComPtr;
struct Fsr3Upscaler::Impl {
  ComPtr<ID3D12Device> device;
  std::vector<unsigned char> scratch;
  std::unique_ptr<FfxFsr3UpscalerContext> context;
  ComPtr<ID3D12Resource> dilated_depth, dilated_motion, previous_depth;
  Fsr3Size render, output;
  uint64_t last_frame = 0, last_epoch = 0;
  bool valid = false, has_dispatch = false, reset_pending = true, last_reset = true;
  ~Impl() {
    if (valid) ffxFsr3UpscalerContextDestroy(context.get());
  }
};

namespace {
ComPtr<ID3D12Resource> SharedTexture(ID3D12Device* device,
                                   const FfxCreateResourceDescription& desc) {
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  switch (desc.resourceDescription.format) {
    case FFX_SURFACE_FORMAT_R32_FLOAT: format = DXGI_FORMAT_R32_FLOAT; break;
    case FFX_SURFACE_FORMAT_R16G16_FLOAT: format = DXGI_FORMAT_R16G16_FLOAT; break;
    case FFX_SURFACE_FORMAT_R32_UINT: format = DXGI_FORMAT_R32_UINT; break;
    default: return {};
  }
  if (desc.resourceDescription.type != FFX_RESOURCE_TYPE_TEXTURE2D ||
      desc.resourceDescription.depth != 1 || desc.resourceDescription.mipCount != 1)
    return {};
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC texture{};
  texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture.Width = desc.resourceDescription.width;
  texture.Height = desc.resourceDescription.height;
  texture.DepthOrArraySize = texture.MipLevels = 1;
  texture.Format = format;
  texture.SampleDesc.Count = 1;
  texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  ComPtr<ID3D12Resource> result;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
      &texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
      IID_PPV_ARGS(&result)))) return {};
  return result;
}

bool TextureExtent(ID3D12Resource* resource, Fsr3Size size) {
  if (!resource) return false;
  const auto desc = resource->GetDesc();
  return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
      desc.Width == size.width && desc.Height == size.height &&
      desc.DepthOrArraySize == 1 && desc.SampleDesc.Count == 1 &&
      desc.MipLevels == 1;
}
bool ColorFormat(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
      format == DXGI_FORMAT_R32G32B32A32_FLOAT ||
      format == DXGI_FORMAT_R8G8B8A8_UNORM ||
      format == DXGI_FORMAT_R10G10B10A2_UNORM;
}
FfxResource ReadResource(ID3D12Resource* resource, const wchar_t* name) {
  return ffxGetResourceDX12(resource,
      ffxGetResourceDescriptionDX12(resource, FFX_RESOURCE_USAGE_READ_ONLY),
      name, FFX_RESOURCE_STATE_COMPUTE_READ);
}
FfxResource WriteResource(ID3D12Resource* resource, const wchar_t* name) {
  return ffxGetResourceDX12(resource,
      ffxGetResourceDescriptionDX12(resource, FFX_RESOURCE_USAGE_UAV),
      name, FFX_RESOURCE_STATE_UNORDERED_ACCESS);
}
}  // namespace

Fsr3Upscaler::Fsr3Upscaler(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Fsr3Upscaler::~Fsr3Upscaler() = default;

std::shared_ptr<Fsr3Upscaler> Fsr3Upscaler::Create(ID3D12Device* device,
    Fsr3Size render, Fsr3Size output, bool inverted_depth) {
  if (!device || !render.width || !render.height ||
      output.width < render.width || output.height < render.height ||
      output.width > 8192 || output.height > 8192) return {};
  if (!QueryD3D12Support(device).Fsr3Ready()) return {};
  auto impl = std::make_unique<Impl>();
  impl->device = device;
  impl->render = render;
  impl->output = output;
  impl->scratch.resize(ffxGetScratchMemorySizeDX12(FFX_FSR3UPSCALER_CONTEXT_COUNT));
  FfxFsr3UpscalerContextDescription desc{};
  desc.flags = FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
  if (inverted_depth) desc.flags |= FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED;
  desc.maxRenderSize = {render.width, render.height};
  desc.maxUpscaleSize = {output.width, output.height};
  if (ffxGetInterfaceDX12(&desc.backendInterface, ffxGetDeviceDX12(device),
      impl->scratch.data(), impl->scratch.size(), FFX_FSR3UPSCALER_CONTEXT_COUNT) != FFX_OK)
    return {};
  impl->context = std::make_unique<FfxFsr3UpscalerContext>();
  if (ffxFsr3UpscalerContextCreate(impl->context.get(), &desc) != FFX_OK) return {};
  impl->valid = true;
  FfxFsr3UpscalerSharedResourceDescriptions shared{};
  if (ffxFsr3UpscalerGetSharedResourceDescriptions(impl->context.get(), &shared) != FFX_OK)
    return {};
  impl->dilated_depth = SharedTexture(device, shared.dilatedDepth);
  impl->dilated_motion = SharedTexture(device, shared.dilatedMotionVectors);
  impl->previous_depth = SharedTexture(device, shared.reconstructedPrevNearestDepth);
  if (!impl->dilated_depth || !impl->dilated_motion || !impl->previous_depth) return {};
  return std::shared_ptr<Fsr3Upscaler>(new Fsr3Upscaler(std::move(impl)));
}

bool Fsr3Upscaler::Jitter(uint64_t frame, float& x, float& y) const {
  const auto phases = ffxFsr3UpscalerGetJitterPhaseCount(impl_->render.width, impl_->output.width);
  return phases > 0 && ffxFsr3UpscalerGetJitterOffset(&x, &y,
      static_cast<int32_t>(frame % phases), phases) == FFX_OK;
}

bool Fsr3Upscaler::Dispatch(ID3D12GraphicsCommandList* commands,
                          const Fsr3FrameInputs& inputs) {
  auto& state = *impl_;
  const auto reject = [&state] { state.reset_pending = true; return false; };
  if (!commands || !TextureExtent(inputs.color, state.render) ||
      !TextureExtent(inputs.depth, state.render) ||
      !TextureExtent(inputs.motion, state.render) ||
      !TextureExtent(inputs.output, state.output)) return reject();
  ID3D12Resource* resources[] = {inputs.color, inputs.depth, inputs.motion, inputs.reactive, inputs.output};
  for (size_t i = 0; i < std::size(resources); ++i)
    for (size_t j = i + 1; j < std::size(resources); ++j)
      if (resources[i] && resources[i] == resources[j]) return reject();
  const auto motion_format = inputs.motion->GetDesc().Format;
  if (inputs.depth->GetDesc().Format != DXGI_FORMAT_R32_FLOAT ||
      (motion_format != DXGI_FORMAT_R16G16_FLOAT && motion_format != DXGI_FORMAT_R32G32_FLOAT) ||
      !ColorFormat(inputs.color->GetDesc().Format) || !ColorFormat(inputs.output->GetDesc().Format) ||
      !(inputs.output->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return reject();
  if (inputs.reactive && (!TextureExtent(inputs.reactive, state.render) ||
      (inputs.reactive->GetDesc().Format != DXGI_FORMAT_R8_UNORM &&
       inputs.reactive->GetDesc().Format != DXGI_FORMAT_R32_FLOAT))) return reject();
  for (float value : {inputs.jitter_x, inputs.jitter_y, inputs.frame_time_ms,
       inputs.pre_exposure, inputs.camera_near, inputs.camera_far,
       inputs.camera_fov, inputs.sharpness})
    if (!std::isfinite(value)) return reject();
  if (inputs.frame_time_ms <= 0 || inputs.pre_exposure <= 0 || inputs.camera_near <= 0 ||
      inputs.camera_far <= inputs.camera_near || inputs.camera_fov <= 0 ||
      inputs.camera_fov >= 3.14159265f || inputs.sharpness < 0 || inputs.sharpness > 1)
    return reject();
  float jitter_x, jitter_y;
  if (!Jitter(inputs.frame, jitter_x, jitter_y) ||
      std::abs(jitter_x - inputs.jitter_x) > 0.000001f ||
      std::abs(jitter_y - inputs.jitter_y) > 0.000001f) return reject();
  // A rejected duplicate/out-of-order frame cannot consume history twice.
  if (state.has_dispatch && (inputs.scene_epoch < state.last_epoch ||
      (inputs.scene_epoch == state.last_epoch && inputs.frame <= state.last_frame))) return reject();
  FfxFsr3UpscalerDispatchDescription desc{};
  desc.commandList = ffxGetCommandListDX12(commands);
  desc.color = ReadResource(inputs.color, L"Native scene color");
  desc.depth = ReadResource(inputs.depth, L"Native scene depth snapshot");
  desc.motionVectors = ReadResource(inputs.motion, L"Native scene normalized velocity");
  if (inputs.reactive) desc.reactive = ReadResource(inputs.reactive, L"Native history rejection");
  desc.output = WriteResource(inputs.output, L"Native FSR 3 output");
  desc.dilatedDepth = WriteResource(state.dilated_depth.Get(), L"FSR dilated depth");
  desc.dilatedMotionVectors = WriteResource(state.dilated_motion.Get(), L"FSR dilated motion");
  desc.reconstructedPrevNearestDepth = WriteResource(state.previous_depth.Get(), L"FSR previous depth");
  desc.renderSize = {state.render.width, state.render.height};
  desc.upscaleSize = {state.output.width, state.output.height};
  desc.motionVectorScale = {float(state.render.width), float(state.render.height)};
  desc.jitterOffset = {inputs.jitter_x, inputs.jitter_y};
  desc.frameTimeDelta = inputs.frame_time_ms;
  desc.preExposure = inputs.pre_exposure;
  desc.cameraNear = inputs.camera_near;
  desc.cameraFar = inputs.camera_far;
  desc.cameraFovAngleVertical = inputs.camera_fov;
  desc.viewSpaceToMetersFactor = 1;
  desc.enableSharpening = inputs.sharpen;
  desc.sharpness = inputs.sharpness;
  desc.reset = state.reset_pending || inputs.camera_cut || inputs.scene_epoch != state.last_epoch ||
      state.last_frame == UINT64_MAX || inputs.frame != state.last_frame + 1;
  if (ffxFsr3UpscalerContextDispatch(state.context.get(), &desc) != FFX_OK) return reject();
  state.last_frame = inputs.frame;
  state.last_epoch = inputs.scene_epoch;
  state.has_dispatch = true;
  state.reset_pending = false;
  state.last_reset = desc.reset;
  return true;
}
void Fsr3Upscaler::InvalidateHistory() { impl_->reset_pending = true; }
bool Fsr3Upscaler::LastDispatchResetHistory() const { return impl_->last_reset; }
}  // namespace legodimensions::gpu_native
