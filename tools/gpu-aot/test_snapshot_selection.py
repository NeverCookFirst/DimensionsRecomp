"""Replay the production snapshot selection with synthetic frame/clock state."""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser();p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++');a=p.parse_args()
root=Path(__file__).resolve().parents[2]
s=(root/'rexlego/src/gpu_native/device.cpp').read_text()
start=s.index('  static const bool manual_only =',s.index('void HostDevice::SnapshotTexture('))
end=s.index('  // A busy character pass',start)
code=r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <string_view>
bool probe=true;unsigned events=0;
bool LongProbeEnabled(){return probe;}
template<class... T>void LongProbeEvent(T...){++events;}
std::atomic<bool>g_probe_capture_requested{false};
struct State {
 unsigned present_number=100,snapshot_selected_frame=~0u,probe_present_burst_remaining=0,snapshot_time_window=0;
 std::chrono::steady_clock::time_point probe_last_anomaly_snapshot{},probe_next_snapshot{},probe_next_present{},snapshot_start_time{};
};
bool Select(State& state,std::string_view label,bool time_windows=false) {
 auto now=std::chrono::steady_clock::now();unsigned frame=100;
''' + s[start:end].replace('return;', 'return false;') + r'''
 return true;
}
void Manual(bool enabled){
#ifdef _WIN32
 _putenv_s("LEGO_GPU_SNAPSHOT_MANUAL_ONLY",enabled?"1":"");
#else
 if(enabled)setenv("LEGO_GPU_SNAPSHOT_MANUAL_ONLY","1",1);else unsetenv("LEGO_GPU_SNAPSHOT_MANUAL_ONLY");
#endif
}
int main(int argc,char**){
 Manual(argc>1); // Startup-only environment policies get a fresh process below.
 if(argc>1){
 for(bool trace:{false,true}) {
  probe=trace;State state;events=0;g_probe_capture_requested=true;
  // An overdue automatic capture and a flagged anomaly cannot override manual mode.
  assert(!Select(state,"present"));assert(!Select(state,"draw-0"));assert(events==0);
  assert(state.snapshot_selected_frame==~0u);assert(g_probe_capture_requested);
  state.snapshot_selected_frame=100;assert(Select(state,"present"));assert(Select(state,"resolve"));
  ++state.present_number;assert(!Select(state,"present"));
  state.probe_present_burst_remaining=2;
  assert(!Select(state,"resolve"));assert(state.probe_present_burst_remaining==2);
  assert(Select(state,"present"));++state.present_number;
  assert(Select(state,"present"));assert(!Select(state,"present"));assert(state.probe_present_burst_remaining==0);
 }
 }else{
 probe=true;State state;events=0;
 assert(Select(state,"present"));assert(state.snapshot_selected_frame==101);assert(events>0);
 probe=false;State fixed;assert(!Select(fixed,"present"));
 }
}
'''
a.output.mkdir(parents=True,exist_ok=True);cpp=a.output/'snapshot-selection.cpp';exe=a.output/'snapshot-selection.exe';cpp.write_text(code)
subprocess.run([a.compiler,'-std=c++20','-UNDEBUG',str(cpp),'-o',str(exe)],check=True,timeout=45)
subprocess.run([str(exe.resolve())],check=True,timeout=10)
subprocess.run([str(exe.resolve()),'manual'],check=True,timeout=10)
print('PASS: manual+trace precedence, explicit whole frame, bounded present burst, automatic and fixed policies')
