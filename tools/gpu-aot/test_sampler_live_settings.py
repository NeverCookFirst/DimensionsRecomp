"""Exercise actual native sampler registration/caches while filtering changes.

The descriptor allocator is mocked. No window or game process is created.
"""
import argparse, hashlib, json, os, subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_frame_ring_transaction import function

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',type=Path)
    parser.add_argument('--compiler',default='clang++')
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[2]
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    source=root/'rexlego/src/gpu_native/device.cpp'
    text=source.read_text()
    request=function(text,'int32_t NativeAnisotropicRequested()')
    present=function(text,'bool HostDevice::PresentTexture(')
    assert present.index('auto recording = LockRecording();') < present.index('NativeAnisotropicRequested()') < present.index('g_anisotropic_override.exchange(')
    assert 'g_anisotropic_override.store(NativeAnisotropicRequested(), std::memory_order_release);' in text
    subprocess.run([sys.executable,str(Path(__file__).with_name('test_sampler_batch.py')),
                    str(out/'fixture'),'--emit-only'],check=True,timeout=20)
    prefix=(out/'fixture/samplers.cpp').read_text().split('int main(){',1)[0]
    prefix=prefix.replace('kBindlessSamplerCount=64','kBindlessSamplerCount=4096')
    prefix=prefix.replace('namespace rex::cvar {std::string GetFlagByName(const char*){return "-1";}}',r'''
namespace rex::cvar {
std::string native="-2",legacy;
std::string GetFlagByName(const char* key){
 if(std::string(key)=="native_anisotropic_override") return native;
 CHECK(std::string(key)=="anisotropic_override");return legacy;
}}
''')
    prefix=prefix.replace('namespace rex::filesystem {std::filesystem::path GetExecutableFolder(){return {};}}',r'''
namespace rex::filesystem {std::filesystem::path folder;
 std::filesystem::path GetExecutableFolder(){return folder;}}
''')
    main_code=r'''
int main(int argc,char**argv){
 CHECK(argc==2);rex::filesystem::folder=argv[1];
 std::filesystem::create_directories(rex::filesystem::folder);
 {std::ofstream f(rex::filesystem::folder/"legodimensions.toml");f<<"anisotropic_override = 3\n";}
 CHECK(NativeAnisotropicRequested()==3); // plugin absent: import installed config
 rex::cvar::legacy="2";CHECK(NativeAnisotropicRequested()==2);
 for(int level=-1;level<=5;++level){
  rex::cvar::native=std::to_string(level);CHECK(NativeAnisotropicRequested()==level);
 }
 rex::cvar::native="-2";CHECK(NativeAnisotropicRequested()==2);
 rex::cvar::legacy="6";CHECK(NativeAnisotropicRequested()==-1);
 rex::cvar::legacy="invalid";CHECK(NativeAnisotropicRequested()==3);
 rex::cvar::native="5";rex::cvar::legacy="0";CHECK(NativeAnisotropicRequested()==5);
 std::array<TextureFetchWords,32> fetches{};
 for(unsigned i=0;i<32;++i){
  fetches[i]={2,4096,127|(127<<13),(1<<19)|(1<<21)|(1<<23),7<<6,(1<<9)|(1<<12)};
  fetches[i][0]|=(i%4)<<10;
  fetches[i][4]|=(i%4)<<12;
 }
 auto reset=[] {g_state=std::make_unique<State>();++g_state_generation;};
 reset();unsigned cases=0;
 std::vector<NativeSamplerKey> old_keys;
 std::mt19937 random(0x5752084B);
 for(unsigned draw=0;draw<3000;++draw){
  // Stable fetches with repeated filtering changes exposed the old bug.
  const int level=int(draw%7)-1;
  g_anisotropic_override.store(level);
  const u32 mask=draw%17==0?0:random();
  std::array<u32,32> ids;ids.fill(0xDEAD);
  HostDevice::RegisterSamplers(fetches,mask,ids);
  for(unsigned i=0;i<32;++i){
   if(!(mask&(1u<<i))){CHECK(ids[i]==0xDEAD);continue;}
   auto expected=DecodeTextureSampler(fetches[i]);
   CHECK(expected.maxLOD>expected.minLOD);
   CHECK(expected.minFilter==plume::RenderFilter::LINEAR&&expected.magFilter==plume::RenderFilter::LINEAR);
   if(level>=0){expected.anisotropyEnabled=level!=0;expected.maxAnisotropy=level?1u<<(level-1):1u;}
   CHECK(ids[i]>0&&ids[i]<=g_state->sampler_descriptors->keys.size());
   CHECK(g_state->sampler_descriptors->keys[ids[i]-1]==TextureSamplerKey(expected));++cases;
   CHECK(HostDevice::RegisterSampler(fetches[i])==ids[i]);
  }
  const auto& keys=g_state->sampler_descriptors->keys;
  CHECK(keys.size()>=old_keys.size());
  CHECK(std::equal(old_keys.begin(),old_keys.end(),keys.begin())); // in-flight IDs never overwritten
  old_keys=keys;
 }
 const auto count=g_state->device->created;
 CHECK(count>4&&count<256); // modes reuse canonical immutable descriptors
 reset();g_anisotropic_override.store(5);g_state->device->fail=true;
 std::array<u32,32> ids{};
 HostDevice::RegisterSamplers(fetches,1,ids);CHECK(ids[0]==0);
 HostDevice::RegisterSamplers(fetches,1,ids);CHECK(ids[0]==0);
 g_state->device->fail=false;HostDevice::RegisterSamplers(fetches,1,ids);CHECK(ids[0]>0);
 auto good=ids[0];g_anisotropic_override.store(0);g_state->device->fail=true;
 HostDevice::RegisterSamplers(fetches,1,ids);CHECK(ids[0]==0);
 g_state->device->fail=false;HostDevice::RegisterSamplers(fetches,1,ids);CHECK(ids[0]>0&&ids[0]!=good);
 g_anisotropic_override.store(5);HostDevice::RegisterSamplers(fetches,1,ids);CHECK(ids[0]==good);
 reset();g_anisotropic_override.store(3);
 HostDevice::RegisterSamplers(fetches,1,ids);CHECK(ids[0]==1&&g_state->device->created==1);
 std::printf("PASS %u sampler selections over 3000 setting changes; scalar/batch parity, immutable IDs, retry, revert and device reset\n",cases);
}
'''
    code=prefix+'\n'+request+'\n'+main_code
    def run(name,body,thread_cache,fetch_cache,no_batch=False):
        cpp=out/(name+'.cpp');exe=out/(name+'.exe');cpp.write_text(body)
        compiled=subprocess.run([args.compiler,'-std=c++20','-O2','-DNOMINMAX','-D_CRT_SECURE_NO_WARNINGS',
            '-I'+str(root/'rexlego/src'),'-I'+str(root/'thirdparty/plume'),str(cpp),'-o',str(exe)],
            capture_output=True,text=True,timeout=60)
        (out/(name+'-compile.log')).write_text(compiled.stdout+compiled.stderr);compiled.check_returncode()
        env=dict(os.environ,LEGO_NATIVE_SAMPLER_THREAD_CACHE=str(thread_cache),LEGO_NATIVE_SAMPLER_FETCH_CACHE=str(fetch_cache))
        env.pop('LEGO_NATIVE_NO_SAMPLER_BATCH',None)
        if no_batch: env['LEGO_NATIVE_NO_SAMPLER_BATCH']='1'
        result=subprocess.run([str(exe),str(out/(name+'-config'))],capture_output=True,text=True,timeout=20,env=env)
        (out/(name+'.log')).write_text(result.stdout+result.stderr)
        return result
    positive=[]
    for thread_cache,fetch_cache,no_batch in [(0,0,False),(0,1,False),(1,0,False),(1,1,False),(0,0,True),(1,1,True)]:
        name=f'live-t{thread_cache}-f{fetch_cache}-scalar{int(no_batch)}'
        result=run(name,code,thread_cache,fetch_cache,no_batch)
        assert result.returncode==0,(name,result.stdout,result.stderr)
        positive.append(dict(name=name,exit=result.returncode,output=result.stdout));print(name,result.stdout.strip(),flush=True)
    negative=[]
    for name,old,new,t,f in [
        ('stale-thread-filter','entry.anisotropic_override == anisotropic_override','true',1,0),
        ('stale-fetch-filter','state.sampler_last_overrides[i]==anisotropic_override','true',0,1),
        ('ignore-native-selection','if (selected >= -1 && selected <= 5) return selected;','if (false) return selected;',0,0)]:
        assert code.count(old)==1,(name,code.count(old))
        result=run(name,code.replace(old,new),t,f)
        assert result.returncode==23,(name,result.returncode,result.stdout,result.stderr)
        negative.append(dict(name=name,exit=result.returncode,error=result.stderr))
    (out/'verification.json').write_text(json.dumps(dict(game_launched=False,window_opened=False,
        passed=True,source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),positive=positive,negative=negative,
        scope='Actual request resolver, sampler decoder, immutable registration and scalar/batch/thread/fetch caches. Registry and GPU descriptor allocation mocked; does not prove native frontend integration, GPU visual filtering or FPS.'),indent=2))
if __name__=='__main__': main()
