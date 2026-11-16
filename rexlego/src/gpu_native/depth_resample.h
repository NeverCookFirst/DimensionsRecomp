#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>
#include <span>

namespace legodimensions::gpu_native {
// GPU-only nearest-center resize of both D32F/S8 planes. Caller transitions
// source to shader-read and destination to depth-write, and retains the returned
// immutable binding lease until the recording's fence completes. No CPU wait.
class DepthResampler {
 public:
  static std::unique_ptr<DepthResampler> Create(ID3D12Device*,
      std::span<const unsigned char> vertex,
      std::span<const unsigned char> depth_stencil,
      std::span<const unsigned char> depth_only,
      std::span<const unsigned char> stencil_bit,
      bool force_portable = false);
  std::shared_ptr<void> Record(ID3D12GraphicsCommandList*,
      ID3D12Resource* source, ID3D12Resource* destination);
  static bool Supports(ID3D12Resource* source, ID3D12Resource* destination);
  bool UsesPixelStencilReference() const {return pixel_stencil_reference_;}
 private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> combined_, depth_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> stencil_[8];
  bool pixel_stencil_reference_ = false;
};
}
