"""Exercise actual opt-in constant phase accounting with a controlled clock."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='clang++')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
source = (root / 'rexlego/src/gpu_native/draw.cpp').read_text()
start = source.index('struct DrawTimer {')
end = source.index('\nbool MapTopology(', start)
timer = source[start:end].replace('std::chrono::steady_clock', 'FakeClock')
cpp = a.output / 'constant-timing.cpp'
cpp.write_text(r'''
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>
#include "gpu_native/draw.h"
using namespace legodimensions::gpu_native;
using u32=uint32_t;
bool enabled=false;
bool NativeTextureTimingEnabled(){return enabled;}
DrawTiming g_draw_timing;
#define REXLOG_INFO(...) ((void)0)
struct FakeClock {
 using time_point=std::chrono::steady_clock::time_point;
 static inline int milliseconds=0,calls=0;
 static time_point now(){++calls;return time_point{std::chrono::milliseconds(milliseconds)};}
};
''' + timer + r'''
int main(){
 // A cadence-only probe still counts attempted draws without any draw clocks.
 {DrawTimer t;t.Next();t.Next();}
 assert(g_draw_timing.calls==1&&FakeClock::calls==0);
 for(double v:g_draw_timing.stages_ms)assert(v==0);
 enabled=true;
 {
  DrawTimer t;
  for(int i=1;i<=6;++i){FakeClock::milliseconds=i;t.Next();}
  FakeClock::milliseconds=7;
 }
 assert(g_draw_timing.calls==2);
 for(double v:g_draw_timing.stages_ms)assert(v==1);
 auto draw_frame=std::exchange(g_draw_timing,{});
 assert(draw_frame.calls==2&&g_draw_timing.calls==0);
 enabled=false;FakeClock::calls=0;FakeClock::milliseconds=0;
 // Disabled instrumentation must not sample the clock or change totals.
 {ConstantTimer t(ConstantCost::kUpload);t.Next(ConstantCost::kShadow);}
 assert(FakeClock::calls==0);
 for(double v:g_draw_timing.constants_ms)assert(v==0);
 enabled=true;
 {
  ConstantTimer t(ConstantCost::kUpload);
  FakeClock::milliseconds=3;t.Next(ConstantCost::kShadow);
  FakeClock::milliseconds=7;t.Next(ConstantCost::kTextures);
  FakeClock::milliseconds=10;t.Next(ConstantCost::kShadow);
  FakeClock::milliseconds=12;t.Next(ConstantCost::kUpload);
  FakeClock::milliseconds=17;t.Next(ConstantCost::kBindings);
  FakeClock::milliseconds=23;
 }
 assert((g_draw_timing.constants_ms==std::array<double,5>{6,8,6,3,0}));
 // Outer descriptor and framebuffer scopes accumulate into the same frame.
 {ConstantTimer t(ConstantCost::kRestore);FakeClock::milliseconds=24;}
 {ConstantTimer t(ConstantCost::kBindings);FakeClock::milliseconds=29;}
 assert((g_draw_timing.constants_ms==std::array<double,5>{6,8,11,3,1}));
 double total=0;for(double v:g_draw_timing.constants_ms)total+=v;
 assert(total==29); // Disjoint ranges account for the entire controlled interval.
 // Early exit accounts for the active phase without inventing later work.
 {ConstantTimer t(ConstantCost::kUpload);FakeClock::milliseconds=36;}
 assert((g_draw_timing.constants_ms==std::array<double,5>{6,15,11,3,1}));
 auto completed=std::exchange(g_draw_timing,{});
 for(double v:g_draw_timing.constants_ms)assert(v==0);
 assert(completed.constants_ms[1]==15);
 enabled=false;const int calls=FakeClock::calls;
 for(int i=0;i<100;++i){ConstantTimer t(ConstantCost::kBindings);t.Next(ConstantCost::kRestore);}
 assert(FakeClock::calls==calls);
 for(double v:g_draw_timing.constants_ms)assert(v==0);
}
''')
exe = a.output / 'constant-timing.exe'
subprocess.run([a.compiler, '-std=c++20', '-O2', '-I'+str(root/'rexlego/src'),
                str(cpp), '-o', str(exe)], check=True, timeout=45)
subprocess.run([str(exe.resolve())], check=True, timeout=10)
print('PASS: production constant timing counts disjoint phases, early exits, reset and disabled clock')
