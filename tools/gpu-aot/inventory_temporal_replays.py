"""Inventory actual fallback replays in a saved frame; qrenderdoc loader job_file.

This launches no game. Keep original draw bindings and exact shader bytes so
lighting/composition cannot be confused with a UI or world-geometry pass.
"""
import hashlib,json,os,struct,traceback
from pathlib import Path
import renderdoc as rd

job=json.loads(Path(job_file).read_text(encoding='utf-8-sig'))
out=Path(job['output']);out.mkdir(parents=True,exist_ok=True)
cap=controller=None;code=1
def write(name,value):
    (out/name).write_text(json.dumps(value,indent=2)+'\n')
try:
    write('progress.json',dict(stage='opening'))
    cap=rd.OpenCaptureFile();status=cap.OpenFile(job['capture'],'',None)
    if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
    status,controller=cap.OpenCapture(rd.ReplayOptions(),None)
    if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
    def flatten(items):
        for a in items:
            yield a
            yield from flatten(a.children)
    actions=list(flatten(controller.GetRootActions()))
    draws=[a for a in actions if a.flags&rd.ActionFlags.Drawcall]
    positions={a.eventId:i for i,a in enumerate(draws)}
    names={str(r.resourceId):r.name for r in controller.GetResources()}
    textures={str(t.resourceId):dict(id=str(t.resourceId),name=names.get(str(t.resourceId),''),
        width=t.width,height=t.height,format=t.format.Name()) for t in controller.GetTextures()}
    reactive=next(t for t in controller.GetTextures() if names.get(str(t.resourceId))=='LEGO FSR Input Reactive')
    write_events=sorted(set(u.eventId for u in controller.GetUsage(reactive.resourceId)
        if 'ColorTarget' in str(u.usage) and u.eventId in positions))
    rows=[];groups={};shader_cache={}
    def shader(pipe,stage):
        rid=str(pipe.GetShader(stage))
        if rid not in shader_cache:
            refl=pipe.GetShaderReflection(stage)
            if not refl:return dict(id=rid)
            blob=bytes(refl.rawBytes);sha=hashlib.sha256(blob).hexdigest()
            (out/(sha+'.dxil')).write_bytes(blob)
            shader_cache[rid]=dict(id=rid,sha256=sha)
        return shader_cache[rid]
    for sequence,event in enumerate(write_events):
        controller.SetFrameEvent(event,True);pipe=controller.GetPipelineState()
        if str(pipe.GetShader(rd.ShaderStage.Pixel))!=job['fallback_ps']:
            continue
        original=draws[positions[event]-1];replay=draws[positions[event]]
        if original.numIndices!=replay.numIndices or original.numInstances!=replay.numInstances:
            raise RuntimeError('Preceding original draw does not match replay '+str(event))
        controller.SetFrameEvent(original.eventId,True)
        pipe=controller.GetPipelineState();state=controller.GetD3D12PipelineState()
        vs=shader(pipe,rd.ShaderStage.Vertex);ps=shader(pipe,rd.ShaderStage.Pixel)
        targets=[textures.get(str(t.resource),dict(id=str(t.resource))) for t in pipe.GetOutputTargets()]
        if any(t['id']==str(reactive.resourceId) for t in targets):
            raise RuntimeError('Preceding draw was another temporal replay '+str(event))
        depth=state.outputMerger.depthStencilState
        blend=state.outputMerger.blendState.blends[0]
        row=dict(event=original.eventId,replay=event,count=original.numIndices,vs=vs,ps=ps,targets=targets,
            depth_enable=depth.depthEnable,depth_write=depth.depthWrites,
            blend_enabled=blend.enabled,write_mask=blend.writeMask,
            color_source=str(blend.colorBlend.source),color_destination=str(blend.colorBlend.destination),
            color_operation=str(blend.colorBlend.operation))
        shared=next((p.descriptor for p in state.rootSignature.parameters if p.space==4 and p.reg==2),None)
        if shared:
            data=bytes(controller.GetBufferData(shared.resource,shared.byteOffset,624))
            if len(data)!=624:raise RuntimeError('Shared constants unavailable')
            row['texture_indices']=list(struct.unpack_from('<32I',data))
        key=json.dumps([ps.get('sha256'),[t['id'] for t in targets],row['depth_enable'],row['blend_enabled'],row['write_mask']])
        if key not in groups:
            reads=[]
            for u in pipe.GetReadOnlyResources(rd.ShaderStage.Pixel,True):
                desc=u.descriptor
                reads.append(dict(index=u.access.arrayElement,resource=textures.get(str(desc.resource),dict(id=str(desc.resource)))))
            row['reads']=reads
            groups[key]=dict(example=row,events=[])
        groups[key]['events'].append(event);rows.append(row)
        if len(rows)%32==0:
            write('progress.json',dict(stage='inventory',visited=sequence+1,total=len(write_events),fallbacks=len(rows),groups=len(groups),event=event))
            write('groups.json',list(groups.values()))
    write('draws.json',rows);write('groups.json',list(groups.values()));write('textures.json',textures)
    write('api-docs.json',{n:getattr(controller,n).__doc__ for n in ('BuildTargetShader','ReplaceResource','RemoveReplacement','FreeTargetResource')})
    write('verification.json',dict(replay_succeeded=True,game_launched=False,frame=controller.GetFrameInfo().frameNumber,
        reactive_writes=len(write_events),fallback_draws=len(rows),groups=len(groups),scope='actual saved command stream; original draw immediately preceding each verified fallback replay'))
    write('progress.json',dict(stage='complete',fallbacks=len(rows),groups=len(groups)));code=0
except BaseException:
    (out/'error.txt').write_text(traceback.format_exc());write('progress.json',dict(stage='failed'))
finally:
    if controller:controller.Shutdown()
    if cap:cap.Shutdown()
    (out/'exit.txt').write_text(str(code));os._exit(code)
