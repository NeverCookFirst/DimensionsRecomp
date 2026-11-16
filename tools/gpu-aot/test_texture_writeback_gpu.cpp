// Windowless production readback -> tiled guest writeback -> pool relocation.
#define NOMINMAX
#include "gpu_native/texture_readback.h"
#include "gpu_native/texture_writeback.h"
#include "gpu_native/texture_volume_upload.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>
namespace plume {std::unique_ptr<RenderInterface> CreateD3D12Interface();}
namespace rex {constexpr uint32_t align(uint32_t a,uint32_t n){return (a+n-1)&~(n-1);}}
namespace xenos {constexpr uint32_t kTextureTileWidthHeight=32;}
// Verbatim SDK function emitted by the driver script, no independent tiler.
#include "tile-address.inc"
using namespace legodimensions::gpu_native;
#define CHECK(c) do{if(!(c)){std::fprintf(stderr,"failed line%d: %s\n",__LINE__,#c);return 23;}}while(0)
int main(int argc,char**argv){
 const bool mutant=argc>1;
 auto api=plume::CreateD3D12Interface();auto device=api->createDevice();CHECK(device);
 auto* raw=static_cast<plume::D3D12Device*>(device.get())->d3d;
 auto queue=device->createCommandQueue(plume::RenderCommandListType::DIRECT);
 auto commands=queue->createCommandList();auto fence=device->createCommandFence();
 auto finish=[&]{commands->end();queue->executeCommandLists(commands.get(),fence.get());queue->waitForCommandFence(fence.get());return true;};
 std::vector<std::shared_ptr<plume::RenderBuffer>> retained;
 auto retain=[&](auto buffer){retained.push_back(std::move(buffer));};
 unsigned readbacks=0,cases=0;
 for(auto format:{TextureReadbackFormat::kRgba8,TextureReadbackFormat::kRg16,TextureReadbackFormat::kRgba16,
     TextureReadbackFormat::kR16Float,TextureReadbackFormat::kRg16Float,TextureReadbackFormat::kRgba16Float,
     TextureReadbackFormat::kR32Float,TextureReadbackFormat::kRg32Float,TextureReadbackFormat::kRgba32Float})
 for(auto wh:{std::pair{17u,13u},std::pair{256u,256u},std::pair{67u,33u}}){
  const auto [w,h]=wh;
  const auto texel_bytes=TextureReadbackTexelBytes(format);
  const bool rgba8=format==TextureReadbackFormat::kRgba8;
  plume::RenderTextureDesc desc;desc.dimension=plume::RenderTextureDimension::TEXTURE_2D;
  desc.width=w;desc.height=h;desc.depth=desc.arraySize=desc.mipLevels=1;
  switch(format){
   case TextureReadbackFormat::kRgba8:desc.format=plume::RenderFormat::R8G8B8A8_UNORM;break;
   case TextureReadbackFormat::kRg16:desc.format=plume::RenderFormat::R16G16_UNORM;break;
   case TextureReadbackFormat::kRgba16:desc.format=plume::RenderFormat::R16G16B16A16_UNORM;break;
   case TextureReadbackFormat::kR16Float:desc.format=plume::RenderFormat::R16_FLOAT;break;
   case TextureReadbackFormat::kRg16Float:desc.format=plume::RenderFormat::R16G16_FLOAT;break;
   case TextureReadbackFormat::kRgba16Float:desc.format=plume::RenderFormat::R16G16B16A16_FLOAT;break;
   case TextureReadbackFormat::kR32Float:desc.format=plume::RenderFormat::R32_FLOAT;break;
   case TextureReadbackFormat::kRg32Float:desc.format=plume::RenderFormat::R32G32_FLOAT;break;
   case TextureReadbackFormat::kRgba32Float:desc.format=plume::RenderFormat::R32G32B32A32_FLOAT;break;
   default:CHECK(false);
  }
  auto texture=device->createTexture(desc);CHECK(texture);
  auto* image=static_cast<plume::D3D12Texture*>(texture.get())->d3d;CHECK(image);
  const auto td=image->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 total=0;
  raw->GetCopyableFootprints(&td,0,1,0,&footprint,nullptr,nullptr,&total);
  auto upload=device->createBuffer(plume::RenderBufferDesc::UploadBuffer(total));CHECK(upload);
  auto* mapped=static_cast<uint8_t*>(upload->map());CHECK(mapped);std::memset(mapped,0x5b,size_t(total));
  std::vector<uint8_t> expected(size_t(w)*h*texel_bytes);
  for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)for(unsigned c=0;c<texel_bytes;++c){
   // 256x256 covers every 16-bit value in every numeric channel.
   constexpr uint32_t float_words[]={0,0x80000000,1,0x007fffff,0x00800000,0x3f800000,0xbf800000,
      0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc12345,0x7f812345,0xffffffff};
   const bool float32=format==TextureReadbackFormat::kR32Float||format==TextureReadbackFormat::kRg32Float||format==TextureReadbackFormat::kRgba32Float;
   const auto value=rgba8?uint8_t(x*17+y*29+c*61):float32?
      uint8_t(float_words[(y*w+x+c/4)%std::size(float_words)]>>(8*(c&3))):
      uint8_t(((y*w+x+c/2*6173)&65535)>>(8*(c&1)));
   mapped[footprint.Offset+size_t(y)*footprint.Footprint.RowPitch+x*texel_bytes+c]=value;
   expected[(y*w+x)*texel_bytes+c]=value;
  }
  upload->unmap();commands->begin();
  commands->barriers(plume::RenderBarrierStage::COPY,plume::RenderTextureBarrier(texture.get(),plume::RenderTextureLayout::COPY_DEST));
  D3D12_TEXTURE_COPY_LOCATION src{},dst{};
  src.pResource=static_cast<plume::D3D12Buffer*>(upload.get())->d3d;src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprint;
  dst.pResource=image;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  static_cast<plume::D3D12CommandList*>(commands.get())->d3d->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  commands->barriers(plume::RenderBarrierStage::GRAPHICS,plume::RenderTextureBarrier(texture.get(),plume::RenderTextureLayout::SHADER_READ));
  std::vector<uint8_t> pixels;
  if(rgba8){CHECK(ReadbackTextureRgba8(device.get(),commands.get(),texture.get(),w,h,pixels,finish,retain));}
  else{CHECK(ReadbackTextureRaw(device.get(),commands.get(),texture.get(),w,h,format,pixels,finish,retain));}
  CHECK(pixels==expected&&retained.empty());++readbacks;
  const unsigned pitch=((w+31)&~31u)*texel_bytes;
  for(bool tiled:{false,true})for(unsigned endian=0;endian<4;++endian)for(bool swap:{false,true}){
   if(!rgba8&&swap)continue;
   auto offset=[&](unsigned x,unsigned y)->int64_t{return tiled?GetTiledOffset2D(x,y,pitch/texel_bytes,TextureReadbackTexelLog2(format)):y*pitch+x*texel_bytes;};
   unsigned extent=0;
   for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)extent=std::max(extent,unsigned(offset(x,y))+texel_bytes);
   extent=(extent+4095)&~4095u;
   std::vector<uint8_t> guest(size_t(extent)+8192,0x5a);
   CHECK(WritebackTextureRaw(pixels,w,h,w*texel_bytes,{guest.data()+16,extent},mutant?0:endian,swap,offset,w,h,nullptr,texel_bytes));
   // Same backwards 4KiB overlapping traversal as the captured TU23 compactor.
   for(unsigned end=extent;end;end-=4096)std::memmove(guest.data()+4112+end-4096,guest.data()+16+end-4096,4096);
   std::vector<uint8_t> decoded(size_t(w)*h*texel_bytes);
   CHECK(CopyTextureVolumeBlocks({guest.data()+4112,extent},decoded,w,h,1,texel_bytes,w*texel_bytes,endian,
       [&](auto x,auto y,auto)->int64_t{return offset(x,y);}));
   for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)for(unsigned c=0;c<texel_bytes;++c)
    CHECK(decoded[(y*w+x)*texel_bytes+c]==expected[(y*w+x)*texel_bytes+(swap&&(c==0||c==2)?2-c:c)]);
   ++cases;
  }
  // No output mutation on a rejected size, and no release of an in-flight copy
  // when the caller cannot establish fence completion.
  commands->begin();pixels.clear();
  CHECK(ReadbackTextureRaw(device.get(),commands.get(),texture.get(),w,h,format,pixels,finish,retain));
  CHECK(pixels==expected);++readbacks; // source GPU pixels survived writeback
  commands->begin();pixels={0x77};auto previous=pixels;
  CHECK(!ReadbackTextureRaw(device.get(),commands.get(),texture.get(),w+1,h,format,pixels,finish,retain));
  CHECK(pixels==previous&&retained.empty());
  CHECK(!ReadbackTextureRaw(device.get(),commands.get(),texture.get(),w,h,TextureReadbackFormat(999),pixels,finish,retain));
  const auto wrong_format=rgba8?TextureReadbackFormat::kRg16:TextureReadbackFormat::kRgba8;
  CHECK(!ReadbackTextureRaw(device.get(),commands.get(),texture.get(),w,h,wrong_format,pixels,finish,retain));
  CHECK(pixels==previous&&retained.empty());
  CHECK(!ReadbackTextureRaw(device.get(),commands.get(),texture.get(),w,h,format,pixels,[]{return false;},retain));
  CHECK(pixels==previous&&retained.size()==1);finish();retained.clear();
 }
 printf("PASS %u GPU readbacks, %u tiled/linear/endian/channel/overlapping relocation cases, nine raw formats, all 65536 half words and float32 special bits, failed-wait retention\n",readbacks,cases);
 return 0;
}
