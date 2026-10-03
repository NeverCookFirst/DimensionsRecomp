"""Check opt-in tracing across translation units and concurrent producers."""
import argparse
import csv
import json
import os
from pathlib import Path
import subprocess

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('output',type=Path)
args=parser.parse_args()
root=Path(__file__).resolve().parents[2]
args.output.mkdir(parents=True,exist_ok=True)
header=(root/'rexlego/src/gpu_native/long_probe.h').as_posix()
a=args.output/'producer.cpp'
b=args.output/'consumer.cpp'
a.write_text(f'#include "{header}"\nvoid Produce() {{ legodimensions::gpu_native::LongProbeEvent("producer",false,42); }}\n')
b.write_text(f'#include "{header}"\n'+r'''
#include <thread>
#include <cassert>
void Produce();
int main() {
  using namespace legodimensions::gpu_native;
  if(!LongProbeEnabled()) { LongProbeEvent("disabled",true,1); assert(!g_probe_capture_requested.load()); return 0; }
  g_probe_frame.store(123);
  std::thread first([] { for(int i=0;i<100;i++) Produce(); });
  std::thread second([] { for(int i=0;i<100;i++) LongProbeEvent("consumer",false,i); });
  first.join(); second.join(); LongProbeEvent("candidate",true,3);
  assert(g_probe_capture_requested.exchange(false));
  const uint8_t bytes[]{1,2,3,4};
  const auto name=SaveLongProbeConstants(bytes);
  LongProbeEvent("frame",false,"file=",name);
  for(int i=0;i<130;i++) SaveLongProbeConstants(bytes);
  g_probe_frame.store(124);
  assert(!SaveLongProbeConstants(bytes).empty());
  LongProbeEvent("frame",false,"budget_reset=1");
}
''')
exe=args.output/'trace-test.exe'
subprocess.run(['clang++','-std=c++20',str(a),str(b),'-o',str(exe)],check=True)
env=dict(os.environ); env.pop('LEGO_NATIVE_TRACE_DIR',None)
subprocess.run([str(exe.resolve())],env=env,check=True)
destination=args.output/'enabled'
assert not destination.exists(), 'Use a fresh output to preserve evidence'
env['LEGO_NATIVE_TRACE_DIR']=str(destination.resolve())
subprocess.run([str(exe.resolve())],env=env,check=True)
with (destination/'events.tsv').open() as file: rows=list(csv.DictReader(file,delimiter='\t'))
assert len(rows)==204
assert sum(r['event']=='producer' for r in rows)==100
assert sum(r['event']=='consumer' for r in rows)==100
assert sum(r['anomaly_candidate']=='1' for r in rows)==1
assert sum(r['frame']=='123' for r in rows)==203
assert sum(r['event']=='constants_frame_budget' for r in rows)==1
assert len(list(destination.glob('constants-f123-*.bin')))==128
assert len(list(destination.glob('constants-f124-*.bin')))==1
assert next(destination.glob('constants-*.bin')).read_bytes()==bytes([1,2,3,4])
assert json.loads((destination/'trace-start.json').read_text())['unix_ms']>0
print('Passed: disabled mode, shared writer across TUs, 200 concurrent events, anomaly request, binary payload, 128-file frame budget and reset')
