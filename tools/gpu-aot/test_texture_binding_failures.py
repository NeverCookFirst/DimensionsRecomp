"""Exercise production texture binding and draw failure propagation without a GPU."""

import argparse
from pathlib import Path
import subprocess


def block(text, start):
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--verify-regression", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    draw = (root / "rexlego/src/gpu_native/draw.cpp").read_text()
    textures = (root / "rexlego/src/gpu_native/textures.cpp").read_text()
    loop = block(draw, draw.index("  for (u32 i = 0; i < kNativeTextureSlots; ++i)",
                                 draw.index("bool BindConstants(")))
    upload_start = textures.index("{", textures.index("bool UploadTextureResourceStorage(")) + 1
    upload_entry = textures[upload_start:textures.index("  if (resource) {", upload_start)]
    hooks = "\n".join(block(draw, draw.index(signature)) for signature in
                       ("u32 DrawVerticesHook(", "u32 DrawIndexedVerticesHook("))
    harness = r'''
#include <array>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
// TEMPORAL_TEXTURE_HEADER
#ifdef _WIN32
#include <stdlib.h>
#endif
using u32=uint32_t; using u64=uint64_t; using i32=int32_t; using u8=uint8_t;
constexpr u32 kNativeTextureSlots=3;
enum class ShaderStage {kVertex,kPixel};
enum class D3DResourceType:u32 {kTexture=3,kVolumeTexture=4,kCubeTexture=5};
using TextureFetchWords=std::array<u32,6>;
void ApplyFsrTextureBias(TextureFetchWords&,float){}
// MERGE_TEXTURE_FETCH
#define REXLOG_INFO(...) ((void)0)
void ReportTextureBindingProblem(u32,u32,u32){}
bool LongProbeEnabled(){return false;}
bool LongProbeOnce(u64){return false;}
template<class... Args>void LongProbeEvent(Args&&...){}
template<class T>int LongProbeHex(const T&){return 0;}
u64 BoundShaderHash(ShaderStage){return 0;}
u32 vertex_mask=0;
u32 BoundShaderTextureMask(ShaderStage stage){return stage==ShaderStage::kVertex?vertex_mask:0;}
namespace plume {struct RenderCommandList{};}
struct MockRecording {void lock(){}void unlock(){}};
struct HostDevice {
 static MockRecording LockRecording(){return {};}
 static u32 RegisterSampler(const TextureFetchWords& f){last_fetch=f;++sampler_calls;return 7;}
 static void RegisterSamplers(const std::array<TextureFetchWords,32>& fetches,u32 mask,
      std::array<u32,32>& out){for(u32 i=0;i<32;++i)if(mask&(1u<<i))out[i]=RegisterSampler(fetches[i]);}
 static inline TextureFetchWords last_fetch{};
 static inline u32 sampler_calls=0;
};
struct D3DDevice {struct {std::array<u32,6>dword{};}fetch_constants[kNativeTextureSlots];u8 padding[16384]{};};
struct PassProfileScope {PassProfileScope(const char*, const D3DDevice*){}};
struct DrawBindings {std::array<u32,kNativeTextureSlots>textures{};};
struct Shared {
 std::array<u32,kNativeTextureSlots>samplers{};
 std::array<u32,kNativeTextureSlots>texture_2d{};
 std::array<u32,kNativeTextureSlots>texture_3d{1,1,1};
 std::array<u32,kNativeTextureSlots>texture_cube{2,2,2};
};
struct TextureResource {
 std::mutex mutex;
 void* texture=reinterpret_cast<void*>(1);
 bool surface=false,resolved_on_host=false,cpu_upload_allowed=true;
 u32 guest_format=0;int format=0;
 u32 descriptor_index=3,d3d_type=u32(D3DResourceType::kTexture);
 u32 uploaded_descriptor=0;
 bool lose_storage_on_upload=false;
 TextureFetchWords sampling_fetch{};bool has_sampling_fetch=false;
};
// Authority decisions have a real SDK/GPU fixture; this allocation/failure boundary does not publish resolves.
void RefreshPublishedTextureAuthority(TextureResource&){}
struct TextureResourceView {void*texture=nullptr;u32 descriptor_index=~u32{0},d3d_type=3;bool surface=false,depth=false;
 TextureFetchWords sampling_fetch{};bool has_sampling_fetch=false;u64 content_hash=0;bool content_hash_valid=false;};
std::unordered_map<u32,std::shared_ptr<TextureResource>>resources;
std::shared_ptr<TextureResource>FindTexture(u32 address){
 auto it=resources.find(address);return it==resources.end()?nullptr:it->second;
}
TextureResourceView ResolveTextureResource(u32 address){
 auto resource=FindTexture(address);if(!resource)return {};
 return {resource->texture,resource->descriptor_index,resource->d3d_type,false,false,
         resource->sampling_fetch,resource->has_sampling_fetch};
}
plume::RenderCommandList* NativeResourceCommands(plume::RenderCommandList* p){return p;}
struct UploadTimer{};
bool IsDepthFormat(int format){return format==22;}
bool UploadGuestDepthTexture(TextureResource&,plume::RenderCommandList*,bool){
 assert(false&&"Depth conversion is exercised by test_texture_depth_upload.py");return false;
}
u32 upload_calls=0,cpu_upload_calls=0;
std::vector<bool>require_hashes;
bool UploadTextureResource(u32 guest_address,plume::RenderCommandList* commands,bool require_content_hash){
 ++upload_calls;require_hashes.push_back(require_content_hash);
 const auto resource=FindTexture(guest_address);
'''
    harness += upload_entry + r'''
 // Only allocation/conversion machinery below the production entry guard is
 // mocked. A residency or conversion failure is represented by false here.
 ++cpu_upload_calls;
 if(resource&&resource->cpu_upload_allowed){
  if(resource->uploaded_descriptor)resource->descriptor_index=resource->uploaded_descriptor;
  if(resource->lose_storage_on_upload)resource->texture=nullptr;
 }
 return resource&&commands&&resource->texture&&resource->cpu_upload_allowed;
}
enum class TexturePreparationResult{kUnavailable,kUploadFailed,kReady};
TexturePreparationResult PrepareTextureResource(u32 address,plume::RenderCommandList* commands,bool hash,TextureResourceView& texture){
 texture=ResolveTextureResource(address);
 if(!texture.texture || texture.descriptor_index==~u32{0})return TexturePreparationResult::kUnavailable;
 if(!UploadTextureResource(address,commands,hash))return TexturePreparationResult::kUploadFailed;
 texture=ResolveTextureResource(address);
 if(!texture.texture || texture.descriptor_index==~u32{0})return TexturePreparationResult::kUploadFailed;
 return TexturePreparationResult::kReady;
}
struct PreparedDrawFixture{std::array<TextureResourceView,32>textures;u32 missing_textures=0;};
PreparedDrawFixture*t_preparing_draw=nullptr;
TexturePreparationResult PrepareTextureBinding(u32,u32 address,const TextureFetchWords&,u32,
 plume::RenderCommandList* commands,bool hash,TextureResourceView& prepared){return PrepareTextureResource(address,commands,hash,prepared);}
bool SnapshotStateBindingsEnabled(){return false;}
int ReadDeviceBindings(const D3DDevice*){return 0;}void PublishDeviceBindings(int){}
bool BindTextureConstants(plume::RenderCommandList*commands,const D3DDevice*device,
                          const DrawBindings&bindings,u32 texture_mask,Shared&shared,
                          const TextureResourceView* composition_input=nullptr){
 constexpr bool trace_constants=false;
 const auto* bytes=reinterpret_cast<const u8*>(device);
 const u32 vertex_texture_mask=vertex_mask;
 legodimensions::gpu_native::TemporalTextureHistory* temporal_textures=nullptr;
 const u64 vertex_shader_hash=0,pixel_shader_hash=0;
 constexpr bool scene_sampling=false;constexpr float fsr_texture_bias=0;
 std::array<TextureFetchWords,32>sampler_fetches{};
 u32 sampler_mask=0;
'''
    batch_start=draw.index('  std::array<u32, 32> sampler_descriptors{};', draw.index('bool BindConstants('))
    batch_end=draw.index('  constant_timing.Next(ConstantCost::kShadow);', batch_start)
    harness += loop + draw[batch_start:batch_end].replace('std::copy(sampler_descriptors.begin(), sampler_descriptors.end(), shared.samplers);','std::copy_n(sampler_descriptors.begin(),kNativeTextureSlots,shared.samplers.begin());') + r'''
 return true;
}
plume::RenderCommandList command_list;
D3DDevice device;
DrawBindings bindings;
u32 texture_mask=7,query_failures=0,successful_draws=0;
Shared last_shared;
bool DispatchDraw(D3DDevice* value,u32,bool,u32,u32,i32){
 // BindConstants' caller rejects false before any draw submission.
 const bool bound=BindTextureConstants(&command_list,value,bindings,texture_mask,last_shared);
 if(bound)++successful_draws;return bound;
}
bool ConsumeCpuPoolCopyDraw(u32,u32,u32){return false;}
void FailActiveQueries(){++query_failures;}
'''
    harness += hooks + r'''
void Reset(){
 device={};HostDevice::last_fetch={};
 resources.clear();bindings={};last_shared={};vertex_mask=0;texture_mask=7;
 upload_calls=cpu_upload_calls=query_failures=successful_draws=HostDevice::sampler_calls=0;
 require_hashes.clear();
}
std::shared_ptr<TextureResource>Add(u32 address,u32 slot){
 auto value=std::make_shared<TextureResource>();value->descriptor_index=address+2;
 resources[address]=value;bindings.textures[slot]=address;return value;
}
int main(){
#ifdef _WIN32
 _set_error_mode(_OUT_TO_STDERR);
 _set_abort_behavior(0,_CALL_REPORTFAULT);
#endif
 {
  Reset();auto original=Add(1,0);original->cpu_upload_allowed=false;
  TextureResourceView reconstructed;reconstructed.texture=reinterpret_cast<void*>(77);
  reconstructed.descriptor_index=99;Shared result;
  assert(BindTextureConstants(&command_list,&device,bindings,1,result,&reconstructed));
  assert(result.texture_2d[0]==99 && upload_calls==0 && HostDevice::sampler_calls==1);
  auto other=Add(2,1);other->cpu_upload_allowed=false;
  assert(!BindTextureConstants(&command_list,&device,bindings,3,result,&reconstructed));
  assert(upload_calls==1); // Override applies only to the audited scene slot0.
 }
 for(bool indexed:{false,true}){
  Reset();auto value=Add(1,0);value->cpu_upload_allowed=false;
  if(indexed)DrawIndexedVerticesHook(&device,0,0,0,3);
  else DrawVerticesHook(&device,0,0,3);
  assert(query_failures==1&&successful_draws==0&&upload_calls==1&&cpu_upload_calls==1);
  assert(HostDevice::sampler_calls==0&&last_shared.texture_2d[0]==0);
 }
 {
  Reset();auto first=Add(1,0);auto second=Add(2,1);second->cpu_upload_allowed=false;
  DrawVerticesHook(&device,0,0,3);
  assert(query_failures==1&&successful_draws==0&&upload_calls==2);
  // Deferred sampler batch is committed only after all uploads succeed.
  assert(HostDevice::sampler_calls==0&&last_shared.texture_2d[1]==0);
 }
 {
  Reset();Add(1,0);auto volume=Add(2,1);volume->d3d_type=u32(D3DResourceType::kVolumeTexture);
  auto cube=Add(3,2);cube->d3d_type=u32(D3DResourceType::kCubeTexture);vertex_mask=2;
  DrawVerticesHook(&device,0,0,3);
  assert(successful_draws==1&&query_failures==0&&cpu_upload_calls==3);
  assert(last_shared.texture_2d[0]==3&&last_shared.texture_3d[1]==4&&last_shared.texture_cube[2]==5);
  assert((require_hashes==std::vector<bool>{false,true,false}));
 }
 for(bool resolved:{false,true}){
  Reset();auto value=Add(1,0);value->cpu_upload_allowed=false;
  value->surface=!resolved;value->resolved_on_host=resolved;
  DrawVerticesHook(&device,0,0,3);
  assert(successful_draws==1&&query_failures==0&&upload_calls==1&&cpu_upload_calls==0);
  // Host authority cannot bypass validation of command/storage availability.
  assert(!UploadTextureResource(1,nullptr,false));
  value->texture=nullptr;assert(!UploadTextureResource(1,&command_list,false));
 }
 {
  // Color alias promotion/demotion changes the SRV during upload. The old
  // descriptor stays alive for preceding draws, but cannot bind this draw.
  Reset();auto value=Add(1,0);value->uploaded_descriptor=29;
  DrawVerticesHook(&device,0,0,3);
  assert(successful_draws==1&&last_shared.texture_2d[0]==29);
 }
 {
  Reset();auto value=Add(1,0);value->uploaded_descriptor=~u32{0};
  DrawVerticesHook(&device,0,0,3);
  assert(query_failures==1&&successful_draws==0&&HostDevice::sampler_calls==0);
 }
 {
  Reset();bindings.textures[0]=99;Add(2,1)->descriptor_index=~u32{0};
  Add(3,2)->texture=nullptr;DrawVerticesHook(&device,0,0,3);
  assert(successful_draws==1&&query_failures==0&&upload_calls==0);
  assert((last_shared.texture_2d==std::array<u32,3>{0,0,0}));
  assert((last_shared.texture_3d==std::array<u32,3>{1,1,1}));
  assert((last_shared.texture_cube==std::array<u32,3>{2,2,2}));
 }
 {
  Reset();auto value=Add(1,0);value->has_sampling_fetch=true;
  value->sampling_fetch={0x88000002,0x10000052,0x007FE3FF,0x00000D10,0x00000E83,0x10800A00};
  device.fetch_constants[0].dword=value->sampling_fetch;
  device.fetch_constants[0].dword[4]=(8u<<2)|(9u<<6);
  reinterpret_cast<u8*>(&device)[12382]=13;
  DrawVerticesHook(&device,0,0,3);
  assert(successful_draws==1);
  assert(((HostDevice::last_fetch[4]>>2)&15)==0);
  assert(((HostDevice::last_fetch[4]>>6)&15)==10);
  // Changing an explicit app request still restricts the sampler.
  reinterpret_cast<u8*>(&device)[12356]=3;
  DrawVerticesHook(&device,0,0,3);
  assert(((HostDevice::last_fetch[4]>>2)&15)==3);
  assert(((device.fetch_constants[0].dword[4]>>2)&15)==8);
 }
 {
  Reset();Add(1,0)->cpu_upload_allowed=false;texture_mask=0;
  DrawVerticesHook(&device,0,0,3);
  assert(successful_draws==1&&query_failures==0&&upload_calls==0);
 }
 std::cout<<"Production texture binding rejects failed uploads; GPU authority and null defaults preserved\n";
}
'''
    sampler = (root / 'rexlego/src/gpu_native/sampler_state.h').read_text()
    harness = harness.replace('// MERGE_TEXTURE_FETCH', block(sampler, sampler.index('inline TextureFetchWords MergeTextureFetch(')))
    args.output.mkdir(parents=True, exist_ok=True)
    source = args.output.resolve() / "texture-binding.cpp"
    binary = args.output.resolve() / "texture-binding.exe"
    harness = harness.replace('// TEMPORAL_TEXTURE_HEADER', '#include "'+(root/'rexlego/src/gpu_native/temporal_texture_history.h').as_posix()+'"')
    source.write_text(harness)
    subprocess.run([args.compiler, "-std=c++20", "-UNDEBUG", '-I'+str(root/'rexlego/src'), str(source), "-o", str(binary)],
                   check=True, timeout=45)
    subprocess.run([str(binary)], check=True, timeout=10)
    if args.verify_regression:
        # Restore the old depth-only refresh in the extracted production body.
        # The color replacement case must fail, rather than merely compile.
        # The fixture prepares via the mocked upload/allocation boundary;
        # test_texture_prepare executes the full production transaction.
        old = " texture=ResolveTextureResource(address);\n if(!texture.texture || texture.descriptor_index==~u32{0})return TexturePreparationResult::kUploadFailed;"
        assert harness.count(old) == 1
        mutant = harness.replace(old,
            " if(texture.depth && !texture.surface) texture=ResolveTextureResource(address);\n if(!texture.texture || texture.descriptor_index==~u32{0})return TexturePreparationResult::kUploadFailed;", 1)
        mutation_source = args.output.resolve() / "texture-binding-old-refresh.cpp"
        mutation_binary = args.output.resolve() / "texture-binding-old-refresh.exe"
        mutation_source.write_text(mutant)
        subprocess.run([args.compiler, "-std=c++20", "-UNDEBUG", str(mutation_source),
                        "-o", str(mutation_binary)], check=True, timeout=45)
        result = subprocess.run([str(mutation_binary)], capture_output=True, timeout=10)
        assert result.returncode != 0, "Old descriptor selection escaped the regression"
        assert b"last_shared.texture_2d[0]==29" in result.stderr + result.stdout
        print("Regression verified: old color descriptor selection fails")
        mutant = harness.replace('      fetch = bound_fetch;', '      /* old sampler source */')
        assert mutant != harness
        mutation_source.write_text(mutant)
        subprocess.run([args.compiler, '-std=c++20', '-UNDEBUG', str(mutation_source),
                        '-o', str(mutation_binary)], check=True, timeout=45)
        result = subprocess.run([str(mutation_binary)], capture_output=True, timeout=10)
        assert result.returncode != 0, 'Stale sampler selection escaped the regression'
        assert b'HostDevice::last_fetch[4]' in result.stderr + result.stdout
        print('Regression verified: stale resource sampler selection fails')


if __name__ == "__main__":
    main()
