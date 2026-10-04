"""Exercise the report CLI with controlled frame budgets and diagnostic records."""
import argparse
import csv
import json
from pathlib import Path
import subprocess
import sys

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
script=Path(__file__).with_name('summarize_frame_metrics.py')
path=a.output/'fixture-frames.csv'
fields=['frame','interval_ms','texture_ms','constants_ms','vertices_ms',
        'buffer_hash_ms','present_cpu_ms','draw_calls','frame_slot_wait_ms',
        'acquire_cpu_ms','present_submit_cpu_ms','swap_present_cpu_ms',
        'buffer_watch_hits','buffer_watch_audits','buffer_watch_mismatches']
with path.open('w',newline='') as f:
 w=csv.DictWriter(f,fieldnames=fields);w.writeheader()
 for n in range(1,101):
  w.writerow(dict.fromkeys(fields,0)|{'frame':n,'interval_ms':n,'draw_calls':505,
      'frame_slot_wait_ms':2,'acquire_cpu_ms':3,'present_submit_cpu_ms':4,
      'swap_present_cpu_ms':5,'buffer_watch_audits':6})
 for interval in ('nan','inf',0,-1):
  w.writerow(dict.fromkeys(fields,0)|{'frame':101,'interval_ms':interval})
 f.write('102,1000,')
record=path.with_name('fixture-process.json')
def report(*args):
 return json.loads(subprocess.check_output([sys.executable,str(script),str(path),*args],text=True))
record.write_text(json.dumps({'mode':'timing','auditBufferWatch':True}))
r=report('--tail-seconds','0.1')
s=r['whole_run']
assert s['frames']==100 and s['seconds']==5.05 and s['mean_fps']==1000/50.5
assert (s['frame_ms_p50'],s['frame_ms_p95'],s['frame_ms_p99'],s['frame_ms_max'])==(50,95,99,100)
assert s['over_16_67ms_percent']==84 and s['over_33_33ms_percent']==67
assert s['frame_slot_wait_ms_mean']==2 and s['swap_present_cpu_ms_mean']==5
assert s['buffer_watch_audits_mean']==6 and s['buffer_watch_mismatches_mean']==0
assert r['tail']['frames']==1 and not r['clean_performance_probe']
record.write_text(json.dumps({'mode':'timing','portraitTrace':True}))
assert not report()['clean_performance_probe']
record.write_text(json.dumps({'mode':'timing'}))
assert report()['clean_performance_probe']
r=report('--after-frame','90','--through-frame','95')
assert r['whole_run']['frames']==5 and r['latest_frame']==95
assert report('--after-frame','100')['whole_run']=={}
record.unlink()
assert report()['clean_performance_probe'] is None
print('PASS: CLI frame budgets, percentile tails, malformed/partial rows, window selection, audit exclusion')
