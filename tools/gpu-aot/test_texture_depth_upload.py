"""Execute actual native UNORM24 upload/resolve/binding bodies with bounded mocks."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--compiler', default='clang++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source_path = root / 'rexlego/src/gpu_native/textures.cpp'
    text = source_path.read_text()
    sdk_path = root / 'rexglue-sdk/src/graphics/shaders/bytecode/d3d12_5_1/texture_load_depth_unorm_cs.h'
    sdk = sdk_path.read_text().split('#endif', 1)[0]
    # Pinned SDK's actual emitted DXBC disassembly: endian conversion precedes
    # stencil removal, endpoint correction, unsigned conversion and scaling.
    operations = ['ushr r2.xyzw, r2.xyzw, l(8, 8, 8, 8)',
                  'ushr r3.xyzw, r2.xyzw, l(23, 23, 23, 23)',
                  'iadd r2.xyzw, r2.xyzw, r3.xyzw',
                  'utof r2.xyzw, r2.xyzw', 'mul r2.xyzw, r2.xyzw']
    cursor = 0
    for op in operations:
        cursor = sdk.index(op, cursor) + len(op)
    bodies = '\n'.join(function(text, sig) for sig in [
        'bool EnsureDepthSamplingMirror(', 'bool UploadGuestDepthTexture(',
        'TextureResourceView ResolveTextureResource('])
    upload = function(text, 'bool UploadTextureResource(')
    upload = upload[:upload.index('  if (DepthAliasesEnabled()')] + '  return false;\n}'
    branch_begin = text.index('  // Xenos k_24_8 sampling')
    branch_end = text.index('  // Preserve both host depth', branch_begin)
    resolve_branch = text[branch_begin:branch_end]
    draw_text = (root / 'rexlego/src/gpu_native/draw.cpp').read_text()
    bind_begin = draw_text.index('    auto texture = ResolveTextureResource(bindings.textures[i]);')
    bind_end = draw_text.index('    TextureFetchWords fetch;', bind_begin)
    bind = draw_text[bind_begin:bind_end]
    tiled = function((root / 'rexglue-sdk/src/graphics/pipeline/texture/util.cpp').read_text(),
                     'int32_t GetTiledOffset2D(')
    harness = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>
#include "texture_depth_upload.h"
#include "texture_swizzle.h"
using namespace legodimensions::gpu_native;
using u8=uint8_t; using u32=uint32_t; using u64=uint64_t;
namespace rex {
constexpr u32 align(u32 v,u32 a) { return (v+a-1)&~(a-1); }
namespace graphics {
namespace xenos { enum class DataDimension { k2DOrStacked=1, k3D=2, kCube=3 }; constexpr u32 kTextureTileWidthHeight=32;
struct Fetch {
 u32 base_address=1, mip_address=0, pitch=3, tiled=1, format=22, packed_mips=0, swizzle=0xB48;
 u32 stacked=0, num_format=0, sign_x=0, sign_y=0, sign_z=0, sign_w=0, endianness=2;
 int exp_adjust=0; DataDimension dimension=DataDimension::k2DOrStacked;
}; }
namespace texture_util {
struct Layout { struct { u32 level_data_extent_bytes=0, array_slice_data_extent_bytes=0, row_pitch_bytes=0; } base; };
inline bool oversized_slice=false;
Layout GetGuestTextureLayout(xenos::DataDimension,u32 pitch,u32 width,u32 height,u32,bool tiled,u32,bool,bool,u32) {
 Layout l; l.base.row_pitch_bytes=(tiled ? rex::align(pitch<<5,32) : width)*4;
 l.base.level_data_extent_bytes=l.base.row_pitch_bytes*(tiled ? rex::align(height,32) : height);
 l.base.array_slice_data_extent_bytes=l.base.level_data_extent_bytes+(oversized_slice ? 4 : 0); return l;
}
void GetPackedMipOffset(u32,u32,u32,u32,u32,u32& x,u32& y,u32& z) { x=y=z=0; }
'''
    harness += tiled + r'''
} } }
namespace plume {
enum class RenderFormat { UNKNOWN, R32_FLOAT, R32G32B32A32_FLOAT, D32_FLOAT_S8 };
enum class RenderTextureDimension { TEXTURE_2D };
enum class RenderTextureViewDimension { TEXTURE_2D };
enum class RenderSampleCount { COUNT_1 };
enum RenderTextureFlag { RENDER_TARGET=1 };
enum class RenderSwizzle { R,G,B,A,ZERO,ONE };
struct RenderComponentMapping { std::array<RenderSwizzle,4> lanes{};
 RenderComponentMapping()=default;
 RenderComponentMapping(RenderSwizzle r,RenderSwizzle g,RenderSwizzle b,RenderSwizzle a):lanes{r,g,b,a}{} };
struct RenderTextureDesc { RenderTextureDimension dimension{};u32 width{},height{},depth{},mipLevels{},arraySize{},flags{};bool committed{};
 struct { RenderSampleCount sampleCount{}; } multisampling; RenderFormat format{}; };
struct RenderTextureViewDesc { RenderTextureViewDimension dimension{}; RenderFormat format{};u32 mipLevels{};RenderComponentMapping componentMapping; };
struct RenderTextureView { RenderTextureViewDesc desc; };
inline bool fail_view=false;
struct RenderTexture { std::vector<u8> contents;u32 width{},height{}; virtual ~RenderTexture()=default;
 std::unique_ptr<RenderTextureView> createTextureView(const RenderTextureViewDesc& desc) {
   return fail_view ? nullptr : std::make_unique<RenderTextureView>(RenderTextureView{desc}); } };
struct D3D12Texture:RenderTexture { void* d3d=this; };
inline bool fail_map=false;
struct RenderBuffer { static inline u32 destroyed=0;std::vector<u8> contents; explicit RenderBuffer(u32 size):contents(size){};
 ~RenderBuffer(){ ++destroyed; } u8* map(){return fail_map ? nullptr : contents.data();} void unmap(){} };
struct RenderBufferDesc {u32 size;static RenderBufferDesc UploadBuffer(u32 size){return{size};}};
enum class RenderBarrierStage { COPY,GRAPHICS };
enum class RenderTextureLayout { COPY_DEST,SHADER_READ };
struct RenderTextureBarrier { RenderTexture* texture; RenderTextureLayout layout; };
struct RenderTextureCopyLocation {RenderTexture* texture=nullptr;RenderBuffer* buffer=nullptr;u32 width=0,height=0,pitch=0;
 static RenderTextureCopyLocation Subresource(RenderTexture* t,u32,u32){return{t};}
 static RenderTextureCopyLocation PlacedFootprint(RenderBuffer* b,RenderFormat format,u32 w,u32 h,u32 d,u32 p,u32 offset){
 assert(format==RenderFormat::R32_FLOAT && d==1 && offset==0);return{nullptr,b,w,h,p};} };
struct RenderCommandList {u32 copies=0;std::vector<RenderTextureLayout> barriers_seen;
 void barriers(RenderBarrierStage,RenderTextureBarrier b){barriers_seen.push_back(b.layout);}
 void copyTextureRegion(RenderTextureCopyLocation dst,RenderTextureCopyLocation src){
 assert(dst.texture && src.buffer);++copies;dst.texture->contents=src.buffer->contents;
 assert(src.width==dst.texture->width && src.height==dst.texture->height);
 assert(src.pitch%64==0 && src.pitch>=src.width); } };
}
enum class D3DResourceType { kTexture=3,kVolumeTexture=4,kCubeTexture=5 };
struct CpuMemoryStamp {u64 generation=0;}; struct CpuMemorySpan {u32 address,size;};
struct TextureUploadSourceKey {u64 hash=0;bool operator==(const TextureUploadSourceKey&) const=default;};
u64 XXH3_64bits(const void* ptr,size_t size) {auto* p=static_cast<const u8*>(ptr);u64 hash=14695981039346656037ull;
 while(size--)hash=(hash^*p++)*1099511628211ull;return hash;}
TextureUploadSourceKey MakeTextureUploadSourceKey(std::span<const u8> base,std::span<const u8>,std::span<const u8> metadata){
 return{XXH3_64bits(base.data(),base.size())^XXH3_64bits(metadata.data(),metadata.size())};}
struct TextureResource {
 u32 guest_address=0x1234,width=67,height=35,levels=1,d3d_type=3,guest_format=22;
 bool surface=false,owns_guest_memory=false,resolved_on_host=false,guest_uploaded=false,sampled_valid=false;
 std::mutex mutex;plume::RenderFormat format=plume::RenderFormat::D32_FLOAT_S8;
 rex::graphics::xenos::Fetch guest_fetch;
 std::unique_ptr<plume::RenderTexture> texture=std::make_unique<plume::D3D12Texture>();
 std::unique_ptr<plume::RenderTexture> sampled_texture;
 std::unique_ptr<plume::RenderTextureView> view=std::make_unique<plume::RenderTextureView>(),sampled_view,resolved_view;
 u32 descriptor_index=4,sampled_descriptor_index=~0u,resolved_descriptor_index=~0u;
 u64 guest_content_hash=0,depth_resolve_generation=0;
 u64 last_authority_probe_signature=0;bool authority_probe_signature_valid=false;
 TextureUploadSourceKey upload_source_key;bool upload_source_key_valid=false;CpuMemoryStamp cpu_stamp;
 u32 HostWidth()const{return width;}u32 HostHeight()const{return height;}
};
struct TextureResourceView {plume::RenderTexture* texture;plume::RenderTextureView* view;plume::RenderFormat format;
 u32 descriptor_index,width,height,d3d_type;bool surface,depth;};
struct {u64 source_hits=0,hashed_bytes=0,converted_bytes=0;double hash_ms=0,source_ms=0;} g_upload_timing;
bool NativeTextureTimingEnabled(){return true;}
struct UploadTimer{~UploadTimer(){}};
bool IsDepthFormat(plume::RenderFormat f){return f==plume::RenderFormat::D32_FLOAT_S8;}
bool LongProbeEnabled(){return false;} bool LongProbeOnce(u64){return false;}
template<class... T>void LongProbeEvent(T&&...){}
u32 RowPitch(plume::RenderFormat f,u32 width){assert(f==plume::RenderFormat::R32_FLOAT);return rex::align(width*4,256);}
std::vector<u8> guest_bytes;
bool readable=true;u32 translations=0;u64 generation=1;
struct Memory {
 template<class T>T TranslatePhysical(u32 address){assert(readable && address==0x1000);++translations;return reinterpret_cast<T>(guest_bytes.data());}
 template<class T>T TranslateVirtual(u32 address){assert(readable && address==0xE0001000);++translations;return reinterpret_cast<T>(guest_bytes.data());}
} memory;
#define REX_KERNEL_MEMORY() (&memory)
#define REXLOG_INFO(...) ((void)0)
bool ReadableUploadSpan(u32 address,u32 size){return readable && (address==0x1000||address==0xE0001000) && size<=guest_bytes.size();}
bool CpuMemoryWatchEnabled(){return true;}
CpuMemoryStamp WatchCpuMemory(std::span<const CpuMemorySpan> spans){assert(spans.size()==1);return{generation};}
bool CpuMemoryUnchanged(const CpuMemoryStamp& stamp){return stamp.generation==generation;}
struct Device {
 bool fail_texture=false,fail_native=false,fail_buffer=false;
 std::unique_ptr<plume::RenderTexture> createTexture(const plume::RenderTextureDesc& desc){
  if(fail_texture)return nullptr;auto texture=std::make_unique<plume::D3D12Texture>();
  texture->width=desc.width;texture->height=desc.height;if(fail_native)texture->d3d=nullptr;return texture;}
 std::unique_ptr<plume::RenderBuffer> createBuffer(plume::RenderBufferDesc desc){
  return fail_buffer ? nullptr : std::make_unique<plume::RenderBuffer>(desc.size);}
} device;
struct Region {u32 left=0,top=0,width=67,height=35,x=0,y=0;};
enum class ColorResolveDestination {kDepthFloat32};
struct HostDevice {
 static inline std::recursive_mutex mutex;
 static inline u32 next_descriptor=100,registrations=0;
 static inline bool fail_descriptor=false,fail_resolve=false;
 static inline std::vector<std::shared_ptr<plume::RenderBuffer>> retired;
 static auto LockRecording(){return std::unique_lock(mutex);}
 static auto Device(){return &device;}
 static u32 RegisterTexture(plume::RenderTexture*,plume::RenderTextureView*){
  if(fail_descriptor)return ~0u;++registrations;return next_descriptor++;}
 static void RetireResource(std::shared_ptr<plume::RenderBuffer> buffer){retired.push_back(std::move(buffer));}
 static bool ResolveHdrColor(plume::RenderTexture*,u32,plume::RenderTexture* dst,u32 w,u32 h,float,
                            ColorResolveDestination,bool,const Region*){
  if(fail_resolve)return false;dst->contents.assign(RowPitch(plume::RenderFormat::R32_FLOAT,w)*h,0x5A);return true;}
 static void SnapshotTexture(plume::RenderTexture*,const std::string&,bool){}
};
std::unordered_map<u32,std::shared_ptr<TextureResource>> textures;
auto FindTexture(u32 address){auto it=textures.find(address);return it==textures.end()?nullptr:it->second;}
auto AdoptTexture(u32 address){return FindTexture(address);}
bool DepthAliasesEnabled(){return true;}
struct Alias {u32 physical_base=0x1000;}; Alias AliasLayout(const TextureResource&){return{};}
u64 g_depth_alias_generation=0;
std::unordered_map<u32,std::weak_ptr<TextureResource>> g_depth_resolves;
'''
    harness += bodies + '\n' + upload
    harness += r'''
bool ResolveDepth(const std::shared_ptr<TextureResource>& adopted_destination) {
 auto source=std::make_shared<TextureResource>();source->surface=true;
 std::unique_ptr<Region> region=std::make_unique<Region>();
 u32 destination_level=0,destination_slice=0,resolve_flags=4,destination_texture=adopted_destination->guest_address;
'''
    harness += resolve_branch + '  return false;\n}\n'
    harness += r'''
u32 BoundShaderTextureMask(int){return 0;}
enum class ShaderStage {kVertex};u32 BoundShaderTextureMask(ShaderStage){return 0;}
bool BindTexture(u32 address,plume::RenderCommandList* commands,u32& selected) {
 struct{std::array<u32,32> textures{};}bindings;bindings.textures[15]=address;
 for(u32 i=15;i<16;++i){
'''
    harness += bind + r'''
  selected=texture.descriptor_index;
 } return true;
}
void Fill(TextureResource& resource,u32 endian,bool tiled) {
 resource.guest_fetch.endianness=endian;resource.guest_fetch.tiled=tiled;
 auto layout=rex::graphics::texture_util::GetGuestTextureLayout(resource.guest_fetch.dimension,
   resource.guest_fetch.pitch,resource.width,resource.height,1,tiled,22,false,true,0);
 guest_bytes.assign(layout.base.level_data_extent_bytes,0xCD);
 const u32 xors[]={0,1,3,2};
 for(u32 y=0;y<resource.height;++y)for(u32 x=0;x<resource.width;++x){
  u32 depth=(x==0 && y==0)?0:(x==resource.width-1 && y==resource.height-1)?0xFFFFFF:((x*72577+y*39523)&0xFFFFFF);
  const u32 raw=(depth<<8)|((x*19+y*7)&255);
  const u32 offset=tiled?rex::graphics::texture_util::GetTiledOffset2D(x,y,layout.base.row_pitch_bytes/4,2):y*layout.base.row_pitch_bytes+x*4;
  const auto* bytes=reinterpret_cast<const u8*>(&raw);
  for(u32 b=0;b<4;++b)guest_bytes[(offset+b)^xors[endian]]=bytes[b];
 }
 ++generation;
}
void CheckPixels(const TextureResource& resource) {
 const auto& bytes=resource.sampled_texture->contents;const u32 pitch=RowPitch(plume::RenderFormat::R32_FLOAT,resource.width);
 for(u32 y=0;y<resource.height;++y)for(u32 x=0;x<resource.width;++x){
  const u32 depth=(x==0&&y==0)?0:(x==resource.width-1&&y==resource.height-1)?0xFFFFFF:((x*72577+y*39523)&0xFFFFFF);
  float value;std::memcpy(&value,bytes.data()+y*pitch+x*4,4);
  const float sdk_value=float(depth+(depth>>23))*std::ldexp(1.0f,-24);
  assert(value==sdk_value);assert(std::abs(double(value)-double(depth)/16777215.0)<0.00000006);
 }
 for(u32 y=0;y<resource.height;++y)for(u32 b=resource.width*4;b<pitch;++b)assert(bytes[y*pitch+b]==0);
}
int main(){
 // Actual non-square tiled rectangles, endian modes and stencil-independent normalized endpoints.
 for(bool tiled:{false,true})for(u32 endian=0;endian<4;++endian){
  auto resource=std::make_shared<TextureResource>();textures[resource->guest_address]=resource;Fill(*resource,endian,tiled);
  plume::RenderCommandList commands;u32 selected=~0u;
  assert(BindTexture(resource->guest_address,&commands,selected));assert(commands.copies==1);
  assert(selected==resource->sampled_descriptor_index && selected!=resource->descriptor_index);
  assert(!resource->resolved_on_host && resource->sampled_valid && resource->guest_uploaded);
  assert((resource->sampled_view->desc.componentMapping.lanes==std::array{plume::RenderSwizzle::R,plume::RenderSwizzle::R,plume::RenderSwizzle::ONE,plume::RenderSwizzle::ONE}));
  CheckPixels(*resource);
  assert((commands.barriers_seen==std::vector{plume::RenderTextureLayout::COPY_DEST,plume::RenderTextureLayout::SHADER_READ}));
  const auto copies=commands.copies;const auto before=translations;
  assert(UploadTextureResource(resource->guest_address,&commands,false));assert(commands.copies==copies && translations==before);
  // Required verification catches direct guest writes even when no watch generation changed.
  guest_bytes[0]^=1;assert(UploadTextureResource(resource->guest_address,&commands,true));assert(commands.copies==copies+1);
  const auto destroyed=plume::RenderBuffer::destroyed;assert(!HostDevice::retired.empty());
  HostDevice::retired.clear();assert(plume::RenderBuffer::destroyed>destroyed);
  // Actual host depth-atlas resolve branch takes authority; stale CPU memory cannot replace it.
  assert(ResolveDepth(resource));assert(resource->resolved_on_host && resource->sampled_valid);
  const auto resolved_bytes=resource->sampled_texture->contents;readable=false;++generation;
  assert(UploadTextureResource(resource->guest_address,&commands,true));
  assert(resource->sampled_texture->contents==resolved_bytes);readable=true;
 }
 // A watched generation change must also refresh the real first-use mirror.
 {
  auto resource=std::make_shared<TextureResource>();textures[resource->guest_address]=resource;Fill(*resource,2,true);
  plume::RenderCommandList commands;assert(UploadTextureResource(resource->guest_address,&commands,false));
  guest_bytes[0]^=0x80;++generation;
  assert(UploadTextureResource(resource->guest_address,&commands,false));assert(commands.copies==2);
  float value;std::memcpy(&value,resource->sampled_texture->contents.data(),4);
  assert(value==UnpackDepth24(0x800000));
 }
 // Exact atlas dimensions exercise the full non-square SDK tiled address range.
 {
  auto resource=std::make_shared<TextureResource>();resource->width=960;resource->height=3840;resource->guest_fetch.pitch=30;
  textures[resource->guest_address]=resource;Fill(*resource,2,true);
  plume::RenderCommandList commands;assert(UploadTextureResource(resource->guest_address,&commands,false));CheckPixels(*resource);
 }
 // Every computed byte address must remain within the resident source span.
 {
  std::array<u8,4> src{},dst{};
  assert(!CopyTextureDepth24(src,dst,1,1,4,0,[](u32,u32,u32)->int64_t{return -1;}));
  assert(!CopyTextureDepth24(src,dst,1,1,4,3,[](u32,u32,u32)->int64_t{return 2;}));
  assert(!CopyTextureDepth24(src,dst,1,1,4,0,[](u32,u32,u32)->int64_t{return INT64_MAX;}));
  assert(!CopyTextureDepth24(src,dst,2,1,4,0,[](u32,u32,u32)->int64_t{return 0;}));
 }
 // Invalid source/extent never reaches translation; FLOAT24 and unsupported shapes remain rejected.
 for(int failure=0;failure<13;++failure){
  auto resource=std::make_shared<TextureResource>();textures[resource->guest_address]=resource;Fill(*resource,2,true);
  if(failure==0)readable=false;
  if(failure==1)rex::graphics::texture_util::oversized_slice=true;
  if(failure==2)resource->guest_format=23;
  if(failure==3)resource->levels=2;
  if(failure==4)resource->guest_fetch.stacked=1;
  if(failure==5)resource->guest_fetch.dimension=rex::graphics::xenos::DataDimension::k3D;
  if(failure==6)device.fail_texture=true;
  if(failure==7)device.fail_native=true;
  if(failure==8)plume::fail_view=true;
  if(failure==9)HostDevice::fail_descriptor=true;
  if(failure==10)device.fail_buffer=true;
  if(failure==11)plume::fail_map=true;
  const auto before=translations;plume::RenderCommandList commands;u32 selected=~0u;
  assert(!BindTexture(resource->guest_address,failure==12?nullptr:&commands,selected));
  assert(!resource->sampled_valid && !resource->guest_uploaded && commands.copies==0 && selected==~0u);
  if(failure<2||failure==12)assert(translations==before);
  if(failure>=6 && failure<=9)assert(!resource->sampled_texture && resource->sampled_descriptor_index==~0u);
  readable=true;rex::graphics::texture_util::oversized_slice=false;
  device.fail_texture=device.fail_native=device.fail_buffer=false;plume::fail_view=plume::fail_map=HostDevice::fail_descriptor=false;
  if(failure>=6 && failure<=11){
   const auto descriptor=resource->sampled_descriptor_index;const auto registrations=HostDevice::registrations;
   assert(BindTexture(resource->guest_address,&commands,selected));assert(resource->sampled_valid && resource->guest_uploaded);
   if(failure>=10){assert(selected==descriptor && HostDevice::registrations==registrations);}
  }
 }
 // Simulate the existing relocation/pool-copy invalidation boundary flags; next upload refreshes this mirror.
 auto resource=std::make_shared<TextureResource>();textures[resource->guest_address]=resource;Fill(*resource,2,true);
 plume::RenderCommandList commands;assert(UploadTextureResource(resource->guest_address,&commands,false));
 resource->guest_uploaded=false;resource->upload_source_key_valid=false;resource->cpu_stamp={};Fill(*resource,2,true);
 resource->guest_fetch.base_address=0xE0001;assert(UploadTextureResource(resource->guest_address,&commands,false));assert(commands.copies==2);
 auto view=ResolveTextureResource(resource->guest_address);assert(view.format==plume::RenderFormat::R32_FLOAT && !view.depth);
 HostDevice::retired.clear();
 std::cout<<"Production UNORM24 depth upload, descriptor refresh and host-authority regressions passed\n";
}
'''
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.output.resolve() / 'test.cpp'
    binary = args.output.resolve() / 'test.exe'
    fixture.write_text(harness)
    subprocess.run([args.compiler, '-std=c++20', '-UNDEBUG', '-Wall', '-Wextra',
                    '-I', str(root / 'rexlego/src/gpu_native'), str(fixture), '-o', str(binary)],
                   check=True, timeout=45)
    subprocess.run([str(binary)], check=True, timeout=10)
    (args.output / 'verification.json').write_text(json.dumps({
        'production_source_sha256': hashlib.sha256(source_path.read_bytes()).hexdigest(),
        'sdk_depth_loader_sha256': hashlib.sha256(sdk_path.read_bytes()).hexdigest(),
        'decoder_sha256': hashlib.sha256((root / 'rexlego/src/gpu_native/texture_depth_upload.h').read_bytes()).hexdigest(),
        'actual_bodies': ['UploadGuestDepthTexture', 'EnsureDepthSamplingMirror',
                         'ResolveTextureResource', 'UploadTextureResource entry guards',
                         'ResolveTextureFromSurface depth atlas branch', 'BindConstants upload/refresh'],
        'result': 'passed'}, indent=2)+'\n')


if __name__ == '__main__':
    main()
