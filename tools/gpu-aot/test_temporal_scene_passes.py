"""Exercise actual draw classification with deferred lights, fog and unknown UI."""
import argparse
from pathlib import Path
import os
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++');a=p.parse_args()
root=Path(__file__).resolve().parents[2];a.output.mkdir(parents=True,exist_ok=True)
draw=(root/'rexlego/src/gpu_native/draw.cpp').read_text()
begin=draw.index('  const bool temporal_image_domain =')
block=draw[begin:draw.index('  bool temporal_previous_valid =',begin)]
fixture=r'''
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include "gpu_native/depth_state.h"
#include "gpu_native/temporal_scene_pass.h"
using namespace legodimensions::gpu_native;
using u32=uint32_t;using u64=uint64_t;
#define CHECK(x) do {if(!(x)){fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);exit(23);}}while(0)
bool fsr=true;bool NativeFsr3Enabled(){return fsr;}
enum class ShaderStage{kPixel};u32 sampled=0x45,mask_reads=0,extent_reads=0;
constexpr u32 kNativeTextureSlots=32;
u32 BoundShaderTextureMask(ShaderStage){++mask_reads;return sampled;}
struct Blend{bool enabled;};Blend DecodeColorBlend(u32 word){return {bool(word)};}
std::array<NativeRenderExtent,32> extents;
NativeRenderExtent NativeTextureExtent(u32 id){++extent_reads;return extents[id-1];}
struct Key{u64 pixel_shader_hash=0x745F43DE42185A41ull;u32 depth_control=0,blend_controls[1]{1};}key;
struct Bindings{std::array<u32,32>textures{};}bindings;
struct Device{struct{float x=0,y=0;}viewport;}object,*device=&object;
struct{void*texture=nullptr;}depth;
NativeRenderExtent raster_extent{1280,720,1129,635};
bool temporal_scene=false;u32 count=36;float viewport_width=1280,viewport_height=720;
bool Classify(){
'''+block+r'''
return temporal_image_pass;
}
int main(int argc,char**argv){
 const bool enabled=argc==1||std::strcmp(argv[1],"disabled")!=0;
 for(u32 i=0;i<32;++i){bindings.textures[i]=i+1;extents[i]=raster_extent;}
 const std::array<u64,16>programs{0x081435C474061F7Eull,0xFC2BDE055D199EE8ull,
  0x0E2FA249CEC37FF0ull,0xB2A918B7FD4ECADCull,0x3DF8FF80E2A08043ull,0x32EAE4018791DA38ull,
  0x745F43DE42185A41ull,0xE85D26243917A70Dull,0x7DDDE95EB4B5A14Dull,0x04FB6886587A4462ull,
  0x82DB1B788F68E33Bull,0xF4CB6445FC4DF523ull,0xCEC8AAACB206BA39ull,0x4BB762F6258DC7D9ull,
  0xC700ED5C1AAD294Aull,0xFF08B82BB808075Full};
 for(auto hash:programs){key.pixel_shader_hash=hash;sampled=TemporalSceneInputMask(hash);
  for(auto size:std::array<NativeRenderExtent,3>{{{1280,720,640,360},{1280,720,1129,635},{1280,720,1920,1080}}}){
   raster_extent=size;extents.fill(size);
   for(auto n:{3u,36u,810u}){count=n;CHECK(Classify()==enabled);}
  }
 }
 if(!enabled)return 0;
 key.pixel_shader_hash=0x745F43DE42185A41ull;sampled=0x45;count=36;
 for(u32 slot:{0u,2u,6u}){
  sampled&=~(1u<<slot);CHECK(!Classify());sampled|=1u<<slot;
  bindings.textures[slot]=0;CHECK(!Classify());bindings.textures[slot]=slot+1;
  --extents[slot].width;CHECK(!Classify());++extents[slot].width;
  --extents[slot].logical_width;CHECK(!Classify());++extents[slot].logical_width;
 }
 // A full-screen UI quad with scene-sized stale bindings is still unknown.
 key.pixel_shader_hash=0x75A4D1ABA62F7594ull;count=4;CHECK(!Classify());
 key.pixel_shader_hash=0x745F43DE42185A41ull;count=36;
 fsr=false;mask_reads=extent_reads=0;CHECK(!Classify());CHECK(!mask_reads&&!extent_reads);fsr=true;
 temporal_scene=true;CHECK(!Classify());temporal_scene=false;
 depth.texture=&depth;key.depth_control=2;CHECK(!Classify());key.depth_control=0;depth.texture=nullptr;
 viewport_width=640;CHECK(!Classify());viewport_width=1280;
 device->viewport.x=1;CHECK(!Classify());device->viewport.x=0;
 raster_extent.logical_height=719;CHECK(!Classify());raster_extent.logical_height=720;
 CHECK(Classify());
 // The actual081 light binds a4x4 white albedo multiplier. It derives scene
 // geometry from depth; that binding is not required to have scene extent.
 key.pixel_shader_hash=0x081435C474061F7Eull;sampled=0x41;
 extents[6]={4,4,4,4};count=810;CHECK(Classify());
 sampled=0x40;CHECK(!Classify());sampled=0x41;
 extents[0].width-=1;CHECK(!Classify());extents[0].width+=1;
 extents[6]=raster_extent;
 // Existing opaque image-copy policy still preserves a scene image.
 key.pixel_shader_hash=123;key.blend_controls[0]=0;count=3;CHECK(Classify());
 sampled=0;CHECK(!Classify());
}
'''
cpp=a.output/'scene-pass.cpp';exe=a.output/'scene-pass.exe';cpp.write_text(fixture)
command=[a.compiler,'-std=c++20','-O2','-I'+str(root/'rexlego/src'),str(cpp),'-o',str(exe)]
subprocess.run(command,check=True)
env=dict(os.environ);env.pop('LEGO_NATIVE_SCENE_PASS_HISTORY',None)
subprocess.run([str(exe.resolve())],env=env,check=True)
env['LEGO_NATIVE_SCENE_PASS_HISTORY']='0'
subprocess.run([str(exe.resolve()),'disabled'],env=env,check=True)
# Broadly preserving every scene-sized draw must fail the unknown UI case.
bad=a.output/'scene-pass-mutant.cpp';badexe=a.output/'scene-pass-mutant.exe'
bad.write_text(fixture.replace('TemporalSceneInputMask(key.pixel_shader_hash)) {','true) {')
    .replace('PreservesTemporalSceneHistory(key.pixel_shader_hash,','PreservesTemporalSceneHistory(TemporalSceneInputMask(key.pixel_shader_hash) ? key.pixel_shader_hash : 0x745F43DE42185A41ull,'))
subprocess.run([*command[:-3],str(bad),'-o',str(badexe)],check=True,capture_output=True)
env.pop('LEGO_NATIVE_SCENE_PASS_HISTORY',None)
assert subprocess.run([str(badexe.resolve())],env=env).returncode==23
print('PASS actual draw block: 16 verified programs/aliases, 3 render sizes, triangle/36/810-index lights, active bindings, unknown UI, depth, viewport, native fast path and startup disable')
print('PASS broad scene-sized bypass mutant fails unknown UI protection')
