"""Check D24 numeric/bank mapping and execute the actual native alias tracker."""
import argparse,json,subprocess,re
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path)
p.add_argument('--compiler',default='clang++')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
source=(root/'rexlego/src/gpu_native/textures.cpp').read_text()
def function(name):
    start=source.index(name+'(')
    start=source.rfind('\n',0,start)+1
    end=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
shader=(root/'rexlego/src/gpu_native/shaders/depth_restore_ps.hlsl').read_text()
shader_mapping=re.search(r'float sourceX = (.*);',shader).group(1).replace('position.x','position')
assert 'float halfTile = g_PushConstants.Param0;' in shader
code=r'''
#include "gpu_native/depth_alias.h"
#include <cassert>
#include <cmath>
#include <memory>
#include <unordered_map>
using u32=uint32_t;using u64=uint64_t;
namespace plume {enum class RenderFormat {R8G8B8A8_UNORM,OTHER};}
namespace legodimensions::gpu_native {
struct TextureResource {
  bool surface=true;u32 edram_base=1328,guest_msaa=0,guest_format=22,guest_address=1;
  u32 width=1280,height=720,logical_width=1280,descriptor_index=7;u64 write_generation=0;
  plume::RenderFormat format=plume::RenderFormat::OTHER;
  std::unique_ptr<int> texture=std::make_unique<int>(42);
  u32 LogicalWidth()const{return logical_width;}
  u32 HostWidth()const{return width;}u32 HostHeight()const{return height;}
};
u64 g_depth_alias_generation=0;
std::unordered_map<u32,std::weak_ptr<TextureResource>> g_edram_colors;
std::unordered_map<u32,std::shared_ptr<TextureResource>> resources;
bool enabled=true;
bool DepthAliasesEnabled(){return enabled;}
auto FindTexture(u32 address){auto f=resources.find(address);return f==resources.end()?nullptr:f->second;}
u32 transfers=0,expected_width=1280,expected_height=720;float expected_half=40;bool transfer_ok=true;
struct HostDevice {
static bool TransferDepthAlias(int* src,u32 descriptor,int* dst,u32 w,u32 h,bool restore,float half){
assert(src&&dst&&src!=dst&&descriptor==7&&w==expected_width&&h==expected_height&&restore&&half==expected_half);
++transfers;return transfer_ok;}
};
#define REXLOG_INFO(...) ((void)0)
''' + 'uint32_t ShaderAliasX(uint32_t x,float halfTile,uint32_t width=1920){uint32_t halfIndex=((x*2+1)*uint32_t(std::round(float(width)/halfTile)))/(width*2);float position=float(x)+0.5f;float sourceX='+shader_mapping+';return uint32_t(sourceX);}\n' + function('MarkSurfaceWritten')+'\n'+function('PrepareSurfaceDepthAlias')+r'''
}
int main(){
 using namespace legodimensions::gpu_native;
 for(u32 z=0;z<0x1000000;++z) {
   const float actual=UnpackDepth24(z);
   assert(std::abs(double(actual)-double(z)/double(0xFFFFFF))<=0.5/16777215.0);
   if(z) assert(actual>UnpackDepth24(z-1));
 }
 assert(UnpackDepth24(0)==0 && UnpackDepth24(0xFFFFFF)==1);
 assert(UnpackDepth24(0xC00000)==0.750000059604644775390625f);
 for(u32 y=0;y<720;++y) for(u32 x=0;x<1280;++x){
   const u32 alias=DepthColorAliasX(x);
   assert(alias<1280&&DepthColorAliasX(alias)==x);
   const u32 tile=(y/16)*16+x/80;
   const u32 depth_address=tile*1280+(y%16)*80+((x%80+40)%80);
   const u32 color_address=(y/16*16+alias/80)*1280+(y%16)*80+alias%80;
   assert(depth_address==color_address);
 }
 assert(DepthColorAliasHalfTile(1280,1280)==40);
 assert(DepthColorAliasHalfTile(1280,1920)==60);
 assert(!DepthColorAliasHalfTile(0,1920));
 assert(DepthColorAliasHalfTile(1280,1129)==35.28125f);
 assert(DepthColorAliasHalfTile(1280,1919)==59.96875f);
 // Exact integer rational oracle for the guest's normalized +/- 1/32 fetch.
 for(u32 w:{640u,960u,1129u,1280u,1919u,1920u})for(u32 x=0;x<w;++x){
   const uint64_t center=uint64_t(2*x+1)*16;
   const u32 half=uint32_t(center/w);
   const u32 expected=u32((half%2 ? center-w : center+w)/32);
   assert(expected<w && ShaderAliasX(x,DepthColorAliasHalfTile(1280,w),w)==expected);
 }
 for(u32 x=0;x<1920;++x){
   const u32 alias=DepthColorAliasX(x,60);
   assert(ShaderAliasX(x,60)==alias);
   assert(alias<1920 && DepthColorAliasX(alias,60)==x);
   assert(alias/120==x/120 && (alias%120+60)%120==x%120);
 }
 assert(DepthColorAliasX(0,60)!=DepthColorAliasX(0));
 DepthAliasLayout layout{0x1E4D8000,1280,720,40,1,2,1,1};
 assert(CompatibleDepthAlias(layout,layout));
 constexpr u32 DepthAliasLayout::* fields[]={&DepthAliasLayout::physical_base,
   &DepthAliasLayout::width,&DepthAliasLayout::height,&DepthAliasLayout::pitch,
   &DepthAliasLayout::tiled,&DepthAliasLayout::endian,&DepthAliasLayout::levels,
   &DepthAliasLayout::dimension};
 for(auto field:fields){auto different=layout;
   different.*field^=1;assert(!CompatibleDepthAlias(layout,different));}
 auto color=std::make_shared<TextureResource>();color->guest_address=2;
 color->format=plume::RenderFormat::R8G8B8A8_UNORM;color->guest_format=6;
 auto depth=std::make_shared<TextureResource>();
 resources[1]=depth;resources[2]=color;
 assert(PrepareSurfaceDepthAlias(1)&&transfers==0);
 MarkSurfaceWritten(2);assert(PrepareSurfaceDepthAlias(1)&&transfers==1);
 assert(PrepareSurfaceDepthAlias(1)&&transfers==1); // unchanged source
 MarkSurfaceWritten(1);assert(PrepareSurfaceDepthAlias(1)&&transfers==1); // own depth write newer
 MarkSurfaceWritten(2);transfer_ok=false;
 assert(!PrepareSurfaceDepthAlias(1)&&transfers==2);
 transfer_ok=true;assert(PrepareSurfaceDepthAlias(1)&&transfers==3); // failed copy never committed
 MarkSurfaceWritten(2);depth->guest_msaa=2;
 assert(PrepareSurfaceDepthAlias(1)&&transfers==3);depth->guest_msaa=0;
 depth->width=640;assert(PrepareSurfaceDepthAlias(1)&&transfers==3);depth->width=1280;
 depth->edram_base=1536;assert(PrepareSurfaceDepthAlias(1)&&transfers==3);depth->edram_base=1328;
 depth->guest_format=23;assert(PrepareSurfaceDepthAlias(1)&&transfers==3);depth->guest_format=22;
 enabled=false;assert(PrepareSurfaceDepthAlias(1)&&transfers==3);enabled=true;
 color->width=depth->width=1920;color->height=depth->height=1080;
 expected_width=1920;expected_height=1080;expected_half=60;
 assert(PrepareSurfaceDepthAlias(1)&&transfers==4);
 MarkSurfaceWritten(2);depth->logical_width=640;
 assert(PrepareSurfaceDepthAlias(1)&&transfers==4);depth->logical_width=1280;
 assert(PrepareSurfaceDepthAlias(1)&&transfers==5);
 MarkSurfaceWritten(2);color->width=depth->width=1129;color->height=depth->height=635;
 expected_width=1129;expected_height=635;expected_half=35.28125f;
 assert(PrepareSurfaceDepthAlias(1)&&transfers==6);
 resources.erase(2);color.reset();assert(PrepareSurfaceDepthAlias(1)&&transfers==6);
}
'''
cpp=a.output/'depth-alias.cpp';exe=a.output/'depth-alias.exe'
cpp.write_text(code)
subprocess.run([a.compiler,'-std=c++20','-O2','-I',str(root/'rexlego/src'),str(cpp),'-o',str(exe)],check=True)
subprocess.run([str(exe.resolve())],check=True)
report={'passed':True,'unorm24_values':16777216,'edram_coordinates':1280*720,
 'actual_tracker_bodies':True,'layout_rejection':True,'write_order_and_failed_retry':True,
 'expired_source_and_disabled_path':True,'scaled_edram_coordinates':1920,'actual_restore_shader_expression':True,'gpu_shader_execution':'runtime capture required'}
(a.output/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report))
