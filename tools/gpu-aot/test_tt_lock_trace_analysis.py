"""Independent synthetic timing expectations and corrupt/partial capture cases."""
import argparse,json,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('output',type=Path);p.add_argument('--compiler');a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
script=Path(__file__).with_name('analyze_tt_lock_trace.py')
header='#tt_lock_trace_v1,qpc_frequency=1000,target=0x84959EDC,cpu_unit=100ns\nsequence,tid,lock,wait_begin_qpc,acquired_qpc,released_qpc,wait_cpu_100ns,hold_cpu_100ns,wall_valid,cpu_valid,native_draw_attempts,native_draw_qpc,native_valid,render_calls,render_qpc,render_valid,scopes_closed,orphan_release\n'
rows=['1,1,0x84959EDC,0,10,80,1000,500000,1,1,100,50,1,1,60,1,1,0',
      '2,2,0x84959EDC,0,80,110,2000,150000,1,1,0,0,1,0,0,1,1,0',
      '3,2,0x84959EDC,110,120,130,0,0,1,0,0,0,1,0,0,1,1,0',
      '4,3,0x84959EDC,0,0,0,0,0,0,0,0,0,1,0,0,1,1,1']
def run(name,data):
 trace=a.output/(name+'.csv');result=a.output/(name+'.json');trace.write_text(data)
 r=subprocess.run([sys.executable,str(script),str(trace),'--output',str(result)],capture_output=True,text=True,timeout=10)
 return r,result
r,result=run('known',header+'\n'.join(rows)+'\n5,2')
assert r.returncode==0,r.stderr;report=json.loads(result.read_text());t=report['traces'][0]
assert t['rows']==4 and t['discarded_partial_tail'] and not report['contains_game_fps_measurement']
assert t['roles']['render']['hold_wall_ms']['mean']==70 and t['roles']['render']['hold_cpu_ms']['total']==50
assert t['roles']['render']['native_wall_ms']['total']==50 and t['roles']['render']['native_draw_attempts']==100
assert t['roles']['other']['hold_wall_ms']['mean']==20 and t['roles']['other']['hold_cpu_ms']['count']==1
assert t['roles']['other']['hold_cpu_ms']['total']==15 and t['roles']['other']['invalid_cpu_rows']==1
assert t['roles']['invalid']['orphan_releases']==1
for name,bad,reason in [('backwards',rows[0].replace('1,1,','3,1,',1).replace(',10,80,',',10,9,'),'Nonmonotonic valid wall clock'),
                 ('span-too-long',rows[0].replace('1,1,','3,1,',1).replace(',100,50,',',100,90,'),'Native span extends'),
                 ('duplicate-sequence',rows[0],'Nonmonotonic sequence')]:
 data=header+'\n'.join(rows[:2]+[bad])+'\n';r,_=run(name,data)
 assert r.returncode!=0 and reason in r.stderr,(name,r.stdout,r.stderr)
(a.output/'verification.json').write_text(json.dumps(dict(result='PASS',analysis_launched_game=False,
    known_wall_cpu_units=True,invalid_cpu_excluded=True,partial_tail_reported=True,corrupt_inputs_rejected=True,fps_inference_absent=True),indent=2))
print('PASS trace analysis units/roles, invalid CPU exclusion, partial tail and malformed input handling')
