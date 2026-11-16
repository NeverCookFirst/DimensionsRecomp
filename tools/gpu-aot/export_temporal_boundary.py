"""Export actual color/depth/motion inputs around a saved composition boundary.

The qrenderdoc loader supplies job_file. These are one captured frame's inputs,
not a reconstructed multi-frame benchmark or proof of temporal reuse.
"""
import hashlib,json,os,traceback
from pathlib import Path
import renderdoc as rd
job=json.loads(Path(job_file).read_text(encoding='utf-8-sig'));out=Path(job['output']);out.mkdir(parents=True,exist_ok=True)
controller=cap=None;code=1
def write(name,value):(out/name).write_text(json.dumps(value,indent=2)+'\n')
try:
    write('progress.json',dict(stage='opening'))
    cap=rd.OpenCaptureFile();status=cap.OpenFile(job['capture'],'',None)
    if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
    status,controller=cap.OpenCapture(rd.ReplayOptions(),None)
    if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
    names={str(r.resourceId):r.name for r in controller.GetResources()}
    textures={str(t.resourceId):t for t in controller.GetTextures()}
    rows=[]
    for request in job['images']:
        event=request['event'];controller.SetFrameEvent(event,True)
        write('progress.json',dict(stage='exporting',event=event))
        selected=[t for key,t in textures.items() if key==request.get('resource') or names.get(key)==request.get('name')]
        if not selected or len(selected)>8:raise RuntimeError('Unexpected texture match count '+str(len(selected)))
        for index,texture in enumerate(selected):
            stem=request['label']+('' if len(selected)==1 else '-'+str(index))
            raw=bytes(controller.GetTextureData(texture.resourceId,rd.Subresource()));(out/(stem+'.bin')).write_bytes(raw)
            if request.get('png'):
                save=rd.TextureSave();save.resourceId=texture.resourceId;save.destType=rd.FileType.PNG
                status=controller.SaveTexture(save,str(out/(stem+'.png')))
                if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
            rows.append(dict(label=stem,event=event,resource=str(texture.resourceId),name=names.get(str(texture.resourceId)),width=texture.width,height=texture.height,format=texture.format.Name(),bytes=len(raw),sha256=hashlib.sha256(raw).hexdigest()))
    write('verification.json',dict(replay_succeeded=True,game_launched=False,frame=controller.GetFrameInfo().frameNumber,exports=rows,scope='one actual saved frame at explicit events, no synthesized history'))
    code=0;write('progress.json',dict(stage='complete'))
except BaseException:
    (out/'error.txt').write_text(traceback.format_exc());write('progress.json',dict(stage='failed'))
finally:
    if controller:controller.Shutdown()
    if cap:cap.Shutdown()
    (out/'exit.txt').write_text(str(code));os._exit(code)
