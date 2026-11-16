"""Offline draw-neighbour inspection inside qrenderdoc --python.

The loader defines job_file (capture/output/event/material_banks). No game is
launched. Dump bounded explicit pipeline fields and exact executed shader bytes.
"""
import hashlib, json, math, os, re, traceback, zlib
from pathlib import Path
import renderdoc as rd

job = json.loads(Path(job_file).read_text(encoding='utf-8-sig'))
out = Path(job['output']); out.mkdir(parents=True, exist_ok=True)
cap = controller = None
code = 1

def write(name, value):
    (out/name).write_text(json.dumps(value, indent=2, allow_nan=False)+'\n')

def small(value, depth=0):
    if value is None or isinstance(value, (str, bool, int)): return value
    if isinstance(value, float): return value if math.isfinite(value) else str(value)
    if depth > 5: return type(value).__name__
    # RenderDoc SWIG vectors support len/getitem, but not __iter__.
    try:
        size = len(value)
        return [small(value[i], depth+1) for i in range(min(size, 32))]
    except (TypeError, AttributeError): pass
    fields = {}
    for name in dir(value):
        if name.startswith('_') or name in ('this', 'thisown', 'reflection', 'resourceStates', 'rawBytes'): continue
        try:
            member = getattr(value, name)
            if not callable(member): fields[name] = small(member, depth+1)
        except Exception: pass
    return fields or str(value)

try:
    write('progress.json', dict(stage='opening'))
    cap = rd.OpenCaptureFile(); status = cap.OpenFile(job['capture'], '', None)
    if status != rd.ResultCode.Succeeded: raise RuntimeError(str(status))
    status, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ResultCode.Succeeded: raise RuntimeError(str(status))
    def flatten(items):
        for action in items:
            yield action
            yield from flatten(action.children)
    actions = list(flatten(controller.GetRootActions()))
    draws = [a for a in actions if a.flags & rd.ActionFlags.Drawcall]
    index = next(i for i, a in enumerate(draws) if a.eventId == job['event'])
    selected = draws[max(0,index-3):index+4]
    textures = {str(t.resourceId): dict(width=t.width, height=t.height, format=t.format.Name()) for t in controller.GetTextures()}
    resources = {str(r.resourceId): r.name for r in controller.GetResources()}
    def resource(r):
        return dict(id=str(r), name=resources.get(str(r), ''), texture=textures.get(str(r)))
    report = []
    hashes = set()
    for action in selected:
        eid = action.eventId
        write('progress.json', dict(stage='inspecting', event=eid))
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState(); state = controller.GetD3D12PipelineState()
        row = dict(event=eid, count=action.numIndices, instances=action.numInstances,
            name=action.GetName(controller.GetStructuredFile()),
            outputs=[resource(o.resource) for o in pipe.GetOutputTargets()],
            depth=small(state.outputMerger.depthStencilState),
            depth_target=resource(state.outputMerger.depthTarget.resource),
            blends=small(state.outputMerger.blendState.blends),
            viewports=small(state.rasterizer.viewports), scissors=small(state.rasterizer.scissors),
            vertex_inputs=small(pipe.GetVertexInputs()), vertex_buffers=small(pipe.GetVBuffers()),
            index_buffer=small(pipe.GetIBuffer()), root_parameters=small(state.rootSignature.parameters))
        for stage, name in ((rd.ShaderStage.Vertex,'vs'), (rd.ShaderStage.Pixel,'ps')):
            reflection = pipe.GetShaderReflection(stage)
            if reflection:
                blob = bytes(reflection.rawBytes); sha = hashlib.sha256(blob).hexdigest(); hashes.add(sha)
                (out/('event-%d-%s.dxil' % (eid,name))).write_bytes(blob)
                row[name] = dict(resource=resource(pipe.GetShader(stage)), sha256=sha,
                    constant_blocks=small(reflection.constantBlocks),
                    read_only_signature=small(reflection.readOnlyResources))
                row[name+'_reads'] = [dict(descriptor=small(u.descriptor), access=small(u.access))
                    for u in pipe.GetReadOnlyResources(stage,True)]
        # Inspect small actual bound buffers, without dumping unrelated heaps.
        for vi, vb in enumerate(pipe.GetVBuffers()):
            if vb.resourceId != rd.ResourceId.Null() and vb.byteSize <= 65536:
                (out/('event-%d-vb-%d.bin' % (eid,vi))).write_bytes(bytes(controller.GetBufferData(vb.resourceId,vb.byteOffset,vb.byteSize)))
        report.append(row); write('draws.json', report)
    identities = {h: [] for h in hashes}
    for bank_name in job.get('material_banks', []):
        bank = Path(bank_name)
        write('progress.json', dict(stage='matching-shaders', bank=str(bank)))
        data = (bank/'linked-bank.bin').read_bytes()
        try: data = zlib.decompress(data)
        except zlib.error: pass
        for h,spec,offset,size in re.findall(r'\{\s*0x([\dA-Fa-f]+),\s*0x([\dA-Fa-f]+),\s*(\d+),\s*(\d+)\s*\}', (bank/'linked-bank-metadata.cpp').read_text()):
            start, size = int(offset), int(size)
            sha = hashlib.sha256(data[start:start+size]).hexdigest()
            if sha in identities: identities[sha].append(dict(bank=str(bank), hash=h, specialization=int(spec,16)))
    write('identities.json', identities)
    write('verification.json',dict(replay_succeeded=True,game_launched=False,event=job['event'],events=[a.eventId for a in selected]))
    write('progress.json',dict(stage='complete')); code=0
except BaseException:
    (out/'error.txt').write_text(traceback.format_exc()); write('progress.json',dict(stage='failed'))
finally:
    if controller: controller.Shutdown()
    if cap: cap.Shutdown()
    (out/'exit.txt').write_text(str(code)); os._exit(code)
