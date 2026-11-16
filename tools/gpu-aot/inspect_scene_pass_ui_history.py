"""Inspect the next rejection after selective lighting repair, saved frame only."""
import hashlib,json,os,struct,traceback
from pathlib import Path
import renderdoc as rd
job=json.loads(Path(job_file).read_text(encoding='utf-8-sig'));out=Path(job['output']);out.mkdir(parents=True,exist_ok=True)
controller=cap=None;replacement=None;code=1
def write(name,value):(out/name).write_text(json.dumps(value,indent=2)+'\n')
try:
 write('progress.json',dict(stage='opening'))
 cap=rd.OpenCaptureFile();status=cap.OpenFile(job['capture'],'',None)
 if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
 status,controller=cap.OpenCapture(rd.ReplayOptions(),None)
 if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
 resources={str(r.resourceId):r for r in controller.GetResources()}
 original=resources[job['fallback_ps']].resourceId
 reactive=next(r.resourceId for r in resources.values() if r.name=='LEGO FSR Input Reactive')
 replacement,errors=controller.BuildTargetShader('main',rd.ShaderEncoding.DXIL,
  Path(job['replacement']).read_bytes(),rd.ShaderCompileFlags(),rd.ShaderStage.Pixel)
 if replacement==rd.ResourceId.Null():raise RuntimeError(errors)
 controller.ReplaceResource(original,replacement)
 controller.SetFrameEvent(26836,True);write('progress.json',dict(stage='pixel-history'))
 history=controller.PixelHistory(reactive,399,489,rd.Subresource(),rd.CompType.Typeless)
 rows=[]
 for h in history:
  rows.append(dict(event=h.eventId,primitive=h.primitiveID,pre=list(h.preMod.col.floatValue),
   post=list(h.postMod.col.floatValue),shader=list(h.shaderOut.col.floatValue),
   depth_failed=h.depthTestFailed,stencil_failed=h.stencilTestFailed,discarded=h.shaderDiscarded))
 write('pixel-history.json',rows)
 changes=[r for r in rows if r['event']>16524 and r['pre'][0]<.5 and r['post'][0]>.5
  and not r['depth_failed'] and not r['stencil_failed'] and not r['discarded']]
 write('rejections.json',changes)
 assert changes,'No later rejection at previously verified history pixel'
 def flatten(items):
  for a in items:yield a;yield from flatten(a.children)
 draws=[a for a in flatten(controller.GetRootActions()) if a.flags&rd.ActionFlags.Drawcall]
 positions={a.eventId:i for i,a in enumerate(draws)};states=[]
 for row in changes[:4]:
  event=row['event'];action=draws[positions[event]-1]
  controller.SetFrameEvent(action.eventId,True);pipe=controller.GetPipelineState();state=controller.GetD3D12PipelineState()
  reflection=pipe.GetShaderReflection(rd.ShaderStage.Pixel);blob=bytes(reflection.rawBytes)
  (out/('original-'+str(action.eventId)+'-ps.dxil')).write_bytes(blob)
  reads=[]
  for item in pipe.GetReadOnlyResources(rd.ShaderStage.Pixel,True):
   resource=resources.get(str(item.descriptor.resource))
   reads.append(dict(index=item.access.arrayElement,resource=str(item.descriptor.resource),name=resource.name if resource else ''))
  descriptor=next(p.descriptor for p in state.rootSignature.parameters if p.space==4 and p.reg==2)
  shared=bytes(controller.GetBufferData(descriptor.resource,descriptor.byteOffset,624))
  states.append(dict(event=action.eventId,replay=event,count=action.numIndices,ps_sha256=hashlib.sha256(blob).hexdigest(),
   depth_enable=state.outputMerger.depthStencilState.depthEnable,
   blend_enabled=state.outputMerger.blendState.blends[0].enabled,
   texture_indices=list(struct.unpack_from('<32I',shared)),reads=reads))
 write('original-draws.json',states)
 write('verification.json',dict(game_launched=False,replay_succeeded=True,pixel=[399,489],
  later_rejection_events=[r['event'] for r in changes],scope='actual pixel history with only seven audited lighting/fog fallback events suppressed'))
 code=0;write('progress.json',dict(stage='complete'))
except BaseException:
 (out/'error.txt').write_text(traceback.format_exc());write('progress.json',dict(stage='failed'))
finally:
 if controller:
  if replacement and replacement!=rd.ResourceId.Null():controller.RemoveReplacement(original);controller.FreeTargetResource(replacement)
  controller.Shutdown()
 if cap:cap.Shutdown()
 (out/'exit.txt').write_text(str(code));os._exit(code)
