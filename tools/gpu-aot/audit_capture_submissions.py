"""Read query/submission call structure from a saved capture, without game UI."""
import collections,json,os,traceback
from pathlib import Path
import renderdoc as rd
job=json.loads(Path(job_file).read_text(encoding='utf-8-sig'));out=Path(job['output']);out.mkdir(parents=True,exist_ok=True)
cap=controller=None;code=1
def write(name,value):(out/name).write_text(json.dumps(value,indent=2)+'\n')
try:
 write('progress.json',dict(stage='opening'));cap=rd.OpenCaptureFile();status=cap.OpenFile(job['capture'],'',None)
 if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
 status,controller=cap.OpenCapture(rd.ReplayOptions(),None)
 if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
 structured=controller.GetStructuredFile();names=collections.Counter(c.name for c in structured.chunks)
 selected={name:n for name,n in names.items() if any(part in name.lower() for part in ('query','executecommand','signal','wait','present','reset','close'))}
 examples=[];timeline=[]
 for index,chunk in enumerate(structured.chunks):
  if any(part in chunk.name.lower() for part in ('endquery','executecommandlists','::signal','::present')):
   timeline.append(dict(chunk=index,name=chunk.name))
  if any(part in chunk.name.lower() for part in ('beginquery','endquery','executecommandlists','::signal')):
   if len(examples)<8:examples.append(dict(chunk=index,name=chunk.name,
       parameters=[chunk.GetChild(i).name for i in range(chunk.NumChildren())]))
 def flatten(actions):
  for a in actions:yield a;yield from flatten(a.children)
 actions=list(flatten(controller.GetRootActions()))
 roots=[dict(event=a.eventId,name=a.GetName(structured),children=len(a.children),flags=str(a.flags)) for a in controller.GetRootActions()]
 write('verification.json',dict(game_launched=False,frame=controller.GetFrameInfo().frameNumber,
  chunks=len(structured.chunks),selected_counts=selected,root_actions=roots,examples=examples,timeline=timeline,
  draw_actions=sum(bool(a.flags&rd.ActionFlags.Drawcall) for a in actions),
  scope='saved capture API structure/counts, not native timing or CPU/GPU overlap'))
 write('chunk-counts.json',dict(names));write('progress.json',dict(stage='complete'));code=0
except BaseException:
 (out/'error.txt').write_text(traceback.format_exc());write('progress.json',dict(stage='failed'))
finally:
 if controller:controller.Shutdown()
 if cap:cap.Shutdown()
 (out/'exit.txt').write_text(str(code));os._exit(code)
