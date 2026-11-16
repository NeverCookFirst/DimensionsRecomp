"""Measure actual color contribution of the saved full-screen UI quad."""
import hashlib,json,os,traceback
from pathlib import Path
import renderdoc as rd
job=json.loads(Path(job_file).read_text(encoding='utf-8-sig'));out=Path(job['output']);out.mkdir(parents=True,exist_ok=True)
cap=controller=None;code=1
def write(name,value):(out/name).write_text(json.dumps(value,indent=2)+'\n')
try:
 write('progress.json',dict(stage='opening'))
 cap=rd.OpenCaptureFile();status=cap.OpenFile(job['capture'],'',None)
 if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
 status,controller=cap.OpenCapture(rd.ReplayOptions(),None)
 if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
 resources={str(r.resourceId):r for r in controller.GetResources()};target=resources['ResourceId::3222'].resourceId
 samples=[]
 for event in (23455,23456):
  controller.SetFrameEvent(event,True);samples.append(bytes(controller.GetTextureData(target,rd.Subresource())))
  (out/('color-'+str(event)+'.rgba8')).write_bytes(samples[-1])
 assert len(samples[0])==len(samples[1])==1129*635*4
 changed=sum(samples[0][i:i+3]!=samples[1][i:i+3] for i in range(0,len(samples[0]),4))
 controller.SetFrameEvent(26836,True);write('progress.json',dict(stage='pixel-history'))
 rows=[]
 for h in controller.PixelHistory(target,399,489,rd.Subresource(),rd.CompType.Typeless):
  if h.eventId==23456:rows.append(dict(event=h.eventId,primitive=h.primitiveID,pre=list(h.preMod.col.floatValue),
   post=list(h.postMod.col.floatValue),shader=list(h.shaderOut.col.floatValue),discarded=h.shaderDiscarded))
 assert rows
 write('pixel-history.json',rows)
 write('verification.json',dict(game_launched=False,replay_succeeded=True,changed_rgb_pixels=changed,total_pixels=1129*635,
  before_sha256=hashlib.sha256(samples[0]).hexdigest(),after_sha256=hashlib.sha256(samples[1]).hexdigest(),pixel=[399,489],history=rows))
 code=0;write('progress.json',dict(stage='complete'))
except BaseException:
 (out/'error.txt').write_text(traceback.format_exc());write('progress.json',dict(stage='failed'))
finally:
 if controller:controller.Shutdown()
 if cap:cap.Shutdown()
 (out/'exit.txt').write_text(str(code));os._exit(code)
