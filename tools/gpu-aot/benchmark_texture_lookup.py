"""Bounded offline cost comparison of production use-time header lookup paths.

Consumes the compiled-function correctness fixture, including its small header
and registry model. No GPU, guest-memory translation, contention or FPS claim.
"""
import argparse,hashlib,json,os,statistics,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('fixture',type=Path)
p.add_argument('output',type=Path)
p.add_argument('--compiler',default='clang++')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
code=a.fixture.read_text()
assert 'RefreshTextureResourceHeader(guest_address,existing)' in code
old='static int LockRecording(){return 0;}'
new='static std::unique_lock<std::recursive_mutex> LockRecording(){static std::recursive_mutex mutex;return std::unique_lock(mutex);}'
assert code.count(old)==1
code=code.replace(old,new).replace('int main(){','int correctness_main(){',1)
closing=code.rfind('}')
code=code[:closing]+' return 0;\n'+code[closing:]
code+='''
#include <chrono>
int main(){
 header.format.dword={2,0xF4000052u,0x001FE0FFu,0xD10,0x200,0xF4100A00u};
 for(u32 address=1;address<=1024;++address){auto r=std::make_shared<TextureResource>();
  r->guest_address=address;r->guest_fetch=header.format.dword;g_textures[address]=r;}
 auto use=[](u32 i){auto recording=HostDevice::LockRecording();auto r=FindTextureAtUse((i&1023)+1);
  CHECK(r && r->guest_fetch==header.format.dword);return r->descriptor_index;};
 for(u32 i=0;i<10000;++i)use(i);
 registry_lookups=header_reads=0;uint64_t checksum=0;constexpr u32 iterations=500000;
 auto begin=std::chrono::steady_clock::now();
 for(u32 i=0;i<iterations;++i)checksum+=use(i);
 auto end=std::chrono::steady_clock::now();
 const char* setting=std::getenv("LEGO_NATIVE_REUSE_TEXTURE_LOOKUP");
 bool legacy=setting && std::strcmp(setting,"0")==0;
 CHECK(registry_lookups==iterations*(legacy?3:1)&&header_reads==iterations);
 double ns=std::chrono::duration<double,std::nano>(end-begin).count()/iterations;
 std::printf("{\\"ns_per_use\\":%.6f,\\"iterations\\":%u,\\"lookups\\":%u,\\"header_reads\\":%u,\\"checksum\\":%llu}\\n",
  ns,iterations,registry_lookups,header_reads,(unsigned long long)checksum);
}
'''
cpp=a.output/'benchmark.cpp';cpp.write_text(code);exe=a.output/'benchmark.exe'
subprocess.run([a.compiler,'-std=c++20','-O2','-D_CRT_SECURE_NO_WARNINGS',str(cpp),'-o',str(exe)],check=True,timeout=45)
samples=[]
for pair in range(8):
    for setting in (('0','1') if pair%2==0 else ('1','0')):
        result=subprocess.run([str(exe.resolve())],capture_output=True,text=True,check=True,timeout=15,
            env=dict(os.environ,LEGO_NATIVE_REUSE_TEXTURE_LOOKUP=setting))
        sample=json.loads(result.stdout);sample.update(pair=pair,reuse=setting=='1');samples.append(sample)
baseline=statistics.median(s['ns_per_use'] for s in samples if not s['reuse'])
optimized=statistics.median(s['ns_per_use'] for s in samples if s['reuse'])
report=dict(game_launched=False,fixture_sha256=hashlib.sha256(a.fixture.read_bytes()).hexdigest(),
    registry_entries=1024,samples=samples,median_legacy_ns=baseline,median_optimized_ns=optimized,
    median_reduction_percent=100*(1-optimized/baseline),
    scope='production header comparison/refresh and registry lookup shape; fixture header/memory/registry, real std mutex/refcounts, recursive recording lock; no GPU/content-upload/contention or game FPS claim')
(a.output/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k!='samples'},indent=2))
