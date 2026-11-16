"""Export original executed warp shaders, IA, constants and color references."""
import hashlib,json,os,traceback
from pathlib import Path
import renderdoc as rd
job=json.loads(Path(job_file).read_text(encoding='utf-8-sig'));out=Path(job['output']);out.mkdir(parents=True,exist_ok=True)
cap=controller=None;code=1
def write(name,value):(out/name).write_text(json.dumps(value,indent=2)+'\n')
def small(value,depth=0):
 if value is None or isinstance(value,(str,bool,int,float)):return value
 if depth>4:return type(value).__name__
 try:return [small(value[i],depth+1) for i in range(min(len(value),64))]
 except (TypeError,AttributeError):pass
 result={}
 for name in dir(value):
  if name.startswith('_') or name in ('this','thisown','reflection','rawBytes'):continue
  try:
   member=getattr(value,name)
   if not callable(member):result[name]=small(member,depth+1)
  except Exception:pass
 return result or str(value)
def save(name,data):
 data=bytes(data);(out/name).write_bytes(data);return dict(file=name,bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
try:
 write('progress.json',dict(stage='opening'));cap=rd.OpenCaptureFile();status=cap.OpenFile(job['capture'],'',None)
 if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
 status,controller=cap.OpenCapture(rd.ReplayOptions(),None)
 if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
 controller.SetFrameEvent(job['event'],True);pipe=controller.GetPipelineState();state=controller.GetD3D12PipelineState()
 def flatten(items):
  for a in items:yield a;yield from flatten(a.children)
 action=next(a for a in flatten(controller.GetRootActions()) if a.eventId==job['event'])
 shaders={}
 for stage,name in ((rd.ShaderStage.Vertex,'vs'),(rd.ShaderStage.Pixel,'ps')):
  reflection=pipe.GetShaderReflection(stage);saved=save(name+'.dxil',reflection.rawBytes)
  if job.get(name+'_sha256') and saved['sha256']!=job[name+'_sha256']:raise RuntimeError('Unexpected executed '+name)
  shaders[name]=dict(**saved,resource=str(pipe.GetShader(stage)),inputs=small(reflection.inputSignature),
   outputs=small(reflection.outputSignature),constant_blocks=small(reflection.constantBlocks))
 buffers=[]
 for index,vb in enumerate(pipe.GetVBuffers()):
  if vb.resourceId==rd.ResourceId.Null():continue
  if vb.byteSize>8*1024*1024:raise RuntimeError('Unexpectedly large bound vertex range')
  buffers.append(dict(slot=index,resource=str(vb.resourceId),offset=vb.byteOffset,stride=vb.byteStride,
   **save('vb-'+str(index)+'.bin',controller.GetBufferData(vb.resourceId,vb.byteOffset,vb.byteSize))))
 constants=[]
 for register,length in ((0,4096),(1,4096),(2,624)):
  binding=next((p.descriptor for p in state.rootSignature.parameters if p.space==4 and p.reg==register),None)
  if not binding or binding.resource==rd.ResourceId.Null():raise RuntimeError('Missing constant buffer'+str(register))
  constants.append(dict(register=register,resource=str(binding.resource),offset=binding.byteOffset,
   **save('cb'+str(register)+'.bin',controller.GetBufferData(binding.resource,binding.byteOffset,length))))
 textures={str(t.resourceId):t for t in controller.GetTextures()}
 images=[]
 for label,event,key in [('target-before',job['event']-1,job['target']),('target-after',job['event'],job['target']),('source',job['event'],job['source'])]:
  controller.SetFrameEvent(event,True);texture=textures[key]
  images.append(dict(label=label,event=event,resource=key,width=texture.width,height=texture.height,
   format=texture.format.Name(),**save(label+'.bin',controller.GetTextureData(texture.resourceId,rd.Subresource()))))
 controller.SetFrameEvent(job['event'],True);pipe=controller.GetPipelineState()
 mesh=controller.GetPostVSData(0,0,rd.MeshDataStage.VSOut)
 post=dict(stride=mesh.vertexByteStride,vertices=mesh.numIndices,
  **save('post-vs.bin',controller.GetBufferData(mesh.vertexResourceId,mesh.vertexByteOffset,mesh.vertexByteStride*mesh.numIndices)))
 write('verification.json',dict(game_launched=False,replay_succeeded=True,frame=controller.GetFrameInfo().frameNumber,
  event=job['event'],action=small(action),vertex_inputs=small(pipe.GetVertexInputs()),vertex_buffers=buffers,
  constants=constants,shaders=shaders,post_vs=post,images=images,
  pixel_reads=small(pipe.GetReadOnlyResources(rd.ShaderStage.Pixel,True)),
  samplers=small(pipe.GetSamplers(rd.ShaderStage.Pixel)) if hasattr(pipe,'GetSamplers') else None,
  sampler_api=[n for n in dir(controller) if 'sampler' in n.lower() or 'descriptor' in n.lower()],
  descriptor_heaps=small(getattr(state,'descriptorHeaps',None)),rasterizer=small(state.rasterizer),
  input_assembly=small(getattr(state,'inputAssembly',None)),scope='actual saved original shaders/IA/constants/inputs/output; no synthesized history or game FPS'))
 code=0;write('progress.json',dict(stage='complete'))
except BaseException:
 (out/'error.txt').write_text(traceback.format_exc());write('progress.json',dict(stage='failed'))
finally:
 if controller:controller.Shutdown()
 if cap:cap.Shutdown()
 (out/'exit.txt').write_text(str(code));os._exit(code)
