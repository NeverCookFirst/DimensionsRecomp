"""Measure executed scene-copy UV distortion from saved GPU post-VS data.

Run through qrenderdoc --python with job_file. The mesh/bytecode/constant data
are actual captured resources, not idealized CPU shader approximations.
"""
import hashlib, json, math, os, struct, traceback
from pathlib import Path
import renderdoc as rd
job=json.loads(Path(job_file).read_text(encoding='utf-8-sig'))
out=Path(job['output']);out.mkdir(parents=True,exist_ok=True)
cap=controller=None;code=1
def write(name,value):
    (out/name).write_text(json.dumps(value,indent=2,allow_nan=False)+'\n')
try:
    write('progress.json',dict(stage='opening'))
    cap=rd.OpenCaptureFile();status=cap.OpenFile(job['capture'],'',None)
    if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
    status,controller=cap.OpenCapture(rd.ReplayOptions(),None)
    if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
    controller.SetFrameEvent(job['event'],True)
    pipe=controller.GetPipelineState();reflection=pipe.GetShaderReflection(rd.ShaderStage.Vertex)
    signature=[dict(name=p.semanticName,index=p.semanticIndex,register=p.regIndex,components=p.compCount) for p in reflection.outputSignature]
    write('signature.json',signature)
    mesh=controller.GetPostVSData(0,0,rd.MeshDataStage.VSOut)
    write('mesh.json',dict(vertex_resource=str(mesh.vertexResourceId),offset=mesh.vertexByteOffset,stride=mesh.vertexByteStride,num_indices=mesh.numIndices,index_resource=str(mesh.indexResourceId),index_stride=mesh.indexByteStride,unproject=mesh.unproject))
    data=bytes(controller.GetBufferData(mesh.vertexResourceId,mesh.vertexByteOffset,0))
    (out/'post-vs.bin').write_bytes(data)
    uv=next(p for p in signature if p['name'].upper() in ('TEXCOORD','TEXCOORD0') and p['index']==0)
    points=[]
    for i in range(len(data)//mesh.vertexByteStride):
        offset=i*mesh.vertexByteStride
        clip=struct.unpack_from('<4f',data,offset)
        tex=struct.unpack_from('<2f',data,offset+uv['register']*16)
        if not all(math.isfinite(v) for v in (*clip,*tex)) or abs(clip[3])<1e-8:continue
        screen=[clip[0]/clip[3]*.5+.5,.5-clip[1]/clip[3]*.5]
        delta=[tex[j]-screen[j] for j in (0,1)]
        points.append(dict(vertex=i,screen_uv=screen,source_uv=list(tex),delta=delta))
    if not points:raise RuntimeError('No finite post-VS points')
    write('points.json',points)
    raw=bytes(reflection.rawBytes);(out/'executed-vs.dxil').write_bytes(raw)
    write('verification.json',dict(replay_succeeded=True,game_launched=False,frame=controller.GetFrameInfo().frameNumber,event=job['event'],vertices=len(points),stride=mesh.vertexByteStride,vs_sha256=hashlib.sha256(raw).hexdigest(),max_abs_uv_delta=max(abs(v) for p in points for v in p['delta']),rms_uv_delta=math.sqrt(sum(v*v for p in points for v in p['delta'])/(2*len(points))),scope='actual GPU post-VS positions and TEXCOORD0; must verify recorded packing/signature before interpreting UV displacement'))
    code=0;write('progress.json',dict(stage='complete'))
except BaseException:
    (out/'error.txt').write_text(traceback.format_exc());write('progress.json',dict(stage='failed'))
finally:
    if controller:controller.Shutdown()
    if cap:cap.Shutdown()
    (out/'exit.txt').write_text(str(code));os._exit(code)
