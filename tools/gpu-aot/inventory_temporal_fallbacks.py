"""Inventory real scene/UI draws behind fallback replays in a saved capture.

The qrenderdoc loader supplies job_file. Shader identities come only from exact
executed DXIL bytes, never guessed resource names or similar material sources.
"""
import hashlib, json, os, re, struct, traceback, zlib
from pathlib import Path
import renderdoc as rd

job = json.loads(Path(job_file).read_text(encoding='utf-8-sig'))
out = Path(job['output']); out.mkdir(parents=True, exist_ok=True)
controller = cap = None
code = 1
def write(name, value):
    (out/name).write_text(json.dumps(value, indent=2)+'\n')
try:
    identities = {}
    for directory in job['material_banks']:
        bank = Path(directory)
        data = (bank/'linked-bank.bin').read_bytes()
        try: data = zlib.decompress(data)
        except zlib.error: pass
        for h, spec, offset, size in re.findall(r'\{\s*0x([\dA-Fa-f]+),\s*0x([\dA-Fa-f]+),\s*(\d+),\s*(\d+)\s*\}', (bank/'linked-bank-metadata.cpp').read_text()):
            digest = hashlib.sha256(data[int(offset):int(offset)+int(size)]).hexdigest()
            identities.setdefault(digest, []).append(dict(hash=h.upper().zfill(16), spec=int(spec,16)))
    for directory in job.get('typed_banks', []):
        bank = Path(directory); data = (bank/'float-blend-indices-bank.bin').read_bytes()
        for h, spec, offset, size, kind in re.findall(r'\{0x([\dA-Fa-f]+)ull,(\d+),(\d+),(\d+),(\d+)\}', (bank/'float-blend-indices-bank.cpp').read_text()):
            digest=hashlib.sha256(data[int(offset):int(offset)+int(size)]).hexdigest()
            identities.setdefault(digest, []).append(dict(hash=h.upper().zfill(16),spec=int(spec),typed_kind=int(kind)))
    for path in job.get('base_identities', []):
        for digest, rows in json.loads(Path(path).read_text())['identities'].items():
            identities.setdefault(digest, []).extend(rows)
    write('progress.json', dict(stage='opening'))
    cap = rd.OpenCaptureFile(); status = cap.OpenFile(job['capture'], '', None)
    if status != rd.ResultCode.Succeeded: raise RuntimeError(str(status))
    status, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ResultCode.Succeeded: raise RuntimeError(str(status))
    def flatten(items):
        for action in items:
            yield action
            yield from flatten(action.children)
    draws = [a for a in flatten(controller.GetRootActions()) if a.flags & rd.ActionFlags.Drawcall]
    positions = {a.eventId:i for i,a in enumerate(draws)}
    textures = {str(t.resourceId):dict(width=t.width,height=t.height,format=t.format.Name()) for t in controller.GetTextures()}
    source_writes = json.loads(Path(job['writes']).read_text())
    shader_cache = {}; rows = []
    def shader(pipe, stage):
        resource = str(pipe.GetShader(stage))
        if resource not in shader_cache:
            reflection = pipe.GetShaderReflection(stage)
            if not reflection: return None
            data = bytes(reflection.rawBytes); digest = hashlib.sha256(data).hexdigest()
            (out/(digest+'.dxil')).write_bytes(data)
            shader_cache[resource] = dict(resource=resource,sha256=digest,identities=identities.get(digest,[]))
        return shader_cache[resource]
    def fields(value, names):
        return {name:str(getattr(value,name)) for name in names if hasattr(value,name)}
    for write_row in source_writes:
        if write_row['kind'] != 'fallback': continue
        if job.get('events') and write_row['event'] not in job['events']: continue
        replay = write_row['event']; original = draws[positions[replay]-1]
        if original.numIndices != draws[positions[replay]].numIndices:
            raise RuntimeError('Unexpected replay predecessor at '+str(replay))
        write('progress.json',dict(stage='draw',event=original.eventId,completed=len(rows)))
        controller.SetFrameEvent(original.eventId,True)
        pipe=controller.GetPipelineState(); state=controller.GetD3D12PipelineState()
        row=dict(event=original.eventId,replay=replay,count=original.numIndices,
            vs=shader(pipe,rd.ShaderStage.Vertex),ps=shader(pipe,rd.ShaderStage.Pixel),
            depth=state.outputMerger.depthStencilState.depthEnable,
            blend=state.outputMerger.blendState.blends[0].enabled,
            targets=[dict(resource=str(t.resource),**textures.get(str(t.resource),{})) for t in pipe.GetOutputTargets()],
            vertex_reads=[dict(index=u.access.arrayElement,resource=str(u.descriptor.resource),**textures.get(str(u.descriptor.resource),{})) for u in pipe.GetReadOnlyResources(rd.ShaderStage.Vertex,True)])
        row['pixel_reads']=[dict(index=u.access.arrayElement,resource=str(u.descriptor.resource),**textures.get(str(u.descriptor.resource),{})) for u in pipe.GetReadOnlyResources(rd.ShaderStage.Pixel,True)]
        row['depth_state']=fields(state.outputMerger.depthStencilState,('depthEnable','depthWrites','depthFunction','stencilEnable','stencilReadMask','stencilWriteMask'))
        row['depth_target']=dict(resource=str(state.outputMerger.depthTarget.resource),**textures.get(str(state.outputMerger.depthTarget.resource),{}))
        blend=state.outputMerger.blendState.blends[0]
        row['blend_state']=dict(color=fields(blend.colorBlend,('source','destination','operation')),alpha=fields(blend.alphaBlend,('source','destination','operation')),state=fields(blend,('enabled','writeMask')))
        row['viewports']=[fields(view,('x','y','width','height','minDepth','maxDepth')) for view in state.rasterizer.viewports]
        if job.get('constants'):
            for register,length in ((0,4096),(2,624)):
                descriptor=next((p.descriptor for p in state.rootSignature.parameters if p.space==4 and p.reg==register),None)
                if not descriptor: continue
                data=bytes(controller.GetBufferData(descriptor.resource,descriptor.byteOffset,length))
                (out/(str(original.eventId)+'-cb'+str(register)+'.bin')).write_bytes(data)
                if register==0: row['camera_bits']=list(struct.unpack_from('<16I',data))
                if register==2: row['texture_indices']=list(struct.unpack_from('<32I',data))
        rows.append(row); write('draws.json',rows)
    pairs=sorted({(v['hash'],p['hash']) for row in rows if row['vs'] and row['ps'] for v in row['vs']['identities'] for p in row['ps']['identities']})
    (out/'pairs.txt').write_text(''.join('VS='+v+' PS='+p+'\n' for v,p in pairs))
    write('verification.json',dict(replay_succeeded=True,game_launched=False,frame=controller.GetFrameInfo().frameNumber,fallback_draws=len(rows),matched_pairs=len(pairs),unmatched=sum(not row.get('vs') or not row['vs']['identities'] or not row.get('ps') or not row['ps']['identities'] for row in rows)))
    code=0; write('progress.json',dict(stage='complete'))
except BaseException:
    (out/'error.txt').write_text(traceback.format_exc()); write('progress.json',dict(stage='failed'))
finally:
    if controller: controller.Shutdown()
    if cap: cap.Shutdown()
    (out/'exit.txt').write_text(str(code)); os._exit(code)
