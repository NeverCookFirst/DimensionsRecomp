"""Check mode segmentation and rejection of mismatched fullscreen comparisons."""
from pathlib import Path
import argparse,csv,json,subprocess,sys
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path);a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
tool=Path(__file__).with_name('summarize_frame_metrics.py')
base=dict(texture_ms=0,constants_ms=0,vertices_ms=0,buffer_hash_ms=0,present_cpu_ms=0,draw_calls=100,
 swap_present_cpu_ms=2,detailed_timing_enabled=0,present_timing_enabled=1,
 output_width=1920,output_height=1080,scene_render_width=1920,scene_render_height=1080,
 present_source_width=1920,present_source_height=1080,fsr_enabled=0,spatial_fsr_presented=0,
 presentation_monitor_index=1,presentation_refresh_hz=100,native_fps_limit_cvar=0,
 native_vsync=0,presentation_focused=1,presentation_borderless=0)
rows=[]
def segment(count,interval,**changes):
 for _ in range(count):rows.append(base|dict(frame=len(rows)+1,interval_ms=interval)|changes)
segment(200,5)
segment(200,10,presentation_borderless=1)
segment(30,25,presentation_borderless=1,presentation_focused=0)
segment(100,10,presentation_borderless=1)
segment(200,20,presentation_borderless=1,output_width=2560,output_height=1440)
segment(200,20,presentation_borderless=1,fsr_enabled=1)
segment(100,20,presentation_borderless=1,native_fps_limit_cvar=60)
segment(100,10,presentation_borderless=1,native_vsync=1)
segment(100,10,presentation_borderless=1,detailed_timing_enabled=1)
segment(100,20,presentation_borderless=1,scene_render_width=3840,scene_render_height=2160)
segment(100,10,presentation_borderless=1,presentation_monitor_index=2)
segment(100,10,presentation_borderless=1,spatial_fsr_presented=1)
def write(name,data,omit=()):
 path=out/name;fields=[k for k in data[0] if k not in omit]
 with path.open('w',newline='') as f:
  writer=csv.DictWriter(f,fields,extrasaction='ignore');writer.writeheader();writer.writerows(data)
  f.write('9999,1,') # A live, incomplete last row is excluded.
 return path
def report(path,*args):
 return json.loads(subprocess.check_output([sys.executable,str(tool),str(path),*args],text=True))
path=write('modes-frames.csv',rows);r=report(path);m=r['presentation']
assert r['whole_run']['frames']==len(rows) and m['available']
assert len(m['segments'])==12 and len(m['groups'])==11
assert [s['conditions']['presentation_borderless'] for s in m['segments'][:4]]==[0,1,1,1]
assert m['segments'][2]['conditions']['presentation_focused']==0
assert m['segments'][2]['first_frame']==401 and m['segments'][3]['first_frame']==431
assert len(m['matched_output_comparisons'])==1
comparison=m['matched_output_comparisons'][0]
assert comparison['windowed']['mean_fps']==200 and comparison['borderless']['mean_fps']==100
assert comparison['borderless']['frames']==300 and comparison['borderless_fps_divided_by_windowed_fps']==.5
assert comparison['scene_identity_requires_visual_confirmation']
assert m['groups'][1]['timing']['intervals_near_refresh_percent']==100
assert m['groups'][0]['timing']['intervals_near_refresh_percent']==0
assert not report(path,'--after-frame','200')['presentation']['matched_output_comparisons']
legacy=write('legacy-frames.csv',rows,omit=['presentation_borderless'])
legacy_result=report(legacy)
assert not legacy_result['presentation']['available']
assert legacy_result['presentation']['missing_columns']==['presentation_borderless']
assert legacy_result['whole_run']['frames']==len(rows)
disabled=write('disabled-timing-frames.csv',[r|dict(present_timing_enabled=0,swap_present_cpu_ms=99) for r in rows[:200]])
assert report(disabled)['whole_run']['swap_present_cpu_ms_mean'] is None
mismatch=write('mismatch-frames.csv',rows[:200]+[r|dict(output_width=2560,output_height=1440) for r in rows[200:400]])
assert not report(mismatch)['presentation']['matched_output_comparisons']
short=write('short-transition-frames.csv',rows[:200]+rows[200:230])
assert not report(short)['presentation']['matched_output_comparisons']
(out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,
 checks=['Equal-size windowed/borderless transition and Alt-Tab segments retained',
 'Only matching output/render/source size, monitor, FSR, profiling, VSync and cap conditions compared',
 'Unfocused rows excluded from mode comparisons','Sub-second transition groups excluded',
 'Legacy missing-mode data cannot prove a comparison','Disabled present timing is unknown rather than zero',
 'Incomplete live row excluded','Scene identity explicitly requires visual confirmation']),indent=2)+'\n')
print('PASS presentation segments, matched-workload conditions, legacy/disabled data and negative comparisons')
