"""Run inside installed qrenderdoc --python through a loader setting job_file.

Find a pixel with accepted temporal history, then inspect its complete GPU
modification history. Exact executed shader bytes are matched to private banks.
This analysis does not treat a later empty snapshot as proof of its draw's fault.
"""
import hashlib,itertools,json,math,os,re,struct,traceback,zlib
from pathlib import Path
import renderdoc as rd

job=json.loads(Path(job_file).read_text())
out=Path(job['output']);out.mkdir(parents=True,exist_ok=True)
cap=controller=None;code=1
def write(name,value):
    (out/name).write_text(json.dumps(value,indent=2,allow_nan=False)+'\n')
def progress(stage,**values):
    write('progress.json',dict(stage=stage,**values))
def finite(values):
    return [v if math.isfinite(v) else str(v) for v in values]
def plain(value,depth=0):
    if value is None or isinstance(value,(str,bool,int)):return value
    if isinstance(value,float):return value if math.isfinite(value) else str(value)
    if depth>=4:return str(value)
    if isinstance(value,(list,tuple)) or hasattr(value,'__iter__'):
        try:return [plain(v,depth+1) for v in itertools.islice(value,32)]
        except TypeError:pass
    fields={}
    for name in dir(value):
        if name.startswith('_') or name in ('this','thisown'):continue
        try:
            member=getattr(value,name)
            if not callable(member):fields[name]=plain(member,depth+1)
        except Exception:pass
    return fields or str(value)
try:
    progress('opening',capture=job['capture'])
    cap=rd.OpenCaptureFile();status=cap.OpenFile(job['capture'],'',None)
    if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
    progress('replaying')
    status,controller=cap.OpenCapture(rd.ReplayOptions(),None)
    if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
    def flatten(items):
        for action in items:
            yield action
            for child in flatten(action.children):yield child
    actions=list(flatten(controller.GetRootActions()));by_event={a.eventId:a for a in actions}
    textures=controller.GetTextures();resources={str(r.resourceId):r.name for r in controller.GetResources()}
    last=actions[-1];last_event=last.eventId
    labelled=[dict(id=str(t.resourceId),name=resources.get(str(t.resourceId),''),width=t.width,height=t.height,format=t.format.Name())
              for t in textures if 'LEGO FSR' in resources.get(str(t.resourceId),'')]
    write('capture.json',dict(frame=controller.GetFrameInfo().frameNumber,actions=len(actions),
        draws=sum(bool(a.flags&rd.ActionFlags.Drawcall) for a in actions),last_event=last_event,
        last_action=last.GetName(controller.GetStructuredFile()),labelled=labelled))
    candidates=[t for t in textures if resources.get(str(t.resourceId))=='LEGO FSR Input Reactive']
    if len(candidates)!=1:raise RuntimeError('Expected one labelled reactive input, found '+str(len(candidates)))
    reactive=candidates[0]
    usages=controller.GetUsage(reactive.resourceId)
    write('reactive-usage.json',[dict(event=u.eventId,usage=str(u.usage)) for u in usages])
    writes=sorted(set(u.eventId for u in usages if str(u.usage) in ('ColorTarget','Clear','CopyDst','ResolveDst')
                      or any(v in str(u.usage) for v in ('ColorTarget','Clear','CopyDst','ResolveDst'))))
    if not writes:raise RuntimeError('No reactive writes found: '+str([str(u.usage) for u in usages]))
    expected_bytes=reactive.width*reactive.height
    def sample(event):
        controller.SetFrameEvent(event,True)
        data=bytes(controller.GetTextureData(reactive.resourceId,rd.Subresource()))
        if len(data)!=expected_bytes:raise RuntimeError('Unexpected R8 readback size '+str(len(data)))
        return data
    samples=[];selected=None
    # Coarse scan is only a way to select a valid pixel. PixelHistory, not
    # monotonicity or a binary search assumption, identifies its later writes.
    for index in list(range(0,len(writes),32))+[len(writes)-1]:
        eid=writes[index];data=sample(eid);accepted=len(data)-data.count(255)
        samples.append(dict(event=eid,accepted_pixels=accepted,total_pixels=len(data)))
        progress('selecting-history-pixel',event=eid,accepted_pixels=accepted)
        if accepted:
            positions=[i for i,value in enumerate(data) if value<255]
            pixel=positions[len(positions)//2];selected=(eid,pixel%reactive.width,pixel//reactive.width)
            break
    write('coarse-samples.json',samples)
    if selected is None:raise RuntimeError('No accepted temporal pixel in coarse samples; a finer scan is required')
    eid,x,y=selected;write('selected-pixel.json',dict(event=eid,x=x,y=y,width=reactive.width,height=reactive.height))
    controller.SetFrameEvent(last_event,True);progress('pixel-history',x=x,y=y)
    history=controller.PixelHistory(reactive.resourceId,x,y,rd.Subresource(),rd.CompType.Typeless)
    rows=[]
    for h in history:
        action=by_event.get(h.eventId)
        rows.append(dict(event=h.eventId,primitive=h.primitiveID,
            pre=finite(list(h.preMod.col.floatValue)),post=finite(list(h.postMod.col.floatValue)),
            shader=finite(list(h.shaderOut.col.floatValue)),depth_failed=h.depthTestFailed,
            stencil_failed=h.stencilTestFailed,discarded=h.shaderDiscarded,
            action=action.GetName(controller.GetStructuredFile()) if action else None))
    write('reactive-pixel-history.json',rows)
    changes=[r for r in rows if r['event']>=eid and isinstance(r['pre'][0],(int,float)) and isinstance(r['post'][0],(int,float))
             and r['pre'][0]<.5 and r['post'][0]>.5 and not r['depth_failed'] and not r['stencil_failed'] and not r['discarded']]
    if not changes:raise RuntimeError('Pixel history did not report an accepted-to-rejected write')
    # Bank bytes are evidence of the actual program, not a guessed association
    # from resource IDs or the high-level material's name.
    identities={}
    def add_identity(blob,identity):
        identities.setdefault(hashlib.sha256(blob).hexdigest(),[]).append(identity)
    bank=Path(job['temporal_bank']);blob=(bank/'temporal-bank.bin').read_bytes()
    for h,stage,spec,offset,size,camera in re.findall(r'\{0x([\dA-Fa-f]+)ull,(\d+),(\d+),(\d+),(\d+),(\d+)\}',(bank/'temporal-bank.cpp').read_text()):
        start,length=int(offset),int(size)
        add_identity(blob[start:start+length],dict(bank='temporal',hash=h,specialization=int(spec),stage=int(stage)))
    if job.get('material_bank'):
        bank=Path(job['material_bank']);blob=zlib.decompress((bank/'linked-bank.bin').read_bytes())
        for h,spec,offset,size in re.findall(r'\{ 0x([\dA-Fa-f]+), 0x([\dA-Fa-f]+), (\d+), (\d+) \}',(bank/'linked-bank-metadata.cpp').read_text()):
            start,length=int(offset),int(size)
            add_identity(blob[start:start+length],dict(bank='material',hash=h,specialization=int(spec,16)))
    states=[]
    for index,row in enumerate(changes[:8]):
        event=row['event'];progress('examining-rejection-write',event=event)
        action=by_event.get(event)
        before=sample(max(0,event-1));after=sample(event)
        state=dict(event=event,accepted_before=len(before)-before.count(255),accepted_after=len(after)-after.count(255),
                   action=row['action'],pixel_before=row['pre'],pixel_after=row['post'])
        if action and action.flags&rd.ActionFlags.Drawcall:
            pipe=controller.GetPipelineState()
            state.update(count=action.numIndices,instances=action.numInstances,base_vertex=action.baseVertex,
                outputs=[dict(resource=str(o.resource),name=resources.get(str(o.resource),'')) for o in pipe.GetOutputTargets()],
                vertex_buffers=plain(pipe.GetVBuffers()),index_buffer=plain(pipe.GetIBuffer()),
                pixel_reads=plain(pipe.GetReadOnlyResources(rd.ShaderStage.Pixel,True)),
                d3d12=plain(controller.GetD3D12PipelineState()))
            for stage,name in [(rd.ShaderStage.Vertex,'vs'),(rd.ShaderStage.Pixel,'ps')]:
                reflection=pipe.GetShaderReflection(stage)
                if reflection:
                    data=bytes(reflection.rawBytes);sha=hashlib.sha256(data).hexdigest()
                    (out/('event-'+str(event)+'-'+name+'.dxil')).write_bytes(data)
                    state[name]=dict(resource=str(pipe.GetShader(stage)),sha256=sha,identities=identities.get(sha,[]))
        states.append(state);write('rejection-writes.json',states)
    controller.SetFrameEvent(last_event,True)
    final=sample(last_event)
    write('verification.json',dict(replay_succeeded=True,frame=controller.GetFrameInfo().frameNumber,
        complete_frame_last_event=last_event,selected_pixel=[x,y],rejection_events=[r['event'] for r in changes],
        accepted_pixels_final=len(final)-final.count(255),total_pixels=len(final),
        scope='actual captured GPU frame and pixel history; shader byte identity where matched',game_launched=False))
    code=0;progress('complete',rejection_events=[r['event'] for r in changes])
except BaseException:
    (out/'error.txt').write_text(traceback.format_exc());progress('failed')
finally:
    if controller:controller.Shutdown()
    if cap:cap.Shutdown()
    (out/'exit.txt').write_text(str(code));os._exit(code)
