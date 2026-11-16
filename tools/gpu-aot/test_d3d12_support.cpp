#include "gpu_native/d3d12_support.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
using namespace legodimensions::gpu_native;
static void Check(bool ok) { if (!ok) std::exit(23); }
struct Device {
  unsigned known_model = 0x66, supported_model = 0x66, tier = 3, wave_min = 32, wave_max = 32;
  bool query_error = false, options_error = false, waves_error = false, waves = true, fp16 = true, uma = false;
  unsigned shader_queries = 0;
  HRESULT CheckFeatureSupport(D3D12_FEATURE feature, void* data, UINT) {
    if (feature == D3D12_FEATURE_SHADER_MODEL) {
      ++shader_queries;
      if (query_error) return E_FAIL;
      auto& value = *static_cast<D3D12_FEATURE_DATA_SHADER_MODEL*>(data);
      if (unsigned(value.HighestShaderModel) > known_model) return E_INVALIDARG;
      value.HighestShaderModel = D3D_SHADER_MODEL(supported_model); return S_OK;
    }
    if (feature == D3D12_FEATURE_D3D12_OPTIONS) {
      if (options_error) return E_FAIL;
      static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS*>(data)->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER(tier);
    } else if (feature == D3D12_FEATURE_D3D12_OPTIONS1) {
      if (waves_error) return E_FAIL;
      auto& value = *static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS1*>(data);
      value.WaveOps = waves; value.WaveLaneCountMin = wave_min; value.WaveLaneCountMax = wave_max;
    } else if (feature == D3D12_FEATURE_D3D12_OPTIONS4) {
      static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS4*>(data)->Native16BitShaderOpsSupported = fp16;
    } else if (feature == D3D12_FEATURE_ARCHITECTURE1) {
      static_cast<D3D12_FEATURE_DATA_ARCHITECTURE1*>(data)->UMA = uma;
    } else return E_INVALIDARG;
    return S_OK;
  }
};
int main() {
  Check(!QueryD3D12Support<Device>(nullptr).BaseReady());
  unsigned profiles = 0;
  // Exercise fixed-32, fixed-64, and selectable 32/64 lanes without vendor IDs.
  for (auto range : {32u, 64u, 96u}) for (bool fp16 : {false, true}) for (bool uma : {false, true}) {
    Device device; device.wave_min = range == 64 ? 64 : 32;
    device.wave_max = range == 32 ? 32 : 64; device.fp16 = fp16; device.uma = uma;
    auto caps = QueryD3D12Support(&device);
    Check(caps.BaseReady() && caps.Fsr3Ready() && caps.native_fp16 == fp16 && caps.uma == uma); ++profiles;
  }
  Device old_runtime; old_runtime.known_model = old_runtime.supported_model = 0x62;
  Check(QueryD3D12Support(&old_runtime).Fsr3Ready() && old_runtime.shader_queries == 5);
  Device base_only; base_only.supported_model = 0x60;
  Check(QueryD3D12Support(&base_only).BaseReady() && !QueryD3D12Support(&base_only).Fsr3Ready());
  Device no_waves; no_waves.waves = false;
  Check(QueryD3D12Support(&no_waves).BaseReady() && !QueryD3D12Support(&no_waves).Fsr3Ready());
  Device bad_waves; bad_waves.wave_min = 64; bad_waves.wave_max = 32;
  Check(!QueryD3D12Support(&bad_waves).Fsr3Ready());
  Device small_waves; small_waves.wave_min = small_waves.wave_max = 4;
  Check(QueryD3D12Support(&small_waves).BaseReady() && !QueryD3D12Support(&small_waves).Fsr3Ready());
  Device wide_range; wide_range.wave_min = 8; wide_range.wave_max = 32;
  Check(QueryD3D12Support(&wide_range).Fsr3Ready());
  Device failed; failed.query_error = true;
  auto failed_caps = QueryD3D12Support(&failed);
  Check(!failed_caps.BaseReady() && !failed_caps.Fsr3Ready() && failed.shader_queries == 1);
  Device unknown; unknown.known_model = 0;
  Check(!QueryD3D12Support(&unknown).BaseReady() && unknown.shader_queries == 7);
  Device tier1; tier1.tier = 1; Check(!QueryD3D12Support(&tier1).BaseReady());
  Device failed_options; failed_options.options_error = true; Check(!QueryD3D12Support(&failed_options).BaseReady());
  Device failed_waves; failed_waves.waves_error = true;
  Check(QueryD3D12Support(&failed_waves).BaseReady() && !QueryD3D12Support(&failed_waves).Fsr3Ready());
  std::printf("PASS: %u vendor-independent lane/fp16/UMA profiles; old-runtime fallback and failed-query rejection\n", profiles);
}
