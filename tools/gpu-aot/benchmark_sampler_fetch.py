"""Bounded CPU-only comparison of the production sampler batch paths.

Uses the compiled-function fixture's mocked device/descriptors and real mutex.
Synthetic repetition/change rates are bounds, not an observed game workload.
"""
import argparse,hashlib,json,os,re,statistics,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('fixture',type=Path)
p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++');a=p.parse_args()
root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
prefix=a.fixture.read_text().split('int main(){',1)[0]
assert 'state.sampler_last_fetches[i]==fetches[i]' in prefix
code=prefix+r'''
int main(){
 std::array<TextureFetchWords,32> fetches{};std::array<u32,32> ids{};
 for(u32 i=0;i<32;++i){fetches[i][0]=(i%8)<<10;fetches[i][1]=4096;fetches[i][2]=63|(63<<13);
  fetches[i][3]=((i/8)%2)<<19;fetches[i][5]=(1<<9)|(1<<12);}
 for(unsigned workload=0;workload<3;++workload)for(unsigned slots:{4u,8u,32u})for(unsigned trial=0;trial<6;++trial){
  g_state=std::make_unique<State>();const u32 mask=slots==32?~u32{0}:((1u<<slots)-1);
  HostDevice::RegisterSamplers(fetches,mask,ids);g_mutex.locks=0;u64 checksum=0;
  const auto start=std::chrono::steady_clock::now();
  for(u32 draw=0;draw<100000;++draw){
   // Raw base-address bits do not alter the canonical sampler key. They still
   // must invalidate a full-fetch cache entry, exercising conservative misses.
   if(workload==1 || (workload==2 && draw%16==0))
    for(unsigned i=0;i<slots;++i)fetches[i][1]^=8192;
   HostDevice::RegisterSamplers(fetches,mask,ids);checksum+=ids[draw%slots];
  }
  const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  CHECK(g_mutex.locks==100000);
  std::printf("workload=%u slots=%u trial=%u ms=%.6f checksum=%llu\n",workload,slots,trial,ms,(unsigned long long)checksum);
 }
}
'''
cpp=out/'benchmark.cpp';exe=out/'benchmark.exe';cpp.write_text(code)
r=subprocess.run([a.compiler,'-std=c++20','-O2','-DNOMINMAX','-D_CRT_SECURE_NO_WARNINGS',
    '-I'+str(root/'rexlego/src'),'-I'+str(root/'thirdparty/plume'),str(cpp),'-o',str(exe)],
    capture_output=True,text=True,timeout=45)
(out/'compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
rows=[]
for run,mode in enumerate(('0','1','1','0')):
    env=dict(os.environ,LEGO_NATIVE_SAMPLER_FETCH_CACHE=mode);env.pop('LEGO_NATIVE_NO_SAMPLER_BATCH',None)
    r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30,env=env)
    (out/f'run-{run}-cache-{mode}.log').write_text(r.stdout+r.stderr);r.check_returncode()
    records=re.findall(r'workload=(\d+) slots=(\d+) trial=(\d+) ms=([\d.]+) checksum=(\d+)',r.stdout)
    assert len(records)==54,len(records)
    for work,slots,trial,ms,checksum in records:
        rows.append(dict(run=run,cache=int(mode),workload=int(work),slots=int(slots),trial=int(trial),ms=float(ms),checksum=int(checksum)))
summaries=[]
for workload,name in enumerate(('stable','all_raw_fetches_change','raw_fetches_change_every_16_draws')):
    for slots in (4,8,32):
        chosen=[r for r in rows if r['workload']==workload and r['slots']==slots]
        assert len({r['checksum'] for r in chosen})==1
        median={mode:statistics.median(r['ms'] for r in chosen if r['cache']==mode) for mode in (0,1)}
        item=dict(workload=name,slots=slots,trials_per_mode=12,draws_per_trial=100000,
            baseline_median_ms=median[0],cache_median_ms=median[1],cache_over_baseline=median[1]/median[0])
        summaries.append(item);print(json.dumps(item))
(out/'verification.json').write_text(json.dumps(dict(game_launched=False,fixture_sha256=hashlib.sha256(a.fixture.read_bytes()).hexdigest(),
    benchmark_source_sha256=hashlib.sha256(code.encode()).hexdigest(),summary=summaries,rows=rows,
    scope='production sampler registration/decoder/batch copied by correctness fixture; mock sampler creation/table, real mutex with lock counter; synthetic hit/miss bounds and input updates; not current game CPU time/hit rate/FPS'),indent=2))
