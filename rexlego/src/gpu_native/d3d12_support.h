#pragma once
#include <d3d12.h>

namespace legodimensions::gpu_native {

// Check features rather than GPU vendor names. FP16 and selectable wave sizes
// are optional FidelityFX permutations, never base-renderer requirements.
struct D3D12Support {
  unsigned shader_model = 0, binding_tier = 0, wave_min = 0, wave_max = 0;
  bool wave_ops = false, native_fp16 = false, uma = false;
  bool BaseReady() const { return shader_model >= 0x60 && binding_tier >= 2; }
  // The bundled FidelityFX shader build uses cs_6_2, with optional cs_6_6.
  // Keep sub-32-only implementations on spatial presentation. The bundled
  // kernels fault inside the tested WARP driver (4 lanes); that path is not
  // validated. This is a capability restriction, not a vendor-name blacklist.
  bool Fsr3Ready() const {
    const bool wave32 = wave_min != 0 && wave_min <= 32 && wave_max >= 32;
    const bool wave64 = wave_min != 0 && wave_min <= 64 && wave_max >= 64;
    return shader_model >= 0x62 && wave_ops && wave_max >= wave_min && (wave32 || wave64);
  }
};

template <typename Device>
D3D12Support QueryD3D12Support(Device* device) {
  D3D12Support result;
  if (!device) return result;
  // Older runtimes return E_INVALIDARG for enum values they do not recognize.
  // Retry lower models; other failures cannot establish feature support.
  for (unsigned model = 0x66; model >= 0x60; --model) {
    D3D12_FEATURE_DATA_SHADER_MODEL data{static_cast<D3D_SHADER_MODEL>(model)};
    const auto status = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &data, sizeof(data));
    if (status == E_INVALIDARG) continue;
    if (SUCCEEDED(status)) result.shader_model = static_cast<unsigned>(data.HighestShaderModel);
    break;
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
    result.binding_tier = static_cast<unsigned>(options.ResourceBindingTier);
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 waves{};
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &waves, sizeof(waves)))) {
    result.wave_ops = waves.WaveOps != FALSE;
    result.wave_min = waves.WaveLaneCountMin;
    result.wave_max = waves.WaveLaneCountMax;
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS4 fp16{};
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &fp16, sizeof(fp16))))
    result.native_fp16 = fp16.Native16BitShaderOpsSupported != FALSE;
  D3D12_FEATURE_DATA_ARCHITECTURE1 architecture{};
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &architecture, sizeof(architecture))))
    result.uma = architecture.UMA != FALSE;
  return result;
}
}  // namespace legodimensions::gpu_native
