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
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    draw = (root / "rexlego/src/gpu_native/draw.cpp").read_text()
    textures = (root / "rexlego/src/gpu_native/textures.cpp").read_text()
    loop = block(draw, draw.index("  for (u32 i = 0; i < kNativeTextureSlots; ++i)",
                                 draw.index("bool BindConstants(")))
    upload_start = textures.index("{", textures.index("bool UploadTextureResource(")) + 1
    upload_entry = textures[upload_start:textures.index("  if (DepthAliasesEnabled()", upload_start)]
    hooks = "\n".join(block(draw, draw.index(signature)) for signature in
                       ("u32 DrawVerticesHook(", "u32 DrawIndexedVerticesHook("))
    harness = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <vector>
using u32=uint32_t; using u64=uint64_t; using i32=int32_t;
constexpr u32 kNativeTextureSlots=3;
enum class ShaderStage {kVertex,kPixel};
enum class D3DResourceType:u32 {kTexture=3,kVolumeTexture=4,kCubeTexture=5};
using TextureFetchWords=std::array<u32,6>;
#define REXLOG_INFO(...) ((void)0)
bool LongProbeEnabled(){return false;}
bool LongProbeOnce(u64){return false;}
template<class... Args>void LongProbeEvent(Args&&...){}
template<class T>int LongProbeHex(const T&){return 0;}
u64 BoundShaderHash(ShaderStage){return 0;}
u32 vertex_mask=0;
u32 BoundShaderTextureMask(ShaderStage stage){return stage==ShaderStage::kVertex?vertex_mask:0;}
namespace plume {struct RenderCommandList{};}
struct HostDevice {
 static int LockRecording(){return 0;}
 static u32 RegisterSampler(const TextureFetchWords&){++sampler_calls;return 7;}
 static inline u32 sampler_calls=0;
};
struct D3DDevice {struct {std::array<u32,6>dword{};}fetch_constants[kNativeTextureSlots];};
struct DrawBindings {std::array<u32,kNativeTextureSlots>textures{};};
struct Shared {
 std::array<u32,kNativeTextureSlots>samplers{};
 std::array<u32,kNativeTextureSlots>texture_2d{};
 std::array<u32,kNativeTextureSlots>texture_3d{1,1,1};
 std::array<u32,kNativeTextureSlots>texture_cube{2,2,2};
};
struct TextureResource {
 void* texture=reinterpret_cast<void*>(1);
 bool surface=false,resolved_on_host=false,cpu_upload_allowed=true;
 u32 guest_format=0;int format=0;
 u32 descriptor_index=3,d3d_type=u32(D3DResourceType::kTexture);
};
struct TextureResourceView {void*texture=nullptr;u32 descriptor_index=~u32{0},d3d_type=3;bool surface=false,depth=false;};
std::unordered_map<u32,std::shared_ptr<TextureResource>>resources;
std::shared_ptr<TextureResource>FindTexture(u32 address){
 auto it=resources.find(address);return it==resources.end()?nullptr:it->second;
}
TextureResourceView ResolveTextureResource(u32 address){
 auto resource=FindTexture(address);if(!resource)return {};
 return {resource->texture,resource->descriptor_index,resource->d3d_type};
}
struct UploadTimer{};
bool IsDepthFormat(int format){return format==22;}
bool UploadGuestDepthTexture(TextureResource&,plume::RenderCommandList*,bool){
 assert(false&&"Depth conversion is exercised by test_texture_depth_upload.py");return false;
}
u32 upload_calls=0,cpu_upload_calls=0;
std::vector<bool>require_hashes;
bool UploadTextureResource(u32 guest_address,plume::RenderCommandList* commands,bool require_content_hash){
 ++upload_calls;require_hashes.push_back(require_content_hash);
'''
    harness += upload_entry + r'''
 // Only allocation/conversion machinery below the production entry guard is
 // mocked. A residency or conversion failure is represented by false here.
 ++cpu_upload_calls;
 return resource&&commands&&resource->texture&&resource->cpu_upload_allowed;
}
bool BindTextureConstants(plume::RenderCommandList*commands,const D3DDevice*device,
                          const DrawBindings&bindings,u32 texture_mask,Shared&shared){
 constexpr bool trace_constants=false;
'''
    harness += loop + r'''
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
 resources.clear();bindings={};last_shared={};vertex_mask=0;texture_mask=7;
 upload_calls=cpu_upload_calls=query_failures=successful_draws=HostDevice::sampler_calls=0;
 require_hashes.clear();
}
std::shared_ptr<TextureResource>Add(u32 address,u32 slot){
 auto value=std::make_shared<TextureResource>();value->descriptor_index=address+2;
 resources[address]=value;bindings.textures[slot]=address;return value;
}
int main(){
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
  assert(HostDevice::sampler_calls==1&&last_shared.texture_2d[1]==0);
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
  Reset();bindings.textures[0]=99;Add(2,1)->descriptor_index=~u32{0};
  Add(3,2)->texture=nullptr;DrawVerticesHook(&device,0,0,3);
  assert(successful_draws==1&&query_failures==0&&upload_calls==0);
  assert((last_shared.texture_2d==std::array<u32,3>{0,0,0}));
  assert((last_shared.texture_3d==std::array<u32,3>{1,1,1}));
  assert((last_shared.texture_cube==std::array<u32,3>{2,2,2}));
 }
 {
  Reset();Add(1,0)->cpu_upload_allowed=false;texture_mask=0;
  DrawVerticesHook(&device,0,0,3);
  assert(successful_draws==1&&query_failures==0&&upload_calls==0);
 }
 std::cout<<"Production texture binding rejects failed uploads; GPU authority and null defaults preserved\n";
}
'''
    args.output.mkdir(parents=True, exist_ok=True)
    source = args.output.resolve() / "texture-binding.cpp"
    binary = args.output.resolve() / "texture-binding.exe"
    source.write_text(harness)
    subprocess.run([args.compiler, "-std=c++20", "-UNDEBUG", str(source), "-o", str(binary)],
                   check=True, timeout=45)
    subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    main()
