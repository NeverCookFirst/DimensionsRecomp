// Standalone capability report. Never loads the game, its DLLs or settings.
#include <cstdio>
#include <string>
#if defined(_M_X64) || defined(__x86_64__)
#define LEGO_PROBE_X64 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "gpu_native/d3d12_support.h"
#endif

static std::string Json(const std::string& value) {
  std::string out = "\"";
  for (unsigned char c : value) {
    if (c == '\\' || c == '"') { out += '\\'; out += char(c); }
    else if (c < 32) { char escape[7]; std::snprintf(escape, sizeof(escape), "\\u%04x", c); out += escape; }
    else out += char(c);
  }
  return out + '"';
}
static const char* Bool(bool value) { return value ? "true" : "false"; }
#ifdef LEGO_PROBE_X64
static void Cpuid(unsigned leaf, unsigned subleaf, unsigned (&out)[4]) {
#if defined(_MSC_VER)
  int data[4]; __cpuidex(data, int(leaf), int(subleaf));
  for (unsigned i = 0; i < 4; ++i) out[i] = unsigned(data[i]);
#else
  __cpuid_count(leaf, subleaf, out[0], out[1], out[2], out[3]);
#endif
}
#if defined(__clang__) || defined(__GNUC__)
__attribute__((target("xsave")))
#endif
static unsigned long long Xcr0() {
#if defined(_MSC_VER)
  return _xgetbv(0);
#else
  unsigned lo, hi;
  __asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  return (static_cast<unsigned long long>(hi) << 32) | lo;
#endif
}
#endif

int main() {
  bool v2 = false, v3 = false;
#ifdef LEGO_PROBE_X64
  unsigned data[4]; Cpuid(0, 0, data); const unsigned max_basic = data[0];
  Cpuid(0x80000000, 0, data); const unsigned max_extended = data[0];
  if (max_basic >= 1 && max_extended >= 0x80000001) {
    Cpuid(1, 0, data); const unsigned ecx = data[2];
    const unsigned v2_mask = (1u<<0)|(1u<<9)|(1u<<13)|(1u<<19)|(1u<<20)|(1u<<23);
    Cpuid(0x80000001, 0, data); const unsigned extended_ecx = data[2];
    v2 = (ecx & v2_mask) == v2_mask && (extended_ecx & 1);
    const unsigned avx_mask = (1u<<12)|(1u<<22)|(1u<<26)|(1u<<27)|(1u<<28)|(1u<<29);
    const bool avx_os = (ecx & avx_mask) == avx_mask && (Xcr0() & 6) == 6;
    if (max_basic >= 7) {
      Cpuid(7, 0, data); const unsigned bmi_avx2 = (1u<<3)|(1u<<5)|(1u<<8);
      v3 = v2 && avx_os && (extended_ecx & (1u<<5)) && (data[1] & bmi_avx2) == bmi_avx2;
    }
  }
#endif
  std::printf("{\n  \"game_launched\": false,\n  \"cpu_x86_64_v2\": %s,\n  \"cpu_x86_64_v3\": %s,\n",
      Bool(v2), Bool(v3));
#if defined(_WIN32)
  using Microsoft::WRL::ComPtr;
  const auto wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version");
  std::printf("  \"platform\": %s,\n  \"adapters\": [", Json(wine ? "Wine/Proton (Windows API)" : "Windows").c_str());
  ComPtr<IDXGIFactory4> factory;
  const HRESULT factory_hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  bool comma = false;
  if (SUCCEEDED(factory_hr)) for (unsigned index = 0;; ++index) {
    ComPtr<IDXGIAdapter1> adapter;
    const HRESULT enum_hr = factory->EnumAdapters1(index, &adapter);
    if (enum_hr == DXGI_ERROR_NOT_FOUND) break;
    if (FAILED(enum_hr)) break;
    DXGI_ADAPTER_DESC1 desc{}; if (FAILED(adapter->GetDesc1(&desc))) continue;
    const int length = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
    std::string name(length > 0 ? length : 1, '\0');
    if (length > 0) WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name.data(), length, nullptr, nullptr);
    name.resize(name.size() - 1);
    ComPtr<ID3D12Device> device;
    const HRESULT create_hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    ComPtr<ID3D12Device8> device8;
    const bool has_device8 = device && SUCCEEDED(device.As(&device8));
    const auto support = legodimensions::gpu_native::QueryD3D12Support(device.Get());
    ComPtr<IDXGIAdapter3> adapter3;
    DXGI_QUERY_VIDEO_MEMORY_INFO budget{};
    const bool has_budget = SUCCEEDED(adapter.As(&adapter3)) &&
        SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &budget));
    std::printf("%s\n    {\"name\": %s, \"vendor_id\": %u, \"device_id\": %u, \"software\": %s, "
        "\"dedicated_bytes\": %llu, \"shared_bytes\": %llu, \"budget_query_ok\": %s, \"local_budget_bytes\": %llu, "
        "\"d3d12_hresult\": %u, \"device8\": %s, \"shader_model\": %u, \"binding_tier\": %u, "
        "\"wave_ops\": %s, \"wave_min\": %u, \"wave_max\": %u, \"native_fp16\": %s, \"uma\": %s, "
        "\"native_feature_floor_met\": %s, \"fsr3_feature_floor_met\": %s}",
        comma ? "," : "", Json(name).c_str(), desc.VendorId, desc.DeviceId, Bool(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE),
        static_cast<unsigned long long>(desc.DedicatedVideoMemory), static_cast<unsigned long long>(desc.SharedSystemMemory),
        Bool(has_budget), static_cast<unsigned long long>(budget.Budget), unsigned(create_hr), Bool(has_device8),
        support.shader_model, support.binding_tier, Bool(support.wave_ops), support.wave_min, support.wave_max,
        Bool(support.native_fp16), Bool(support.uma), Bool(has_device8 && support.BaseReady()), Bool(support.Fsr3Ready()));
    comma = true;
  }
  std::printf("\n  ],\n  \"factory_hresult\": %u,\n", unsigned(factory_hr));
#else
  std::printf("  \"platform\": \"non-Windows\",\n  \"native_renderer_available\": false,\n");
#endif
  std::puts("  \"gameplay_validated\": false,\n  \"note\": \"Feature checks only. No performance, driver correctness or Linux/Proton acceptance is implied. Current game executable requires x86-64-v3; its SDK requires v2.\"\n}");
}
