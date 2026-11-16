"""Inspect executed shader input masks versus bound IA streams in a saved frame."""
import json,os,re,traceback
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
    def flatten(actions):
        for action in actions:
            yield action
            yield from flatten(action.children)
    skip={r['event'] for r in json.loads(Path(job['temporal_writes']).read_text())}
    actions=[a for a in flatten(controller.GetRootActions()) if a.flags&rd.ActionFlags.Drawcall and a.eventId not in skip]
    if job.get('events'):actions=[a for a in actions if a.eventId in job['events']]
    shader_cache={};rows=[];unknown=0;unknown_examples=[]
    for index,action in enumerate(actions):
        controller.SetFrameEvent(action.eventId,True);pipe=controller.GetPipelineState()
        if index%64==0:write('progress.json',dict(stage='draw',completed=index,total=len(actions)))
        reflection=pipe.GetShaderReflection(rd.ShaderStage.Vertex)
        if not reflection:continue
        key=str(pipe.GetShader(rd.ShaderStage.Vertex))
        if key not in shader_cache:
            signature=[]
            for param in reflection.inputSignature:
                if not hasattr(param,'channelUsedMask'):raise RuntimeError('Missing actual shader input read mask')
                signature.append(dict(name=param.semanticName,index=param.semanticIndex,used_mask=param.channelUsedMask,
                    variable=getattr(param,'varName','')))
            shader_cache[key]=signature
        signature=shader_cache[key];attrs=pipe.GetVertexInputs();buffers=pipe.GetVBuffers()
        declared=set();used=set();unmatched=[]
        for attr in attrs:
            slot=attr.vertexBuffer;declared.add(slot)
            # RenderDoc's vertex input attribute name includes the semantic index.
            matches=[s for s in signature if attr.name.upper() in
                (s['name'].upper()+str(s['index']),s['name'].upper() if s['index']==0 else '',s['variable'].upper())]
            # A complete input signature can omit unused declaration elements.
            # Accept only the observed D3D semantic spelling; keep other names
            # conservative rather than guessing their shader mapping.
            if len(matches)>1 or (not matches and not re.fullmatch(r'[A-Z_]+[0-9]*',attr.name.upper())):
                used.add(slot);unmatched.append(attr.name)
            elif not matches:pass
            elif matches[0]['used_mask']:used.add(slot)
        unused=[]
        for slot in sorted(declared-used):
            if slot>=len(buffers):continue
            buffer=buffers[slot]
            if buffer.resourceId==rd.ResourceId.Null():continue
            unused.append(dict(slot=slot,resource=str(buffer.resourceId),bytes=buffer.byteSize,stride=buffer.byteStride))
        if unmatched:
            unknown+=1
            if len(unknown_examples)<8:unknown_examples.append(dict(event=action.eventId,signature=signature,
                attributes=[dict(name=a.name,slot=a.vertexBuffer) for a in attrs]))
        if unused:rows.append(dict(event=action.eventId,vertices=action.numIndices,shader=key,
            inputs=signature,unused=unused,unmatched=unmatched))
    write('draws.json',rows)
    write('unmatched.json',unknown_examples)
    write('verification.json',dict(game_launched=False,replay_succeeded=True,
        frame=controller.GetFrameInfo().frameNumber,original_draws=len(actions),draws_with_unused_streams=len(rows),
        unused_stream_bindings=sum(len(r['unused']) for r in rows),unmatched_draws=unknown,
        bound_unused_bytes_sum=sum(s['bytes'] for r in rows for s in r['unused']),
        scope='actual executed shader read masks and bound host IA ranges; not guest upload count, bytes converted, timing or FPS'))
    code=0;write('progress.json',dict(stage='complete'))
except BaseException:
    (out/'error.txt').write_text(traceback.format_exc());write('progress.json',dict(stage='failed'))
finally:
    if controller:controller.Shutdown()
    if cap:cap.Shutdown()
    (out/'exit.txt').write_text(str(code));os._exit(code)
