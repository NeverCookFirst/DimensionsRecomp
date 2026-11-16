#include "cpu-alias-prefix.inc"
#include "make-image.inc"

std::shared_ptr<TextureResource> CpuView(const Image& image,u32 kind){
 auto reader=std::make_shared<TextureResource>();reader->guest_address=0x80008000;
 reader->guest_fetch=image.resource->guest_fetch;reader->resolved_on_host=kind==1;
 reader->guest_backing_current=kind==1;reader->texture.reset();
 if(kind==2){reader->levels=2;reader->guest_fetch.mip_address=reader->guest_fetch.base_address;reader->guest_fetch.base_address=128;}
 if(kind==3){reader->owns_guest_memory=true;reader->mirror_address=reader->guest_fetch.base_address<<12;reader->mirror_size=32768;}
 g_textures[reader->guest_address]=reader;return reader;
}
int main(){
 coordinator=std::this_thread::get_id();auto api=plume::CreateD3D12Interface();device=api->createDevice();CHECK(device);
 queue=device->createCommandQueue(plume::RenderCommandListType::DIRECT);commands=queue->createCommandList();fence=device->createCommandFence();
 rex::graphics::texture_util::layout={{32768,512},32768};u32 cases=0;
 for(u32 format=0;format<3;++format)for(u32 scale:{2u,3u,4u})for(bool tiled:{false,true})
 for(u32 endian=0;endian<4;++endian)for(bool partial:{false,true})for(u32 kind=0;kind<4;++kind)for(bool later:{false,true}){
  g_textures.clear();memory.data.assign(memory.data.size(),0x55);auto expected=memory.data;
  commands->begin();open=true;auto image=MakeImage(0,format,scale,tiled,endian,partial,expected);
  auto reader=CpuView(image,kind);const u32 base=image.resource->guest_fetch.base_address<<12;
  if(later)expected[base]=0x55^0x80;
  during_wait=[&]{if(later)memory.data[base]^=0x80;};const auto before=readbacks;
  Recorder([&]{CHECK(MaterializeTextureForCpuRead(reader->guest_address));});
  CHECK(readbacks==before+1&&image.resource->guest_backing_current);BytesMatch(expected);
  // Reading the now-current alias again does not submit another readback.
  Recorder([&]{CHECK(!MaterializeTextureForCpuRead(reader->guest_address));});CHECK(readbacks==before+1);
  if(open)Finish();during_wait={};retained.clear();++cases;
 }
 // A base/mip reader may depend on two separate GPU allocations. One
 // transaction must validate and publish both, with just one fence wait.
 for(u32 failure=0;failure<5;++failure){
  g_textures.clear();memory.data.assign(memory.data.size(),0x55);auto expected=memory.data;
  commands->begin();open=true;auto a=MakeImage(0,0,2,false,0,false,expected);auto b=MakeImage(1,1,3,true,3,true,expected);
  auto reader=CpuView(a,0);reader->levels=2;reader->guest_fetch.mip_address=b.resource->guest_fetch.base_address;
  const auto original=memory.data;const auto before=readbacks;
  if(failure==1)b.resource->levels=2;
  if(failure==2)during_wait=[&]{invalid_header=b.resource->guest_address;};
  if(failure==3)wait_ok=false;
  if(failure==4)invalid_header=reader->guest_address;
  Recorder([&]{CHECK(MaterializeTextureForCpuRead(reader->guest_address)==(failure==0));});
  if(!failure){BytesMatch(expected);CHECK(readbacks==before+1);}
  else {CHECK(memory.data==original&&!a.resource->guest_backing_current&&!b.resource->guest_backing_current);
   CHECK(readbacks==before+((failure==2||failure==3)?1:0));}
  if(failure==3)CHECK(retained.size()==2);
  if(open)Finish();during_wait={};wait_ok=true;invalid_header=0;retained.clear();++cases;
 }
 const auto before=readbacks;CHECK(!MaterializeTextureForCpuRead(0));CHECK(readbacks==before);
 printf("PASS physical CPU-lock aliases: %u cases; base/mip/mirror/current views, raw formats, endian, scale, partial coverage, later CPU writes, one wait and atomic failures\n",cases);
}
