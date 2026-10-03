"""Summarize long-run events without treating transitions as confirmed faults."""
import argparse
from collections import Counter,deque
import csv
import json
from pathlib import Path
import re

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('directory',type=Path)
args=parser.parse_args()
counts=Counter(); candidates=deque(maxlen=30); intervals=[]; duration=0; pixels=[]
with (args.directory/'events.tsv').open() as file:
    for row in csv.DictReader(file,delimiter='\t'):
        if not row.get('details'): continue
        counts[row['event']]+=1
        duration=max(duration,float(row['elapsed_ms']))
        if row['anomaly_candidate']=='1': candidates.append(row)
        if row['event']=='frame':
            match=re.search(r'interval_ms=\s*([0-9.eE+-]+)',row['details'])
            if match: intervals.append(float(match[1]))
        if row['event']=='present_pixels': pixels.append(row)
ordered=sorted(intervals)
def percentile(p): return ordered[min(len(ordered)-1,int((len(ordered)-1)*p))] if ordered else None
report={'duration_seconds':duration/1000,'event_counts':dict(counts),
        'frame_count':len(intervals),'frame_ms_p50':percentile(.5),'frame_ms_p95':percentile(.95),
        'frame_ms_max':max(intervals,default=0),'last_anomaly_candidates':list(candidates),
        'present_pixel_samples':pixels,'diagnostic_capture_overhead_included':True,
        'menu_idle_10min_confirmed':False,'visual_faults_require_screenshot_review':True}
monitor=args.directory/'monitor.jsonl'
if monitor.exists():
    for line in monitor.read_text().splitlines():
        event=json.loads(line)
        if event['event']=='menu_idle_600s_elapsed': report['menu_idle_10min_confirmed']=True
(args.directory/'summary.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in report.items() if k not in ('last_anomaly_candidates','present_pixel_samples')},indent=2))
