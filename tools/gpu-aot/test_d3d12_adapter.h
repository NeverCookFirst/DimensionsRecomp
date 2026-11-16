#pragma once
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdlib>
#include <string>

// Fixture-only selection. An explicit unavailable adapter is an error, never
// silently a test on some other GPU. "warp" requests Microsoft's CPU driver.
inline HRESULT TestD3D12Adapter(IDXGIFactory4* factory, IDXGIAdapter1** output) {
  if (!factory || !output) return E_INVALIDARG;
  *output = nullptr;
  const char* requested = std::getenv("LEGO_TEST_ADAPTER");
  if (requested && std::string(requested) == "warp")
    return factory->EnumWarpAdapter(IID_PPV_ARGS(output));
  for (unsigned index = 0;; ++index) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    const auto hr = factory->EnumAdapters1(index, &adapter);
    if (FAILED(hr)) return hr;
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & (DXGI_ADAPTER_FLAG_SOFTWARE | DXGI_ADAPTER_FLAG_REMOTE))) continue;
    if (requested && *requested) {
      const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, requested, -1, nullptr, 0);
      if (!count) return E_INVALIDARG;
      std::wstring name(count, L'\0');
      if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, requested, -1, name.data(), count)) return E_INVALIDARG;
      name.resize(name.size() - 1);
      if (name != desc.Description) continue;
    }
    *output = adapter.Detach(); return S_OK;
  }
}
