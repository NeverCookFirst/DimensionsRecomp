"""Audit saved-frame composition attachments; no live game or synthesized state."""
import hashlib,json,os,traceback
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
    def flatten(items):
        for action in items:
            yield action
            yield from flatten(action.children)
    actions={a.eventId:a for a in flatten(controller.GetRootActions())}
    structured=controller.GetStructuredFile()
    textures={str(t.resourceId):t for t in controller.GetTextures()}
    records=[]
    for key in job['resources']:
        texture=textures[key];usage=[]
        for entry in controller.GetUsage(texture.resourceId):
            action=actions.get(entry.eventId)
            usage.append(dict(event=entry.eventId,usage=str(entry.usage),
                action=action.GetName(structured) if action else None,
                flags=str(action.flags) if action else None,
                outputs=[str(r) for r in action.outputs] if action else [],
                depth_out=str(action.depthOut) if action else None))
        records.append(dict(resource=key,width=texture.width,height=texture.height,
            format=texture.format.Name(),usage=usage))
    write('usage.json',records)
    exports=[]
    for request in job.get('exports',[]):
        controller.SetFrameEvent(request['event'],True)
        write('progress.json',dict(stage='exporting',event=request['event']))
        texture=textures[request['resource']]
        raw=bytes(controller.GetTextureData(texture.resourceId,rd.Subresource()))
        (out/(request['label']+'.bin')).write_bytes(raw)
        exports.append(dict(**request,width=texture.width,height=texture.height,
            format=texture.format.Name(),bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest()))
    write('verification.json',dict(replay_succeeded=True,game_launched=False,
        frame=controller.GetFrameInfo().frameNumber,resources=len(records),exports=exports,
        scope='one saved frame, actual resource usages and raw attachment contents'))
    write('progress.json',dict(stage='complete'));code=0
except BaseException:
    (out/'error.txt').write_text(traceback.format_exc());write('progress.json',dict(stage='failed'))
finally:
    if controller:controller.Shutdown()
    if cap:cap.Shutdown()
    (out/'exit.txt').write_text(str(code));os._exit(code)
