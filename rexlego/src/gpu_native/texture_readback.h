#pragma once
#include "gpu_native/native_recording.h"
#include "gpu_native/texture_readback_format.h"
#include <plume_d3d12.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace legodimensions::gpu_native {
// Only used for an explicit guest data dependency, not ordinary drawing.
// Finish must submit and wait for these commands. Retain keeps their allocation
// alive if that wait fails after the copy was recorded.
struct TextureReadbackRequest {
  plume::RenderTexture* texture;
  uint32_t width,height;
  TextureReadbackFormat format;
};
constexpr DXGI_FORMAT TextureReadbackDxgiFormat(TextureReadbackFormat format) {
  switch(format) {
    case TextureReadbackFormat::kRgba8: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case TextureReadbackFormat::kRg16: return DXGI_FORMAT_R16G16_UNORM;
    case TextureReadbackFormat::kRgba16: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case TextureReadbackFormat::kR16Float: return DXGI_FORMAT_R16_FLOAT;
    case TextureReadbackFormat::kRg16Float: return DXGI_FORMAT_R16G16_FLOAT;
    case TextureReadbackFormat::kRgba16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case TextureReadbackFormat::kR32Float: return DXGI_FORMAT_R32_FLOAT;
    case TextureReadbackFormat::kRg32Float: return DXGI_FORMAT_R32G32_FLOAT;
    case TextureReadbackFormat::kRgba32Float: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}
template<class Finish,class Retain>
bool ReadbackTexturesRaw(plume::RenderDevice* device,plume::RenderCommandList* commands,
    std::span<const TextureReadbackRequest> requests,std::vector<std::vector<uint8_t>>& pixels,
    Finish finish,Retain retain) {
  if (!device || !commands) return false;
  struct Pending {
    std::shared_ptr<plume::RenderBuffer> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    uint32_t row_bytes,height;
  };
  std::vector<Pending> pending;
  pending.reserve(requests.size());
  // Allocate and validate every request before recording any command.
  for (const auto& request:requests) {
    const auto [texture,width,height,format]=request;
    const auto expected_format=TextureReadbackDxgiFormat(format);
    if (!texture || expected_format==DXGI_FORMAT_UNKNOWN) return false;
    auto* raw=static_cast<plume::D3D12Texture*>(texture)->d3d;
    if (!raw) return false;
    const auto desc=raw->GetDesc();
    const uint32_t texel_bytes = TextureReadbackTexelBytes(format);
    if (desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize!=1 ||
        desc.SampleDesc.Count!=1 || desc.Format!=expected_format ||
        !desc.Width || !desc.Height || desc.Width!=width || desc.Height!=height ||
        desc.Width>4096 || desc.Height>4096) return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes=0;
    static_cast<plume::D3D12Device*>(device)->d3d->GetCopyableFootprints(
        &desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
    if (!bytes || bytes>128ull*1024*1024) return false;
    auto readback=std::shared_ptr<plume::RenderBuffer>(device->createBuffer(
        plume::RenderBufferDesc::ReadbackBuffer(bytes)).release());
    if (!readback || !static_cast<plume::D3D12Buffer*>(readback.get())->d3d) return false;
    pending.push_back({std::move(readback),footprint,width*texel_bytes,height});
  }
  struct InFlight {
    std::vector<Pending>& pending;Retain& retain;
    bool recorded=false,completed=false;
    ~InFlight(){if(recorded&&!completed)for(auto& item:pending)retain(std::move(item.buffer));}
  } lifetime{pending,retain};
  lifetime.recorded=!requests.empty();
  for (size_t i=0;i<requests.size();++i) {
    const auto texture=requests[i].texture;
    const auto& item=pending[i];
    commands->setFramebuffer(nullptr);
    commands->barriers(plume::RenderBarrierStage::COPY,
        plume::RenderTextureBarrier(texture,plume::RenderTextureLayout::COPY_SOURCE));
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};
    src.pResource=static_cast<plume::D3D12Texture*>(texture)->d3d;
    src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=static_cast<plume::D3D12Buffer*>(item.buffer.get())->d3d;
    dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=item.footprint;
    RecordNativeCommand(commands, [dst, src](auto* list) {
      static_cast<plume::D3D12CommandList*>(list)->d3d->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    });
    commands->barriers(plume::RenderBarrierStage::GRAPHICS,
        plume::RenderTextureBarrier(texture,plume::RenderTextureLayout::SHADER_READ));
  }
  if (!requests.empty() && !finish()) return false;
  lifetime.completed=true;
  std::vector<std::vector<uint8_t>> result;
  result.reserve(pending.size());
  for (const auto& item:pending) {
    result.emplace_back(size_t(item.row_bytes)*item.height);
    const auto* mapped=static_cast<const uint8_t*>(item.buffer->map());
    if (!mapped) return false;
    for (uint32_t y=0;y<item.height;++y)
      std::memcpy(result.back().data()+size_t(y)*item.row_bytes,
          mapped+item.footprint.Offset+size_t(y)*item.footprint.Footprint.RowPitch,item.row_bytes);
    item.buffer->unmap();
  }
  pixels=std::move(result);return true;
}
template<class Finish,class Retain>
bool ReadbackTextureRaw(plume::RenderDevice* device,
    plume::RenderCommandList* commands,plume::RenderTexture* texture,
    uint32_t width,uint32_t height,TextureReadbackFormat format,
    std::vector<uint8_t>& pixels,Finish finish,Retain retain) {
  const TextureReadbackRequest request{texture,width,height,format};
  std::vector<std::vector<uint8_t>> result;
  if(!ReadbackTexturesRaw(device,commands,{&request,1},result,std::move(finish),std::move(retain)))return false;
  pixels=std::move(result.front());return true;
}
template<class Finish,class Retain>
bool ReadbackTextureRgba8(plume::RenderDevice* device,
    plume::RenderCommandList* commands, plume::RenderTexture* texture,
    uint32_t width, uint32_t height, std::vector<uint8_t>& pixels, Finish finish, Retain retain) {
  return ReadbackTextureRaw(device,commands,texture,width,height,TextureReadbackFormat::kRgba8,
      pixels,std::move(finish),std::move(retain));
}
} // namespace legodimensions::gpu_native
