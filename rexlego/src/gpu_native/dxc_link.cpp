/**
 * @file    gpu/shaders/dxc_link.cpp
 * @brief   Isolated COM TU. No engine headers, only <unknwn.h> and
 *          <dxcapi.h>.
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu_native/dxc_link.h"

#include <unknwn.h>

#include <dxcapi.h>

#include <cstdio>

namespace legodimensions::gpu_native {
namespace {

template <typename T> class Com {
public:
  Com() = default;
  ~Com() {
    if (p_)
      p_->Release();
  }
  Com(const Com &) = delete;
  Com &operator=(const Com &) = delete;
  T **put() { return &p_; }
  T *operator->() const { return p_; }
  T *get() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }

private:
  T *p_ = nullptr;
};

std::vector<uint8_t> ToVector(IDxcBlob *blob) {
  const auto *bytes = static_cast<const uint8_t *>(blob->GetBufferPointer());
  return std::vector<uint8_t>(bytes, bytes + blob->GetBufferSize());
}

} // namespace

std::vector<uint8_t> CompileSpecConstantLib(uint32_t value) {
  Com<IDxcCompiler3> compiler;
  if (FAILED(
          DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(compiler.put())))) {
    return {};
  }

  char hlsl[128];
  const int len =
      std::snprintf(hlsl, sizeof(hlsl),
                    "export uint g_SpecConstants() { return %u; }", value);
  DxcBuffer buffer{};
  buffer.Ptr = hlsl;
  buffer.Size = static_cast<SIZE_T>(len);
  buffer.Encoding = DXC_CP_ACP;

  const wchar_t *args[] = {L"-T", L"lib_6_3"};
  Com<IDxcResult> result;
  if (FAILED(compiler->Compile(&buffer, args, 2, nullptr,
                               IID_PPV_ARGS(result.put()))) ||
      !result) {
    return {};
  }

  Com<IDxcBlob> blob;
  if (FAILED(result->GetResult(blob.put())) || !blob ||
      blob->GetBufferSize() == 0) {
    return {};
  }
  return ToVector(blob.get());
}

std::vector<uint8_t> LinkSpecConstantLib(const uint8_t *libraryDxil,
                                         uint32_t libraryDxilSize,
                                         const uint8_t *specLib,
                                         size_t specLibSize,
                                         const wchar_t *profile) {
  if (!libraryDxil || libraryDxilSize == 0 || !specLib || specLibSize == 0)
    return {};

  Com<IDxcUtils> utils;
  if (FAILED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(utils.put()))))
    return {};

  Com<IDxcBlobEncoding> specBlob;
  Com<IDxcBlobEncoding> shaderBlob;
  if (FAILED(utils->CreateBlobFromPinned(specLib,
                                         static_cast<UINT32>(specLibSize),
                                         DXC_CP_ACP, specBlob.put())) ||
      FAILED(utils->CreateBlobFromPinned(libraryDxil, libraryDxilSize,
                                         DXC_CP_ACP, shaderBlob.put()))) {
    return {};
  }

  Com<IDxcLinker> linker;
  if (FAILED(DxcCreateInstance(CLSID_DxcLinker, IID_PPV_ARGS(linker.put()))))
    return {};

  linker->RegisterLibrary(L"SpecConstants", specBlob.get());
  linker->RegisterLibrary(L"Shader", shaderBlob.get());
  const wchar_t *libNames[] = {L"SpecConstants", L"Shader"};

  Com<IDxcOperationResult> result;
  if (FAILED(linker->Link(L"main", profile, libNames, 2, nullptr, 0,
                          result.put())) ||
      !result) {
    return {};
  }
  // Link reports a failed link through the operation status, not the HRESULT.
  HRESULT status = E_FAIL;
  if (FAILED(result->GetStatus(&status)) || FAILED(status))
    return {};

  Com<IDxcBlob> linked;
  if (FAILED(result->GetResult(linked.put())) || !linked ||
      linked->GetBufferSize() == 0) {
    return {};
  }
  return ToVector(linked.get());
}

}  // namespace legodimensions::gpu_native
