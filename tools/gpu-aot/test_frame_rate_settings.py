"""Execute the production frame limiter's live CVar read without the title."""
import argparse,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
p.add_argument('--compiler',default='clang++');p.add_argument('--expect-old-failure',action='store_true')
a=p.parse_args();root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
source=(root/'rexlego/src/gpu_native/device.cpp').read_text()
start=source.index('void LimitFrameRate()');end=source.index('{',start)+1;depth=1
while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
body=source[start:end]
prefix=r'''
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
using int32_t=std::int32_t;
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
#define REXLOG_INFO(...) ((void)0)
std::mutex registry;int32_t cap=1000;
std::atomic<unsigned> raw_reads=0,locked_reads=0;
int32_t RawRead(){++raw_reads;return cap;}
#define REXCVAR_GET(n) RawRead()
namespace rex::cvar {
template<class T>T Query(const char*);
template<>int32_t Query<int32_t>(const char*){std::lock_guard lock(registry);++locked_reads;return cap;}
}
void Set(int32_t value){std::lock_guard lock(registry);cap=value;}
'''
suffix=r'''
int main(int argc,char**argv){
 CHECK(argc==2);const bool forced=std::atoi(argv[1])!=0;
 LimitFrameRate();Set(0);LimitFrameRate();LimitFrameRate();
 CHECK(raw_reads==0&&locked_reads==(forced?0:3));
 if(!forced){
  std::atomic<bool>running=true;std::atomic<unsigned>writes=0;
  std::thread ui([&]{while(running.load()){Set(writes.fetch_add(1)%2?0:1000);std::this_thread::yield();}});
  while(!writes.load())std::this_thread::yield();
  for(unsigned frame=0;frame<200;++frame)LimitFrameRate();
  running=false;ui.join();CHECK(locked_reads==203&&raw_reads==0);
 }
 std::printf("PASS live frame-limit reads guarded=%u raw=%u forced_zero=%u; real clock/timer, no FPS benchmark\n",locked_reads.load(),raw_reads.load(),forced);
}
'''
def build(name,text):
    cpp=out/(name+'.cpp');exe=out/(name+'.exe');cpp.write_text(prefix+text+suffix)
    r=subprocess.run([a.compiler,'-std=c++20','-O2','-D_CRT_SECURE_NO_WARNINGS',str(cpp),'-o',str(exe)],
        capture_output=True,text=True,timeout=45)
    (out/(name+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode();return exe
def run(exe,forced):
    env=dict(os.environ);env.pop('LEGO_NATIVE_FPS_LIMIT',None)
    if forced:env['LEGO_NATIVE_FPS_LIMIT']='0'
    r=subprocess.run([str(exe),str(int(forced))],capture_output=True,text=True,timeout=20,env=env)
    (out/(exe.stem+'-'+str(int(forced))+'.log')).write_text(r.stdout+r.stderr);return r
exe=build('current',body);results=[]
for forced in (False,True):
    r=run(exe,forced);expected=23 if a.expect_old_failure and not forced else 0
    assert r.returncode==expected,(r.stdout,r.stderr)
    results.append(dict(forced_zero=forced,exit=r.returncode,stdout=r.stdout,stderr=r.stderr))
negative=[]
if not a.expect_old_failure:
    old='rex::cvar::Query<int32_t>("native_fps_limit")';assert body.count(old)==1
    bad=build('raw-read',body.replace(old,'REXCVAR_GET(native_fps_limit)'))
    r=run(bad,False);assert r.returncode==23,r.stdout+r.stderr
    negative.append(dict(name='raw-read',exit=r.returncode,stderr=r.stderr))
(out/'verification.json').write_text(json.dumps(dict(game_launched=False,before_fix=a.expect_old_failure,
    positive=results,negative=negative,
    scope='actual LimitFrameRate, synchronized mock registry, real host writer/clock/timer; no game or FPS/cap accuracy benchmark'),indent=2))
print('REPRODUCED raw frame-limit read' if a.expect_old_failure else 'PASS synchronized live cap, override bypass, concurrent writer and raw-read control')
