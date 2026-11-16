"""Summarize opt-in TT lock traces without treating lock time as frame rate."""
import argparse,collections,csv,hashlib,io,json,re,statistics
from pathlib import Path

def distribution(values):
    if not values:return dict(count=0,total=0,mean=None,median=None,p95=None,max=None)
    ordered=sorted(values)
    return dict(count=len(values),total=sum(values),mean=statistics.mean(values),
        median=statistics.median(values),p95=ordered[max(0,(95*len(values)+99)//100-1)],max=ordered[-1])

def analyze(path):
    raw=path.read_bytes();text=raw.decode('utf-8-sig');lines=text.splitlines(keepends=True)
    if len(lines)<2 or not lines[0].startswith('#tt_lock_trace_v1,'):raise ValueError('Missing TT lock trace metadata')
    match=re.search(r'qpc_frequency=(\d+)',lines[0]);frequency=int(match[1]) if match else 0
    if frequency<=0 or 'target=0x84959EDC' not in lines[0] or 'cpu_unit=100ns' not in lines[0]:raise ValueError('Invalid clock/target metadata')
    partial=bool(lines and not lines[-1].endswith('\n'))
    if partial:lines=lines[:-1]
    rows=list(csv.DictReader(io.StringIO(''.join(lines[1:]))));previous=0
    numeric=['sequence','tid','wait_begin_qpc','acquired_qpc','released_qpc','wait_cpu_100ns','hold_cpu_100ns',
        'wall_valid','cpu_valid','native_draw_attempts','native_draw_qpc','native_valid','render_calls','render_qpc','render_valid','scopes_closed','orphan_release']
    flags=['wall_valid','cpu_valid','native_valid','render_valid','scopes_closed','orphan_release']
    for row in rows:
        if None in row or any(row.get(k) is None for k in numeric):raise ValueError('Incomplete interior row')
        for key in numeric:
            row[key]=int(row[key])
            if not 0<=row[key]<=2**64-1:raise ValueError('Invalid unsigned field '+key)
        if any(row[k] not in (0,1) for k in flags):raise ValueError('Invalid flag')
        if row['sequence']<=previous:raise ValueError('Nonmonotonic sequence')
        previous=row['sequence']
        if int(row['lock'],0)!=0x84959EDC:raise ValueError('Unexpected target lock')
        if row['wall_valid'] and (row['orphan_release'] or not row['wait_begin_qpc']<=row['acquired_qpc']<=row['released_qpc']):raise ValueError('Nonmonotonic valid wall clock')
        if row['orphan_release'] and row['cpu_valid']:raise ValueError('Orphan release cannot have valid CPU timing')
        hold=row['released_qpc']-row['acquired_qpc']
        if row['wall_valid'] and row['scopes_closed']:
            if row['native_valid'] and row['native_draw_qpc']>hold:raise ValueError('Native span extends beyond lock hold')
            if row['render_valid'] and row['render_qpc']>hold:raise ValueError('Render span extends beyond lock hold')
        row['role']='invalid' if row['orphan_release'] else 'render' if row['render_calls'] else 'other'
    def summary(group):
        wall=[r for r in group if r['wall_valid']];cpu=[r for r in group if r['cpu_valid']]
        native=[r for r in wall if r['native_valid'] and r['scopes_closed']]
        render=[r for r in wall if r['render_valid'] and r['scopes_closed']]
        scale=1000/frequency
        return dict(rows=len(group),invalid_wall_rows=len(group)-len(wall),invalid_cpu_rows=len(group)-len(cpu),
            orphan_releases=sum(r['orphan_release'] for r in group),unclosed_scope_rows=sum(not r['scopes_closed'] for r in group),
            wait_wall_ms=distribution([(r['acquired_qpc']-r['wait_begin_qpc'])*scale for r in wall]),
            hold_wall_ms=distribution([(r['released_qpc']-r['acquired_qpc'])*scale for r in wall]),
            wait_cpu_ms=distribution([r['wait_cpu_100ns']/10000 for r in cpu]),
            hold_cpu_ms=distribution([r['hold_cpu_100ns']/10000 for r in cpu]),
            native_draw_attempts=sum(r['native_draw_attempts'] for r in group),
            native_wall_ms=distribution([r['native_draw_qpc']*scale for r in native]),
            render_calls=sum(r['render_calls'] for r in group),
            render_wall_ms=distribution([r['render_qpc']*scale for r in render]))
    roles={role:summary([r for r in rows if r['role']==role]) for role in sorted({r['role'] for r in rows})}
    threads={str(tid):summary([r for r in rows if r['tid']==tid]) for tid in sorted({r['tid'] for r in rows})}
    return dict(path=str(path.resolve()),sha256=hashlib.sha256(raw).hexdigest(),qpc_frequency=frequency,rows=len(rows),
        discarded_partial_tail=partial,roles=roles,threads=threads,short_render_trace=sum(r['render_calls'] for r in rows)<120,
        interpretation=['Roles describe observed render spans, not inferred thread names; other includes update/maintenance.',
          'GetThreadTimes has100ns units but can be coarsely updated; aggregate long windows, do not read0 as no CPU work.',
          'Native/render spans are inclusive CPU-side wall time, including waits; they are not GPU durations.',
          'Instrumentation adds overhead; lock hold time is not frame rate and these data do not establish100FPS.'])

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('trace',type=Path,nargs='+');p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    report=dict(analysis_launched_game=False,contains_game_fps_measurement=False,traces=[analyze(path) for path in a.trace])
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n')
    for trace in report['traces']:
        print(trace['path'],':',trace['rows'],'rows; partial tail discarded:',trace['discarded_partial_tail'])
        for role,data in trace['roles'].items():print(' ',role,'rows',data['rows'],'mean hold ms',data['hold_wall_ms']['mean'],'total CPU hold ms',data['hold_cpu_ms']['total'])
if __name__=='__main__':main()
