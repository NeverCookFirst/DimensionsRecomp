"""Fault-inject actual FSR context/buffer preparation and commit functions."""
from pathlib import Path
import argparse, hashlib, json, subprocess


def body(text, signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]


def main():
    p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++');a=p.parse_args()
    root=Path(__file__).resolve().parents[2];source=root/'rexlego/src/gpu_native/temporal_present.cpp';text=source.read_text()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    prefix=r'''
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d failed: %s\n",__LINE__,#x); std::exit(23); } } while (0)
#define REXLOG_INFO(...) ((void)0)
using u32=uint32_t;
unsigned allocations=0,views=0,contexts=0,registrations=0,names=0;
unsigned fail_at=0,next_descriptor=100;
bool null_texture=false,null_native=false,fail_view=false,fail_descriptor=false,fail_context=false,has_device=true;
std::set<u32> live_slots;
struct ID3D12Resource {void SetName(const wchar_t*){++names;}};
namespace plume {
enum class RenderFormat {R16G16_FLOAT,R8_UNORM,R16G16B16A16_FLOAT};
enum class RenderTextureDimension {TEXTURE_2D};enum class RenderSampleCount{COUNT_1};
enum class RenderTextureFlag{UNORDERED_ACCESS,RENDER_TARGET};
struct RenderTextureDesc {RenderTextureDimension dimension;u32 width,height,depth,mipLevels,arraySize;struct{RenderSampleCount sampleCount;}multisampling;RenderFormat format;bool committed;RenderTextureFlag flags;};
struct RenderTextureViewDesc {static auto Texture2D(RenderFormat){return RenderTextureViewDesc{};}};
struct RenderTextureView{};struct RenderFramebuffer{};
struct RenderTexture {
 virtual ~RenderTexture()=default;
 std::unique_ptr<RenderTextureView> createTextureView(RenderTextureViewDesc){++views;if(fail_view)return{};return std::make_unique<RenderTextureView>();}
};
struct D3D12Texture:RenderTexture {ID3D12Resource resource;ID3D12Resource*d3d=&resource;};
struct RenderDevice {
 virtual ~RenderDevice()=default;
 std::unique_ptr<RenderTexture> createTexture(RenderTextureDesc){++allocations;if(null_texture&&allocations==fail_at)return{};
  auto result=std::make_unique<D3D12Texture>();if(null_native&&allocations==fail_at)result->d3d=nullptr;return result;}
};
struct D3D12Device:RenderDevice {void*d3d=this;};
}
struct Fsr3Size {u32 width,height;};
struct Fsr3Upscaler {
 unsigned resets=0;
 static std::shared_ptr<Fsr3Upscaler> Create(void*,Fsr3Size,Fsr3Size,bool){++contexts;if(fail_context)return{};return std::make_shared<Fsr3Upscaler>();}
 void InvalidateHistory(){++resets;}
};
std::vector<std::shared_ptr<void>> retired;
plume::D3D12Device device;
struct HostDevice {
 static plume::RenderDevice* Device(){return has_device?&device:nullptr;}
 static u32 RegisterTexture(plume::RenderTexture*,plume::RenderTextureView*){++registrations;if(fail_descriptor)return~0u;live_slots.insert(next_descriptor);return next_descriptor++;}
 static void UnregisterTexture(u32 id){CHECK(live_slots.erase(id)==1);}
 static void RetireResource(std::shared_ptr<void> value){if(value)retired.push_back(std::move(value));}
};
'''
    prefix+=body(text,'struct PreparedTemporalSettings {')+';\n'
    prefix+=r'''
struct Presentation {
 std::shared_ptr<Fsr3Upscaler>upscaler;
 std::shared_ptr<plume::RenderTexture>motion,reactive,output;
 std::shared_ptr<plume::RenderTextureView>output_view;
 std::shared_ptr<plume::RenderFramebuffer>framebuffer;
 plume::RenderTexture*attachment=nullptr;
 u32 descriptor=~0u,width=0,height=0;
 uint64_t epoch=1;bool inverted=false;
 std::optional<int>previous_camera;
}g;
uint64_t temporal_settings_generation=1;
std::atomic<u32>presentation_status{0};
'''
    funcs='\n'.join(body(text,s) for s in ['std::shared_ptr<plume::RenderTexture> Texture(',
        'ID3D12Resource* Raw(','std::shared_ptr<PreparedTemporalSettings> PrepareTemporalSettings(',
        'bool CanCommitTemporalSettings(','bool CommitTemporalSettings(','void ResetTemporalPresentation('])
    tests=r'''
void setup(){
 g={};++temporal_settings_generation;retired.clear();live_slots.clear();
 allocations=views=contexts=registrations=names=fail_at=0;next_descriptor=100;
 null_texture=null_native=fail_view=fail_descriptor=fail_context=false;has_device=true;
 g.upscaler=std::make_shared<Fsr3Upscaler>();g.motion=std::make_shared<plume::D3D12Texture>();
 g.reactive=std::make_shared<plume::D3D12Texture>();g.output=std::make_shared<plume::D3D12Texture>();
 g.output_view=std::make_shared<plume::RenderTextureView>();g.framebuffer=std::make_shared<plume::RenderFramebuffer>();
 g.attachment=g.output.get();g.width=1280;g.height=720;g.descriptor=10;g.epoch=7;g.previous_camera=4;live_slots.insert(10);
}
struct Old {Presentation state=g;uint64_t generation=temporal_settings_generation;};
void unchanged(const Old&old){
 CHECK(g.upscaler==old.state.upscaler&&g.motion==old.state.motion&&g.reactive==old.state.reactive&&g.output==old.state.output);
 CHECK(g.output_view==old.state.output_view&&g.framebuffer==old.state.framebuffer&&g.attachment==old.state.attachment);
 CHECK(g.width==old.state.width&&g.height==old.state.height&&g.descriptor==old.state.descriptor&&g.epoch==old.state.epoch);
 CHECK(g.previous_camera==old.state.previous_camera&&temporal_settings_generation==old.generation&&retired.empty());
}
int main(){unsigned cases=0;
 for(int kind=0;kind<5;++kind){for(unsigned at=1;at<=(kind==1||kind==2?3u:1u);++at){
  setup();Old old;
  fail_context=kind==0;null_texture=kind==1;null_native=kind==2;fail_view=kind==3;fail_descriptor=kind==4;fail_at=at;
  CHECK(!PrepareTemporalSettings(960,540,true));unchanged(old);CHECK(live_slots==std::set<u32>{10});++cases;
 }}
 {setup();Old old;auto plan=PrepareTemporalSettings(960,540,true);CHECK(plan&&CanCommitTemporalSettings(plan));unchanged(old);
  CHECK(live_slots.size()==2&&contexts==1&&allocations==3&&views==1&&names==3);
  plan.reset();unchanged(old);CHECK(live_slots==std::set<u32>{10});++cases;}
 {setup();Old old;auto plan=PrepareTemporalSettings(960,540,true);CHECK(CommitTemporalSettings(plan,false));
  CHECK(g.upscaler!=old.state.upscaler&&g.motion!=old.state.motion&&g.output!=old.state.output&&g.inverted);
  CHECK(g.width==960&&g.height==540&&g.descriptor==100&&!live_slots.contains(10)&&live_slots.contains(100));
  CHECK(g.attachment==nullptr&&!g.previous_camera&&retired.size()==6);
  CHECK(!CanCommitTemporalSettings(plan)&&!CommitTemporalSettings(plan,false));plan.reset();CHECK(live_slots.contains(100));++cases;}
 {setup();Old old;auto plan=PrepareTemporalSettings(1280,720,true);unchanged(old);
  CHECK(contexts==1&&allocations==0&&registrations==0);CHECK(CommitTemporalSettings(plan,false));
  CHECK(g.upscaler!=old.state.upscaler&&g.motion==old.state.motion&&g.output==old.state.output&&retired.size()==1&&g.inverted);++cases;}
 {setup();Old old;fail_context=fail_view=fail_descriptor=null_texture=true;auto plan=PrepareTemporalSettings(1280,720,false);
  CHECK(plan&&contexts==0&&allocations==0);unchanged(old);CHECK(CommitTemporalSettings(plan,false));
  CHECK(g.upscaler==old.state.upscaler&&retired.empty()&&g.epoch==old.state.epoch);++cases;}
 {setup();auto plan=PrepareTemporalSettings(1280,720,false);CHECK(CommitTemporalSettings(plan,true));
  CHECK(g.upscaler->resets==1&&!g.previous_camera&&g.epoch==8);++cases;}
 {setup();auto first=PrepareTemporalSettings(960,540,false),second=PrepareTemporalSettings(640,360,false);
  CHECK(first&&second&&CommitTemporalSettings(first,false));auto active=g.upscaler;
  CHECK(!CanCommitTemporalSettings(second)&&!CommitTemporalSettings(second,false)&&g.upscaler==active);++cases;}
 {setup();auto plan=PrepareTemporalSettings(960,540,false);ResetTemporalPresentation();
  CHECK(!CanCommitTemporalSettings(plan)&&!CommitTemporalSettings(plan,false)&&!g.upscaler);plan.reset();++cases;}
 {setup();has_device=false;CHECK(!PrepareTemporalSettings(960,540,false));++cases;}
 {setup();CHECK(!PrepareTemporalSettings(0,540,false)&&!PrepareTemporalSettings(960,0,false)&&contexts==0);++cases;}
 std::printf("PASS %u temporal preparation cases: failed context/texture/native-wrapper/view/descriptor allocation preserves old state, abandoned plans, atomic ownership transfer, reuse, reset and stale-plan guards\n",cases);
}
'''
    def run(label,selected=funcs,pre=prefix):
        cpp=out/(label+'.cpp');cpp.write_text(pre+selected+tests);exe=out/(label+'.exe')
        r=subprocess.run([a.compiler,'-std=c++23','-O2',str(cpp),'-o',str(exe)],capture_output=True,text=True,timeout=60)
        (out/(label+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
        r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20);(out/(label+'.log')).write_text(r.stdout+r.stderr);return r
    positive=run('production');positive.check_returncode();print(positive.stdout,end='',flush=True)
    negative=[]
    for label,old,new,pre in [
        ('stale-plan-publishes','prepared->generation==temporal_settings_generation &&','',False),
        ('release-published-output','std::exchange(prepared->descriptor,~0u)','prepared->descriptor',False),
        ('leak-abandoned-output','if (descriptor!=~0u) HostDevice::UnregisterTexture(descriptor);','',True),
    ]:
        source=prefix if pre else funcs;assert source.count(old)==1,label
        r=run(label,selected=funcs if pre else funcs.replace(old,new),pre=prefix.replace(old,new) if pre else prefix)
        assert r.returncode==23,(label,r.returncode,r.stdout,r.stderr);negative.append(dict(name=label,exit=r.returncode,error=r.stderr))
    files=[source,source.with_suffix('.h'),Path(__file__).resolve()] if isinstance(source,Path) else [root/'rexlego/src/gpu_native/temporal_present.cpp',root/'rexlego/src/gpu_native/temporal_present.h',Path(__file__).resolve()]
    (out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,output=positive.stdout,negative=negative,
        sources={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
        scope='Actual prepared temporal state/resource ownership functions; FSR context, D3D12 wrappers, allocations/descriptors and fences are injected. Real context/GPU presentation is verified separately.'),indent=2))


if __name__=='__main__':main()
