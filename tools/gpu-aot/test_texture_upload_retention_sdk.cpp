#define NOMINMAX
#define XXH_INLINE_ALL
#include "gpu_native/d3d.h"
#include "gpu_native/texture_upload_cache.h"
#include "gpu_native/texture_volume_upload.h"
#include "gpu_native/texture_alpha4_upload.h"
#include "plume_render_interface_types.h"
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/system/xmemory.h>
#include <rex/math.h>
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include "source-under-test.h"
using namespace legodimensions::gpu_native;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
rex::memory::Memory*memory_pointer;
#define REX_KERNEL_MEMORY() memory_pointer
constexpr u32 kPitchAlignment=256;
struct TextureResource {u32 width=32,height=16,depth=1,levels=3,d3d_type=3;plume::RenderFormat format=plume::RenderFormat::R8G8B8A8_UNORM;};
bool IsBlockCompressedFormat(plume::RenderFormat);
u32 FormatBlockBytes(plume::RenderFormat);
#include "retained-helpers.inc"
using Fetch=rex::graphics::xenos::xe_gpu_texture_fetch_t;
bool Decode(const std::shared_ptr<TextureResource>&resource,const Fetch&fetch,
    const std::optional<TextureUploadSource>&retained,std::vector<u8>&decoded){
 namespace tu=rex::graphics::texture_util;
 using rex::graphics::xenos::DataDimension;
 const bool cube=fetch.dimension==DataDimension::kCube,volume=fetch.dimension==DataDimension::k3D;
 const u32 faces=cube?6:1;
 const auto*format_info=rex::graphics::FormatInfo::Get(fetch.format);
 const bool alpha4=fetch.format==rex::graphics::xenos::TextureFormat::k_DXT3A;
 const u32 block_bytes=alpha4?8:plume::RenderFormatSize(resource->format);
 const u32 block_width=alpha4?4:plume::RenderFormatBlockWidth(resource->format);
 const auto layout=tu::GetGuestTextureLayout(fetch.dimension,fetch.pitch,resource->width,resource->height,
     volume?resource->depth:1,fetch.tiled,fetch.format,fetch.packed_mips,true,resource->levels-1);
 const u32 endian_xor=TextureEndianXor(u32(fetch.endianness));
 u32 upload_size=0;for(u32 face=0;face<faces;++face)for(u32 level=0;level<resource->levels;++level)upload_size=AlignUp(upload_size,512)+LevelSize(*resource,level);
 decoded.assign(upload_size,0);auto*mapped=decoded.data();
#include "retained-decode.inc"
 return true;
}
u32 Physical(u32 address){return address<0x20000000u?address:REX_KERNEL_MEMORY()->GetPhysicalAddress(address);}
void Fill(u32 address,u32 bytes,u8 seed){auto*p=REX_KERNEL_MEMORY()->TranslatePhysical<u8*>(Physical(address));for(u32 i=0;i<bytes;++i)p[i]=u8(seed+(i*37)^(i>>4));}
void CheckBoundsAndBank(){
 unsigned reads=0;auto reject=[&](u32,std::span<u8>){++reads;return false;};
 CHECK(!TextureUploadSource::Capture(0,4,0,0,reject));CHECK(!TextureUploadSource::Capture(1,128u*1024*1024+1,0,0,reject));
 CHECK(!TextureUploadSource::Capture(0xFFFFFFFE,4,0,0,reject)&&!reads);
 auto snapshot=TextureUploadSource::Capture(0x1000,16,0x1000,16,[&](u32,std::span<u8>bytes){std::fill(bytes.begin(),bytes.end(),++reads==1?0x11:0x22);return true;});
 CHECK(snapshot&&snapshot->Slice(false,0x1000,16)[0]==0x11&&snapshot->Slice(true,0x1000,16)[0]==0x22);
 CHECK(snapshot->Slice(false,0x100F,2).empty()&&snapshot->Slice(true,0x0FFF,1).empty()&&snapshot->Slice(true,0x1000,0).empty());
 reads=0;CHECK(!TextureUploadSource::Capture(0x1000,16,0x2000,16,[&](u32,std::span<u8>){return ++reads==1;}));CHECK(reads==2);
}
int main(){
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
 char loaded[MAX_PATH]{};CHECK(GetModuleFileNameA(GetModuleHandleA("rexruntime.dll"),loaded,MAX_PATH));CHECK(!_stricmp(loaded,std::getenv("LEGO_TEST_RUNTIME_EXPECTED")));
 rex::memory::Memory memory;CHECK(memory.Initialize());memory_pointer=&memory;
 CHECK(memory.LookupHeap(0xC0000000)->Protect(0xC0020000,0x100000,rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite));
 CHECK(memory.LookupHeap(0xC0000000)->AllocFixed(0xD0000000,0x1000000,0x1000000,
     rex::memory::kMemoryAllocationReserve|rex::memory::kMemoryAllocationCommit,
     rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite));
 CheckBoundsAndBank();unsigned tested=0;
 using TF=rex::graphics::xenos::TextureFormat;using RF=plume::RenderFormat;using DD=rex::graphics::xenos::DataDimension;
 const std::array formats{std::pair(TF::k_8_8_8_8,RF::R8G8B8A8_UNORM),std::pair(TF::k_16_FLOAT,RF::R16_FLOAT),
  std::pair(TF::k_32_32_32_32_FLOAT,RF::R32G32B32A32_FLOAT),std::pair(TF::k_DXT1,RF::BC1_UNORM),
  std::pair(TF::k_DXT4_5,RF::BC3_UNORM),std::pair(TF::k_DXT3A,RF::R8_UNORM)};
 for(auto [guest_format,host_format]:formats)for(auto dimension:{DD::k2DOrStacked,DD::kCube,DD::k3D})
 for(bool tiled:{false,true})for(bool packed:{false,true})for(u32 endian=0;endian<4;++endian)
 for(u32 address:{0x20000u,0xA0020000u,0xC0020000u,0xE001F000u,0xF001F000u}){
  auto resource=std::make_shared<TextureResource>();resource->format=host_format;resource->d3d_type=dimension==DD::kCube?18:dimension==DD::k3D?17:3;resource->depth=dimension==DD::k3D?4:1;
  Fetch fetch{};fetch.dimension=dimension;fetch.format=guest_format;fetch.tiled=tiled;fetch.packed_mips=packed;fetch.pitch=2;fetch.endianness=decltype(fetch.endianness)(endian);
  fetch.base_address=address>>12;fetch.mip_address=(address+0x60000)>>12;
  const auto layout=rex::graphics::texture_util::GetGuestTextureLayout(dimension,fetch.pitch,resource->width,resource->height,resource->depth,tiled,guest_format,packed,true,2);
  const auto base_size=TextureEndianExtent(layout.base.level_data_extent_bytes,TextureEndianXor(endian));
  const auto mip_size=TextureEndianExtent(layout.mips_total_extent_bytes,TextureEndianXor(endian));CHECK(base_size&&mip_size);
  Fill(address,base_size,0x12);Fill(address+0x60000,mip_size,0x34);
  const auto retained=TextureUploadSource::Capture(address,base_size,address+0x60000,mip_size,CopyTextureUploadSpan);
  if(!retained)std::fprintf(stderr,"capture address=%08X format=%u dim=%u tiled=%u packed=%u endian=%u base=%u mip=%u readable=%d/%d\n",address,u32(guest_format),u32(dimension),tiled,packed,endian,base_size,mip_size,ReadableUploadSpan(address,base_size),ReadableUploadSpan(address+0x60000,mip_size));
  CHECK(retained);
  const auto before=MakeRelocatableTextureUploadSourceKey(retained->Base(),retained->Mips(),{reinterpret_cast<const u8*>(&fetch),sizeof(fetch)});
  std::vector<u8>expected,actual;const bool decoded=Decode(resource,fetch,retained,expected);
  if(!decoded){std::fprintf(stderr,"decode format=%u dim=%u tiled=%u packed=%u endian=%u address=%08X base=%u mip=%u packed_level=%u\n",u32(guest_format),u32(dimension),tiled,packed,endian,address,base_size,mip_size,layout.packed_level);
   for(u32 level=0;level<3;++level){const auto at=std::min(level,layout.packed_level);const auto&l=level?layout.mips[at]:layout.base;std::fprintf(stderr,"level=%u off=%u extent=%u stride=%u\n",level,level?layout.mip_offsets_bytes[at]:0,l.array_slice_data_extent_bytes,l.array_slice_stride_bytes);}}
  CHECK(decoded);
  // Reproduce a CPU writer between source hashing and untile/decode.
  Fill(address,base_size,0x92);Fill(address+0x60000,mip_size,0xB4);
  CHECK(Decode(resource,fetch,retained,actual)&&actual==expected);
  CHECK(MakeRelocatableTextureUploadSourceKey(retained->Base(),retained->Mips(),{reinterpret_cast<const u8*>(&fetch),sizeof(fetch)})==before);
  auto fresh=TextureUploadSource::Capture(address,base_size,address+0x60000,mip_size,CopyTextureUploadSpan);CHECK(fresh);
  CHECK(!(MakeRelocatableTextureUploadSourceKey(fresh->Base(),fresh->Mips(),{reinterpret_cast<const u8*>(&fetch),sizeof(fetch)})==before));
  CHECK(Decode(resource,fetch,fresh,actual)&&actual!=expected);++tested;
 }
 CHECK(memory.LookupHeap(0xC0000000)->Protect(0xC0020000,4096,rex::memory::kMemoryProtectRead));
 CHECK(TextureUploadSource::Capture(0xA0020000,64,0,0,CopyTextureUploadSpan));
 CHECK(memory.LookupHeap(0xC0000000)->Protect(0xC0020000,4096,rex::memory::kMemoryProtectNoAccess));
 CHECK(!TextureUploadSource::Capture(0x20000,64,0,0,CopyTextureUploadSpan));
 std::printf("PASS %u actual native decode cases: six formats, 2D/cube/volume, linear/tiled, packed mips, endian modes, raw/A/C/E/F storage; retained hash/decode survives later CPU edits; bank/bounds/failure cases. CPU SDK only; no game/GPU.\n",tested);
}
