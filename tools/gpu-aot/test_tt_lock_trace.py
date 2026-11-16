"""Exercise production timing state and raw-hook call ordering/register behavior."""
import argparse,json,os,subprocess
from pathlib import Path
def block(text,signature):
 start=text.index(signature);end=text.index('{',start)+1;depth=1
 while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++');a=p.parse_args()
 root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
 text=(root/'rexlego/src/tt_lock_trace.cpp').read_text()
 hooks='\n'.join(block(text,'REX_HOOK_RAW('+name+')') for name in ('sub_82A2A260','sub_82A2A2B0','sub_82B73D48'))
 assert 'renderer_route.h' not in text
 cmake=(root/'rexlego/CMakeLists.txt').read_text()
 assert 'option(LEGODIMENSIONS_TT_LOCK_TRACE "Build TT shared-render-lock timing hooks" OFF)' in cmake
 code=r'''
#include "tt_lock_trace_state.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>
using namespace legodimensions::tt_lock_trace;
using u8=unsigned char;
struct PPCContext{struct{uint32_t u32=0;}r3;uint64_t lr=0,scratch=0;};
#define REX_HOOK_RAW(n) void n(PPCContext&ctx,u8*base)
#undef assert
#define assert(c) do{if(!(c)){std::fprintf(stderr,"failed line %d: %s\n",__LINE__,#c);std::exit(23);}}while(0)
bool guest_locked=false,throw_acquire=false,enabled=true;
unsigned calls=0,clock_reads=0;uint64_t tick=0;LockState hook_state;std::vector<LockRow>rows;
namespace legodimensions::tt_lock_trace {
bool Enabled(){return enabled;}
ClockPoint SampleClock(){++clock_reads;tick+=10;return {tick,tick/2,true,true};}
void Acquired(ClockPoint a,ClockPoint b){assert(guest_locked);hook_state.Acquired(a,b);}
void Released(ClockPoint end){if(auto row=hook_state.Released(end)){assert(!guest_locked);rows.push_back(*row);}}
struct RenderScope{
 SpanToken token=hook_state.Begin(SpanKind::kRender,++tick,true);
 ~RenderScope(){hook_state.End(token,++tick,true);}
};
}
void __imp__sub_82A2A260(PPCContext&c,u8*){++calls;if(throw_acquire)throw std::runtime_error("original");guest_locked=true;c.r3.u32=1;c.lr=77;c.scratch+=19;}
void __imp__sub_82A2A2B0(PPCContext&c,u8*){++calls;guest_locked=false;c.r3.u32=91;c.lr=99;c.scratch+=23;}
void __imp__sub_82B73D48(PPCContext&c,u8*){++calls;assert(guest_locked);c.r3.u32=7;c.scratch+=13;}
'''+hooks+r'''
int main(){
 // Outer wait/hold CPU points remain exact across a recursive acquisition.
 LockState state;state.Acquired({100,10,true,true},{140,12,true,true});
 auto render=state.Begin(SpanKind::kRender,150,true);
 auto first=state.Begin(SpanKind::kNativeDraw,160,true);
 auto nested=state.Begin(SpanKind::kNativeDraw,165,true);state.End(nested,170,true);
 state.End(first,190,true);state.End(render,200,true);
 state.Acquired({201,15,true,true},{202,16,true,true});assert(!state.Released({210,18,true,true}));
 auto row=state.Released({240,20,true,true});assert(row&&WallValid(*row)&&CpuValid(*row));
 assert(row->waiting.wall==100&&row->acquired.wall==140&&row->released.wall==240);
 assert(row->draws==2&&row->draw_ticks==30&&row->renders==1&&row->render_ticks==50&&row->scopes_closed);
 assert(!state.Held());auto orphan=state.Released({300,30,true,true});assert(orphan&&orphan->orphan&&!WallValid(*orphan)&&!CpuValid(*orphan));
 // Missing/nonmonotonic clocks are explicitly invalid, never claimed as0ms CPU.
 state.Acquired({100,10,true,false},{140,12,true,true});row=state.Released({240,20,true,true});assert(WallValid(*row)&&!CpuValid(*row));
 state.Acquired({100,10,true,true},{90,12,true,true});row=state.Released({240,20,true,true});assert(!WallValid(*row)&&CpuValid(*row));
 state.Acquired({100,10,true,true},{140,12,true,true});row=state.Released({240,11,true,true});assert(!CpuValid(*row));
 // Crossing unlock, bad clocks and overflow must not corrupt later generations.
 state.Acquired({1,1,true,true},{2,2,true,true});auto stale=state.Begin(SpanKind::kNativeDraw,3,true);
 row=state.Released({4,4,true,true});assert(!row->scopes_closed);
 state.Acquired({5,5,true,true},{6,6,true,true});state.End(stale,7,true);row=state.Released({8,8,true,true});assert(row->draw_ticks==0&&row->scopes_closed);
 state.Acquired({10,10,true,true},{11,11,true,true});auto bad=state.Begin(SpanKind::kNativeDraw,20,true);state.End(bad,19,true);
 row=state.Released({30,30,true,true});assert(!row->draw_valid&&row->draw_ticks==0);
 state.Acquired({1,1,true,true},{2,2,true,true});auto large=state.Begin(SpanKind::kNativeDraw,0,true);state.End(large,UINT64_MAX,true);
 auto extra=state.Begin(SpanKind::kNativeDraw,1,true);state.End(extra,2,true);row=state.Released({3,3,true,true});assert(!row->draw_valid);
 assert(!state.Begin(SpanKind::kRender,9,true).active);
 // The actual raw wrappers preserve the original calls/register side effects.
 PPCContext ctx;ctx.r3.u32=kGraphicsLock;sub_82A2A260(ctx,nullptr);
 assert(ctx.r3.u32==1&&ctx.lr==77&&ctx.scratch==19&&calls==1&&clock_reads==2);
 sub_82B73D48(ctx,nullptr);assert(ctx.r3.u32==7&&ctx.scratch==32&&calls==2);
 ctx.r3.u32=kGraphicsLock;sub_82A2A2B0(ctx,nullptr);
 assert(ctx.r3.u32==91&&ctx.lr==99&&ctx.scratch==55&&calls==3&&rows.size()==1&&rows[0].renders==1);
 for(bool disabled:{false,true}){enabled=!disabled;ctx.r3.u32=disabled?kGraphicsLock:123;
  auto clocks=clock_reads;auto count=rows.size();sub_82A2A260(ctx,nullptr);
  ctx.r3.u32=disabled?kGraphicsLock:123;sub_82A2A2B0(ctx,nullptr);
  assert(clock_reads==clocks&&rows.size()==count);}
 enabled=true;throw_acquire=true;ctx.r3.u32=kGraphicsLock;
 try{sub_82A2A260(ctx,nullptr);assert(false);}catch(const std::runtime_error&){}
 assert(!hook_state.Held());
 std::puts("PASS timing/recursion/clock validity/span generations, original hook calls/registers, disabled paths and output after unlock");
}
'''
 def run(name,body):
  cpp=out/(name+'.cpp');exe=out/(name+'.exe');cpp.write_text(body)
  subprocess.run([a.compiler,'-std=c++20','-O2','-UNDEBUG','-I'+str(root/'rexlego/src'),str(cpp),'-o',str(exe)],check=True,timeout=45)
  return subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
 positive=run('current',code);assert positive.returncode==0,positive.stderr
 # Writing before the original unlock is observable even when clocks are valid.
 old='  __imp__sub_82A2A2B0(ctx,base);\n  if(trace)Released(end);'
 assert old in code
 negative=run('write-before-unlock',code.replace(old,'  if(trace)Released(end);\n  __imp__sub_82A2A2B0(ctx,base);'))
 assert negative.returncode==23,negative.stderr
 (out/'verification.json').write_text(json.dumps(dict(game_launched=False,result='PASS',actual_hook_bodies=True,
  default_build_option_off=True,pm4_bypass_absent=True,negative_exit=negative.returncode,
  scope='production portable state and raw hook bodies; fake clock/original guest calls, not live Windows timing or game performance'),indent=2))
 print(positive.stdout.strip())
if __name__=='__main__':main()
