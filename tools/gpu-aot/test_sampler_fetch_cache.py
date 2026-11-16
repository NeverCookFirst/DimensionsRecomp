"""Differential replay of production sampler batches with a per-device fetch cache."""
import argparse,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
p.add_argument('--compiler',default='clang++');a=p.parse_args()
root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,LEGO_NATIVE_SAMPLER_FETCH_CACHE='1',LEGO_NATIVE_SAMPLER_THREAD_CACHE='0');env.pop('LEGO_NATIVE_NO_SAMPLER_BATCH',None)
r=subprocess.run([os.sys.executable,str(Path(__file__).with_name('test_sampler_batch.py')),
    str(out/'batch-boundary'),'--compiler',a.compiler],capture_output=True,text=True,timeout=60,env=env)
(out/'batch-boundary.log').write_text(r.stdout+r.stderr);r.check_returncode()
prefix=(out/'batch-boundary/samplers.cpp').read_text().split('int main(){',1)[0]
# The original batch fixture independently covers exhaustion at64 descriptors.
prefix=prefix.replace('kBindlessSamplerCount=64','kBindlessSamplerCount=4096')
prefix=prefix.replace('u32 RegisterSamplerLocked(','u64 resolve_calls=0;\nu32 RegisterSamplerLocked(',1)
prefix=prefix.replace('  auto desc = DecodeTextureSampler(fetch);','  ++resolve_calls;auto desc = DecodeTextureSampler(fetch);',1)
code=prefix+r'''
int main(){
 const char* setting=std::getenv("LEGO_NATIVE_SAMPLER_FETCH_CACHE");
 const bool cached=setting && *setting && std::string(setting)!="0";
 std::vector<TextureFetchWords> pool(65);
 for(u32 variant=0;variant<64;++variant){
  auto& f=pool[variant+1];f={2,4096,127|(127<<13),1<<19|1<<21|1<<23,7<<6,1<<9|1<<12};
  if(variant&1)f[0]|=2<<10;
  if(variant&2)f[1]=0;
  if(variant&4)f[2]=31|(31<<13);
  if(variant&8)f[3]=(f[3]&~(3<<23))|(2<<23);
  if(variant&16)f[4]|=960<<12; // signed -2 mip bias
  if(variant&32)f[5]|=1;
 }
 struct Draw{u32 mask;std::array<TextureFetchWords,32>fetches;};
 std::vector<Draw> draws;std::mt19937 random(0x5752084B);std::array<u32,32> selected{};
 for(u32 draw=0;draw<10000;++draw){
  Draw d;d.mask=draw%31==0?0:random();
  for(u32 i=0;i<32;++i){
   if(draw%13==0 || draw>=8000)selected[i]=random()%pool.size();
   d.fetches[i]=pool[selected[i]];
  }
  draws.push_back(d);
 }
 // Force every source word to affect an immediate following batch too.
 for(u32 word=0;word<6;++word){
  Draw d{};d.mask=1;d.fetches[0]=pool[1];draws.push_back(d);
  d.fetches[0]=pool[(1u<<word)+1];draws.push_back(d);
 }
 std::vector<std::array<u32,32>> reference;g_state=std::make_unique<State>();resolve_calls=0;
 for(const auto& d:draws){
  std::array<u32,32> ids;ids.fill(0xDEAD);
  for(u32 i=0;i<32;++i)if(d.mask&(1u<<i))ids[i]=HostDevice::RegisterSampler(d.fetches[i]);
  reference.push_back(ids);
 }
 const auto reference_keys=g_state->sampler_descriptors->keys;
 const auto scalar_calls=resolve_calls;g_state=std::make_unique<State>();resolve_calls=0;
 for(size_t draw=0;draw<draws.size();++draw){
  std::array<u32,32> ids;ids.fill(0xDEAD);const auto& d=draws[draw];
  HostDevice::RegisterSamplers(d.fetches,d.mask,ids);CHECK(ids==reference[draw]);
 }
 CHECK(g_state->sampler_descriptors->keys==reference_keys);
 const auto batch_calls=resolve_calls;
 CHECK(cached?batch_calls<scalar_calls/2:batch_calls==scalar_calls);
 // A device/state replacement starts with no valid descriptor reuse.
 g_state=std::make_unique<State>();resolve_calls=0;
 std::array<TextureFetchWords,32> fetches{};std::array<u32,32> ids{};
 HostDevice::RegisterSamplers(fetches,1,ids);CHECK(ids[0]>0&&resolve_calls==1&&g_state->device->created==1);
 HostDevice::RegisterSamplers(fetches,1,ids);CHECK(resolve_calls==(cached?1:2));
 // Failed creation must be retried, even for repeated identical fetch words.
 g_state=std::make_unique<State>();resolve_calls=0;g_state->device->fail=true;
 HostDevice::RegisterSamplers(fetches,1,ids);CHECK(ids[0]==0&&resolve_calls==1);
 HostDevice::RegisterSamplers(fetches,1,ids);CHECK(ids[0]==0&&resolve_calls==2);
 g_state->device->fail=false;HostDevice::RegisterSamplers(fetches,1,ids);
 CHECK(ids[0]>0&&resolve_calls==3);const auto old_id=ids[0];
 fetches[0]=pool[17];g_state->device->fail=true;
 HostDevice::RegisterSamplers(fetches,1,ids);CHECK(ids[0]==0&&resolve_calls==4);
 g_state->device->fail=false;HostDevice::RegisterSamplers(fetches,1,ids);
 CHECK(ids[0]>0&&ids[0]!=old_id&&resolve_calls==5);
 std::printf("PASS sampler cache=%u draws=%zu scalar_resolves=%llu batch_resolves=%llu keys=%zu; all6 words, state reset, failure/recovery\n",
  cached,draws.size(),(unsigned long long)scalar_calls,(unsigned long long)batch_calls,reference_keys.size());
}
'''
def run(name,body,mode):
    cpp=out/(name+'.cpp');exe=out/(name+'.exe');cpp.write_text(body)
    r=subprocess.run([a.compiler,'-std=c++20','-O2','-DNOMINMAX','-D_CRT_SECURE_NO_WARNINGS',
        '-I'+str(root/'rexlego/src'),'-I'+str(root/'thirdparty/plume'),str(cpp),'-o',str(exe)],
        capture_output=True,text=True,timeout=45)
    (out/(name+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
    child=dict(env,LEGO_NATIVE_SAMPLER_FETCH_CACHE=mode)
    r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15,env=child)
    (out/(name+'.log')).write_text(r.stdout+r.stderr);return r
positive=[]
for mode in ('0','1'):
    r=run('cache-'+mode,code,mode);assert r.returncode==0,r.stdout+r.stderr
    positive.append(dict(cache=mode,exit=r.returncode,stdout=r.stdout));print(r.stdout)
negative=[]
for name,old,new in [
    ('partial-fetch-key','state.sampler_last_fetches[i]==fetches[i]','state.sampler_last_fetches[i][0]==fetches[i][0]'),
    ('reuse-failed-descriptor','state.sampler_last_descriptors[i] && ','')]:
    assert code.count(old)==1,(name,code.count(old))
    r=run(name,code.replace(old,new),'1');assert r.returncode==23,(name,r.stdout,r.stderr)
    negative.append(dict(name=name,exit=r.returncode,stderr=r.stderr))
(out/'verification.json').write_text(json.dumps(dict(game_launched=False,positive=positive,negative=negative,
    production_sampler_decoder_registration_and_batch=True,
    scope='actual sampler decoder/canonical key/registration/batch; per-device driver allocation and descriptor table mocked; synthetic fetch traces, not observed game hit rate or FPS'),indent=2))
