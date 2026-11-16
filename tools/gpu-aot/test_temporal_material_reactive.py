"""Exercise production replay selection and coverage blend-state configuration."""
import argparse,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++');a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True);root=Path(__file__).resolve().parents[2]
source=(root/'rexlego/src/gpu_native/draw.cpp').read_text()
start=source.index('  const auto coverage_mode =',source.index('plume::RenderPipeline* GetTemporalPipeline('))
classify=source[start:source.index('  const bool companion =',start)]
start=source.index('    auto motion_key = key;');selection=source[start:source.index('    auto* motion_pipeline =',start)]
start=source.index('  if (material_coverage) {',source.index('plume::RenderPipeline* GetTemporalPipeline('));end=source.index('  desc.depthTargetFormat',start)
blend=source[start:end]
fixture=r'''
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include "gpu_native/temporal_coverage.h"
using namespace legodimensions::gpu_native;using u32=uint32_t;using u64=uint64_t;
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(23);}}while(0)
namespace plume{enum class RenderFormat{UNKNOWN,Depth};enum class RenderBlend{ZERO,ONE};enum class RenderBlendOperation{ADD,MAX};}
bool available=true;unsigned calls=0;
bool HasTemporalCoverageShader(u64 hash,TemporalCoverage mode,u32 spec){++calls;return available&&hash==0x75A4D1ABA62F7594ull&&mode!=TemporalCoverage::kNone;}
struct Key{u64 pixel_shader_hash=0x75A4D1ABA62F7594ull;u32 depth_control=2,depth_format=1,spec_constants=0,blend_controls[1]{0x01010106};}key;
bool MaterialForDepth(plume::RenderFormat depth_format){
'''+classify+r'''
 return material_coverage;
}
auto Select(bool temporal_scene){
'''+selection+r'''
 return motion_key;
}
struct Blend{unsigned renderTargetWriteMask=15;bool blendEnabled=false;plume::RenderBlend srcBlend=plume::RenderBlend::ZERO,dstBlend=plume::RenderBlend::ZERO,srcBlendAlpha=plume::RenderBlend::ZERO,dstBlendAlpha=plume::RenderBlend::ZERO;plume::RenderBlendOperation blendOp=plume::RenderBlendOperation::ADD,blendOpAlpha=plume::RenderBlendOperation::ADD;};
struct Desc{Blend renderTargetBlend[2];};
auto Configure(bool material_coverage){Desc desc;
'''+blend+r'''
 return desc;
}
int main(int argc,char**argv){
 const bool enabled=argc==1;
 CHECK(MaterialForDepth(plume::RenderFormat::UNKNOWN)==enabled);
 CHECK(MaterialForDepth(plume::RenderFormat::Depth)==enabled);
 auto selected=Select(false);CHECK(selected.pixel_shader_hash==(enabled?key.pixel_shader_hash:0));CHECK(!selected.depth_control&&!selected.depth_format);
 if(!enabled)return 0;
 calls=0;selected=Select(true);CHECK(selected.pixel_shader_hash==key.pixel_shader_hash&&selected.depth_control==2&&selected.depth_format==1&&!calls);
 available=false;CHECK(!Select(false).pixel_shader_hash);available=true;
 key.pixel_shader_hash=123;CHECK(!Select(false).pixel_shader_hash);key.pixel_shader_hash=0x75A4D1ABA62F7594ull;
 const auto mode=[](unsigned source,unsigned destination,unsigned op=0){return TemporalCoverageForBlend(DecodeColorBlend(source|(op<<5)|(destination<<8)|(1<<16)|(1<<24)));};
 CHECK(TemporalCoverageForBlend(DecodeColorBlend(0x00010001))==TemporalCoverage::kOpaque);CHECK(mode(6,7)==TemporalCoverage::kAlpha);
 CHECK(mode(6,1)==TemporalCoverage::kAddAlpha);CHECK(mode(1,1)==TemporalCoverage::kAdd);CHECK(mode(1,7)==TemporalCoverage::kPremultiplied);
 CHECK(mode(6,1,1)==TemporalCoverage::kNone);CHECK(mode(10,7)==TemporalCoverage::kNone);CHECK(mode(2,1)==TemporalCoverage::kNone);
 key.blend_controls[0]=6|(1<<5)|(1<<8)|(1<<16)|(1<<24);CHECK(!Select(false).pixel_shader_hash);
 auto desc=Configure(true);CHECK(!desc.renderTargetBlend[0].renderTargetWriteMask);
 auto reactive=desc.renderTargetBlend[1];CHECK(reactive.renderTargetWriteMask==15&&reactive.blendEnabled);
 CHECK(reactive.srcBlend==plume::RenderBlend::ONE&&reactive.dstBlend==plume::RenderBlend::ONE&&reactive.blendOp==plume::RenderBlendOperation::MAX);
 desc=Configure(false);CHECK(desc.renderTargetBlend[0].renderTargetWriteMask==15&&!desc.renderTargetBlend[1].blendEnabled);
}
'''
cpp=a.output/'material-reactive.cpp';exe=a.output/'material-reactive.exe';cpp.write_text(fixture)
command=[a.compiler,'-std=c++20','-O2','-I'+str(root/'rexlego/src'),str(cpp),'-o',str(exe)]
subprocess.run(command,check=True);env=dict(os.environ);env.pop('LEGO_NATIVE_MATERIAL_REACTIVE',None)
subprocess.run([str(exe.resolve())],env=env,check=True);env['LEGO_NATIVE_MATERIAL_REACTIVE']='0';subprocess.run([str(exe.resolve()),'disabled'],env=env,check=True)
print('PASS actual replay and PSO blend blocks: material identity/availability, scene exclusion, five blend modes, unsupported blends, startup disable, retained motion and MAX rejection')
