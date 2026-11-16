"""Collect replay GPU-event timings from a saved capture, never live FPS.

qrenderdoc loader supplies job_file. Timestamp event sums exclude presentation,
CPU/driver submission, and workload differences introduced by RenderDoc.
"""
import json, os, traceback
from pathlib import Path
import renderdoc as rd
job=json.loads(Path(job_file).read_text(encoding='utf-8-sig'))
out=Path(job['output']);out.mkdir(parents=True,exist_ok=True)
cap=controller=None;code=1
def write(name,value):(out/name).write_text(json.dumps(value,indent=2)+'\n')
try:
    write('progress.json',dict(stage='opening'))
    cap=rd.OpenCaptureFile();status=cap.OpenFile(job['capture'],'',None)
    if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
    status,controller=cap.OpenCapture(rd.ReplayOptions(),None)
    if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
    counter=rd.GPUCounter.EventGPUDuration
    if counter not in controller.EnumerateCounters():raise RuntimeError('GPU event duration unavailable')
    description=controller.DescribeCounter(counter)
    write('counter.json',dict(name=description.name,unit=str(description.unit),type=str(description.resultType),bytes=description.resultByteWidth))
    if description.resultByteWidth!=8 or 'Seconds' not in str(description.unit):raise RuntimeError('Unsupported duration representation')
    def flatten(items):
        for action in items:
            yield action
            yield from flatten(action.children)
    actions={a.eventId:a for a in flatten(controller.GetRootActions())}
    temporal={r['event']:r['kind'] for r in json.loads(Path(job['writes']).read_text())}
    controller.SetFrameEvent(max(actions),True)
    write('progress.json',dict(stage='fetching-counters'))
    counters=controller.FetchCounters([counter]);rows=[];totals={}
    write('fetch.json',dict(results=len(counters),actions=len(actions),first_results=[dict(event=r.eventId,seconds=r.value.d) for r in counters[:16]]))
    if not counters:
        write('debug-messages.json',[dict(description=m.description,severity=str(m.severity)) for m in controller.GetDebugMessages()])
    structured=controller.GetStructuredFile()
    for result in counters:
        action=actions.get(result.eventId)
        if action is None:continue
        if action.flags & rd.ActionFlags.Drawcall:
            group='temporal_'+temporal[result.eventId] if result.eventId in temporal else 'other_draw'
        elif action.flags & rd.ActionFlags.Dispatch:group='compute'
        else:group='other_gpu_event'
        ms=result.value.d*1000
        if ms<0:raise RuntimeError('Negative duration')
        row=dict(event=result.eventId,ms=ms,group=group,name=action.GetName(structured),indices=action.numIndices)
        rows.append(row);aggregate=totals.setdefault(group,dict(events=0,ms=0.0));aggregate['events']+=1;aggregate['ms']+=ms
    if not rows:raise RuntimeError('No GPU counter results')
    rows.sort(key=lambda row:row['ms'],reverse=True)
    write('events.json',rows)
    write('verification.json',dict(replay_succeeded=True,game_launched=False,frame=controller.GetFrameInfo().frameNumber,counters=len(rows),event_ms_sum=sum(row['ms'] for row in rows),groups=totals,top_events=rows[:30],scope='saved Balanced capture replay GPU timestamps; event sums are not frame FPS, CPU cost, or native-render performance'))
    code=0;write('progress.json',dict(stage='complete'))
except BaseException:
    (out/'error.txt').write_text(traceback.format_exc());write('progress.json',dict(stage='failed'))
finally:
    if controller:controller.Shutdown()
    if cap:cap.Shutdown()
    (out/'exit.txt').write_text(str(code));os._exit(code)
