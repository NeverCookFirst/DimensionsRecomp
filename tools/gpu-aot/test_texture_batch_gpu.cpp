// Reuse the windowless fixture environment, with actual production functions
// extracted by the driver. No game executable is involved.
#include "batch-fixture-prefix.inc"

struct Image {std::shared_ptr<TextureResource> resource;std::unique_ptr<plume::RenderBuffer> upload;};
Image MakeImage(u32 number,u32 format,u32 scale,bool tiled,u32 endian,bool partial,std::vector<u8>& expected){
 auto r=std::make_shared<TextureResource>();auto& f=r->guest_fetch;
 r->guest_address+=number*0x100;r->scale_twice=scale;f.base_address=32+number*16;
 f.tiled=tiled;f.endianness=endian;f.swizzle=0x688;
 if(format==1){r->format=plume::RenderFormat::R16G16_UNORM;f.format=rex::graphics::xenos::TextureFormat::k_16_16;}
 if(format==2){r->format=plume::RenderFormat::R16G16B16A16_UNORM;f.format=rex::graphics::xenos::TextureFormat::k_16_16_16_16;}
 const u32 bpt=format==2?8:4,base=f.base_address<<12;
 plume::RenderTextureDesc td;td.dimension=plume::RenderTextureDimension::TEXTURE_2D;
 td.width=r->HostWidth();td.height=r->HostHeight();td.depth=td.arraySize=td.mipLevels=1;td.format=r->format;
 r->texture=device->createTexture(td);CHECK(r->texture);
 auto* image=static_cast<plume::D3D12Texture*>(r->texture.get())->d3d;
 auto desc=image->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes=0;
 static_cast<plume::D3D12Device*>(device.get())->d3d->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
 auto upload=device->createBuffer(plume::RenderBufferDesc::UploadBuffer(bytes));CHECK(upload);
 auto* pixels=static_cast<u8*>(upload->map());CHECK(pixels);std::memset(pixels,0xee,size_t(bytes));
 const u32 covered=partial?r->HostHeight()/2:r->HostHeight();
 auto value=[&](u32 x,u32 y,u32 c){return u8(number*43+x*17+y*29+c*61);};
 for(u32 y=0;y<covered;++y)for(u32 x=0;x<r->HostWidth();++x)for(u32 c=0;c<bpt;++c)
  pixels[fp.Offset+size_t(y)*fp.Footprint.RowPitch+x*bpt+c]=value(x,y,c);
 upload->unmap();
 const u32 endian_xor=endian==1?1:endian==2?3:endian==3?2:0;
 for(u32 y=0;y<r->height;++y)for(u32 x=0;x<r->width;++x){
  const u32 sx=(x*r->HostWidth()+r->HostWidth()/2)/r->width;
  const u32 sy=(y*r->HostHeight()+r->HostHeight()/2)/r->height;
  if(sy>=covered)continue;
  const u32 at=tiled?rex::graphics::texture_util::GetTiledOffset2D(x,y,512/bpt,bpt==8?3:2):y*512+x*bpt;
  for(u32 c=0;c<bpt;++c)expected[base+((at+c)^endian_xor)]=value(sx,sy,c);
 }
 commands->barriers(plume::RenderBarrierStage::COPY,plume::RenderTextureBarrier(r->texture.get(),plume::RenderTextureLayout::COPY_DEST));
 D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=static_cast<plume::D3D12Buffer*>(upload.get())->d3d;
 src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;dst.pResource=image;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
 static_cast<plume::D3D12CommandList*>(commands.get())->d3d->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
 commands->barriers(plume::RenderBarrierStage::GRAPHICS,plume::RenderTextureBarrier(r->texture.get(),plume::RenderTextureLayout::SHADER_READ));
 ResolveRegion area{};area.width=r->HostWidth();area.height=covered;RecordResolvedTexture(*r,area,0,0);
 g_textures.emplace(r->guest_address,r);return {r,std::move(upload)};
}
template<class F>void Recorder(F run){std::thread worker(std::move(run));worker.join();}
void BytesMatch(const std::vector<u8>& expected){
 for(size_t i=0;i<expected.size();++i)if(memory.data[i]!=expected[i]){
  std::fprintf(stderr,"byte=%zu actual=%u expected=%u\n",i,memory.data[i],expected[i]);CHECK(false);
 }
}
int main(){
 coordinator=std::this_thread::get_id();auto api=plume::CreateD3D12Interface();device=api->createDevice();CHECK(device);
 queue=device->createCommandQueue(plume::RenderCommandListType::DIRECT);commands=queue->createCommandList();fence=device->createCommandFence();
 rex::graphics::texture_util::layout={{32768,512},0};u32 cases=0;
 for(u32 mode=0;mode<4;++mode)for(u32 scale:{2u,3u,4u})for(bool tiled:{false,true})for(u32 endian=0;endian<4;++endian)for(bool partial:{false,true})for(bool later:{false,true}){
  g_textures.clear();g_pool_batch={};memory.data.assign(memory.data.size(),0x55);auto expected=memory.data;
  commands->begin();open=true;
  auto a=MakeImage(0,0,scale,tiled,endian,partial,expected);
  auto b=MakeImage(1,1,scale,!tiled,endian,partial,expected);
  auto c=MakeImage(2,2,scale,tiled,endian,partial,expected);
  const u32 ba=a.resource->guest_fetch.base_address<<12,bb=b.resource->guest_fetch.base_address<<12,bc=c.resource->guest_fetch.base_address<<12;
  const auto before=readbacks;const auto old=memory.data;cpu_write_during_wait=false;
  during_wait=[&]{if(later){memory.data[ba]^=0x80;memory.data[bb]^=0x80;memory.data[bc]^=0x80;}};
  if(later){expected[ba]=expected[bb]=expected[bc]=0x55^0x80;}
  const u32 bytes=mode==1?bb+32768-ba:mode==2?32768:4096;
  const PhysicalCopyRange source{ba,bytes};
  const PhysicalCopyRange destination{mode==1?0x90000u:mode==2?bc:bb+4096,bytes};
  if(mode==3){
   // Duplicate owners must be read once. All outputs merge in one transaction.
   std::array owners{c.resource,a.resource,b.resource,a.resource};
   Recorder([&]{CHECK(MaterializeResolvedTextures(owners));});
   CHECK(readbacks==before+1);BytesMatch(expected);
  }else{
   // Restore expectations for resources untouched by the dependency set.
   std::copy(old.begin()+bc,old.begin()+bc+32768,expected.begin()+bc);
   if(mode==2)std::copy(old.begin()+bb,old.begin()+bb+32768,expected.begin()+bb);
   if(later){expected[ba]=expected[bb]=expected[bc]=0x55^0x80;}
   std::memmove(expected.data()+destination.address,expected.data()+source.address,bytes);
   PoolCopyRecorded(destination.address,source.address,bytes,source,destination);
   FlushPoolCopyBatch();CHECK(readbacks==before+1);BytesMatch(expected);
  }
  if(open)Finish();during_wait={};retained.clear();++cases;
 }
 // Failure transactions must leave every guest byte and resource authoritative.
 for(u32 failure=0;failure<7;++failure){
  g_textures.clear();memory.data.assign(memory.data.size(),0x55);auto expected=memory.data;
  commands->begin();open=true;auto a=MakeImage(0,0,2,false,0,false,expected);auto b=MakeImage(1,1,2,true,0,false,expected);
  const auto old=memory.data;const auto before=readbacks;std::array owners{a.resource,b.resource};
  if(failure==0)b.resource->guest_fetch.base_address=a.resource->guest_fetch.base_address;
  if(failure==1)b.resource->levels=2;
  if(failure==2)during_wait=[&]{invalid_header=b.resource->guest_address;};
  if(failure==3)wait_ok=false;
  if(failure==6)physical_window_ok=false;
  if(failure==4){
   auto last=std::less<const TextureResource*>{}(a.resource.get(),b.resource.get())?b.resource:a.resource;
   last->guest_fetch.tiled=0;last->guest_fetch.packed_mips=1;rex::graphics::texture_util::packed_offset=32768;
  }
  if(failure!=5){Recorder([&]{const bool ok=MaterializeResolvedTextures(owners);
    if(ok)std::fprintf(stderr,"unexpected success for failure case %u\n",failure);CHECK(!ok);});CHECK(memory.data==old);
   CHECK(!a.resource->guest_backing_current&&!b.resource->guest_backing_current);
   CHECK(readbacks==before+(failure>=2?1:0));if(failure==3)CHECK(retained.size()==2);
  }else{
   // Unsupported requests in the lower-level API are validated before commands.
   TextureReadbackRequest requests[]={{a.resource->texture.get(),64,32,TextureReadbackFormat::kRgba8},
       {b.resource->texture.get(),999,32,TextureReadbackFormat::kRg16}};
   std::vector<std::vector<u8>> out{{1,2,3}};u32 waits=0;
   CHECK(!ReadbackTexturesRaw(device.get(),commands.get(),requests,out,[&]{++waits;return false;},[](auto){CHECK(false);}));
   CHECK((waits==0&&out==std::vector<std::vector<u8>>{{1,2,3}}));
  }
  if(open)Finish();retained.clear();wait_ok=true;physical_window_ok=true;invalid_header=0;during_wait={};rex::graphics::texture_util::packed_offset=0;++cases;
 }
 std::printf("PASS multi-texture transactions: %u cases; one wait, mixed raw formats, later CPU writes, overlap/preflight/header/wait failures\n",cases);
}
