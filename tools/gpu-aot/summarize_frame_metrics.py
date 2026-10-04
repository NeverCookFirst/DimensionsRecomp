"""Summarize every-present timings; snapshots are unnecessary for this probe."""
import argparse
import csv
import io
import json
import math
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__);p.add_argument('file',type=Path)
p.add_argument('--tail-seconds',type=float,default=30)
p.add_argument('--after-frame',type=int,default=0)
p.add_argument('--through-frame',type=int)
a=p.parse_args()
if not math.isfinite(a.tail_seconds) or a.tail_seconds <= 0:
 p.error('--tail-seconds must be finite and positive')
text=a.file.read_text();text=text[:text.rfind('\n')+1] # Exclude a partial live row.
rows=[]
for row in csv.DictReader(io.StringIO(text)):
 try:
  parsed={k:float(v) for k,v in row.items()}
  if (parsed.get('interval_ms',0)>0 and 'frame' in parsed and
      all(math.isfinite(v) for v in parsed.values())): rows.append(parsed)
 except (TypeError,ValueError):continue
rows=[r for r in rows if r['frame']>a.after_frame and
      (a.through_frame is None or r['frame']<=a.through_frame)]
tail=[];elapsed=0
for row in reversed(rows):
 tail.append(row);elapsed+=row['interval_ms']/1000
 if elapsed>=a.tail_seconds:break
def stats(data):
 if not data:return {}
 times=sorted(r['interval_ms'] for r in data)
 mean=lambda key:sum(r[key] for r in data)/len(data)
 result={'frames':len(data),'seconds':sum(times)/1000,'mean_fps':1000/mean('interval_ms'),
 'frame_ms_p50':times[(len(times)-1)//2],'frame_ms_p95':times[int((len(times)-1)*.95)],
 'frame_ms_p99':times[int((len(times)-1)*.99)],
 'frame_ms_max':max(times),'over_33_5ms_percent':100*sum(t>33.5 for t in times)/len(times),
 'over_16_67ms_percent':100*sum(t>1000/60 for t in times)/len(times),
 'over_33_33ms_percent':100*sum(t>1000/30 for t in times)/len(times),
 'texture_ms_mean':mean('texture_ms'),'constants_ms_mean':mean('constants_ms'),
 'vertices_ms_mean':mean('vertices_ms'),'buffer_hash_ms_mean':mean('buffer_hash_ms'),
 'present_cpu_ms_mean':mean('present_cpu_ms'),'draws_mean':mean('draw_calls')}
 for key in ['cpu_resource_wait_skips','bindings_ms','begin_ms','pipeline_ms','issue_ms',
             'tail_ms','buffer_converted_bytes','frame_slot_wait_calls','frame_slot_wait_ms',
             'acquire_cpu_ms','present_submit_cpu_ms','swap_present_cpu_ms',
             'buffer_watch_hits','buffer_watch_audits','buffer_watch_mismatches'] + [k for k in data[0] if k.startswith(('sync_', 'callbacks_'))]:
  if key in data[0]:result[key+'_mean']=mean(key)
 return result
record_path=a.file.with_name(a.file.name.replace('-frames.csv','-process.json'))
record=json.loads(record_path.read_text(encoding='utf-8-sig')) if record_path.exists() else None
heavy_capture=bool(record and (record.get('mode')=='snapshot' or any(record.get(k) for k in
 ['longProbe','logoUploads','logoCaptureRenderDocDll','captureMissing','meshTrace','auditTextureWatch','auditBufferWatch','portraitTrace'])))
report={'source':str(a.file),'whole_run':stats(rows),'tail':stats(tail),
        'latest_frame':rows[-1]['frame'] if rows else None,
        'after_frame':a.after_frame,'through_frame':a.through_frame,
        'scene_identity_requires_visual_confirmation':True,
        'percentile_method':'lower order statistic: floor((n-1)*q)',
        'frame_budgets_ms':{'60fps':1000/60,'30fps':1000/30},
        'clean_performance_probe':not heavy_capture if record else None}
a.file.with_suffix('.summary.json').write_text(json.dumps(report,indent=2))
print(json.dumps(report,indent=2))
