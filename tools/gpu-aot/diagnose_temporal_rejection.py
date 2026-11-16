"""Find actual history loss and the original draws responsible, saved frame only."""
import hashlib,json,os,struct,traceback
from pathlib import Path
import renderdoc as rd
job=json.loads(Path(job_file).read_text(encoding='utf-8-sig'));out=Path(job['output']);out.mkdir(parents=True,exist_ok=True)
cap=controller=None;code=1
def write(name,value):(out/name).write_text(json.dumps(value,indent=2)+'\n')
try:
 cap=rd.OpenCaptureFile();status=cap.OpenFile(job['capture'],'',None)
 if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
 status,controller=cap.OpenCapture(rd.ReplayOptions(),None)
 if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
 resources={str(r.resourceId):r for r in controller.GetResources()};names={r.name:r.resourceId for r in resources.values()}
 reactive=names['LEGO FSR Input Reactive'];motion=names['LEGO FSR Input Motion']
 textures={str(t.resourceId):dict(resource=str(t.resourceId),name=resources[str(t.resourceId)].name,width=t.width,height=t.height,format=t.format.Name()) for t in controller.GetTextures()}
 def flatten(items):
  for a in items:yield a;yield from flatten(a.children)
 actions=list(flatten(controller.GetRootActions()));draws=[a for a in actions if a.flags&rd.ActionFlags.Drawcall]
 positions={a.eventId:i for i,a in enumerate(draws)};last=max(a.eventId for a in actions)
 writes=sorted(set(u.eventId for u in controller.GetUsage(reactive) if 'ColorTarget' in str(u.usage)))
 samples=[];selected=None
 def masks(event):
  controller.SetFrameEvent(event,True)
  return bytes(controller.GetTextureData(reactive,rd.Subresource())),bytes(controller.GetTextureData(motion,rd.Subresource()))
 width=textures[str(reactive)]['width']
 selection_events=sorted(set(writes[::16]+writes[-1:]),reverse=True) if job.get('selection')=='latest' else writes[::32]
 for event in selection_events:
  mask,mv=masks(event)
  valid=[i for i,value in enumerate(mask) if value<255] if job.get('selection')=='latest' else [i for i,(x,y) in enumerate(struct.iter_unpack('<ee',mv)) if mask[i]<255 and (abs(x)>1e-7 or abs(y)>1e-7)]
  samples.append(dict(event=event,reusable_motion_pixels=len(valid)));write('progress.json',dict(stage='selecting',event=event,reusable_motion_pixels=len(valid)))
  if valid:
   index=valid[len(valid)//2];selected=dict(event=event,x=index%width,y=index//width);break
 write('selection-samples.json',samples)
 if not selected:raise RuntimeError('No motion/history pixel found in sampled writes; finer scan required')
 write('selected.json',selected);controller.SetFrameEvent(last,True);write('progress.json',dict(stage='pixel-history',pixel=selected))
 rows=[]
 for h in controller.PixelHistory(reactive,selected['x'],selected['y'],rd.Subresource(),rd.CompType.Typeless):
  rows.append(dict(event=h.eventId,primitive=h.primitiveID,pre=list(h.preMod.col.floatValue),post=list(h.postMod.col.floatValue),shader=list(h.shaderOut.col.floatValue),depth_failed=h.depthTestFailed,stencil_failed=h.stencilTestFailed,discarded=h.shaderDiscarded))
 write('pixel-history.json',rows)
 changes=[r for r in rows if r['event']>=selected['event'] and r['pre'][0]<.999 and r['post'][0]>=.999 and not r['depth_failed'] and not r['stencil_failed'] and not r['discarded']]
 write('rejections.json',changes);states=[]
 for change in changes[:6]:
  event=change['event'];write('progress.json',dict(stage='original-draw',event=event))
  before,_=masks(event-1);after,_=masks(event)
  original=draws[positions[event]-1];assert original.numIndices==draws[positions[event]].numIndices
  controller.SetFrameEvent(original.eventId,True);pipe=controller.GetPipelineState();state=controller.GetD3D12PipelineState();blend=state.outputMerger.blendState.blends[0]
  row=dict(event=original.eventId,replay=event,count=original.numIndices,depth_enable=state.outputMerger.depthStencilState.depthEnable,
   blend_enabled=blend.enabled,color_source=str(blend.colorBlend.source),color_destination=str(blend.colorBlend.destination),color_operation=str(blend.colorBlend.operation),
   accepted_before=len(before)-before.count(255),accepted_after=len(after)-after.count(255),
   targets=[textures.get(str(t.resource),dict(resource=str(t.resource))) for t in pipe.GetOutputTargets()])
  for stage,name in ((rd.ShaderStage.Vertex,'vs'),(rd.ShaderStage.Pixel,'ps')):
   blob=bytes(pipe.GetShaderReflection(stage).rawBytes);sha=hashlib.sha256(blob).hexdigest();(out/(sha+'.dxil')).write_bytes(blob);row[name]=dict(resource=str(pipe.GetShader(stage)),sha256=sha)
  for register in (0,1,2,4):
   descriptor=next((p.descriptor for p in state.rootSignature.parameters if p.space==4 and p.reg==register),None)
   if descriptor:
    data=bytes(controller.GetBufferData(descriptor.resource,descriptor.byteOffset,{0:4096,1:3584,2:624,4:4112}[register]))
    (out/(str(original.eventId)+'-cb'+str(register)+'.bin')).write_bytes(data)
    if register==2:row['texture_indices']=list(struct.unpack_from('<32I',data))
  row['reads']=[dict(index=u.access.arrayElement,texture=textures.get(str(u.descriptor.resource),dict(resource=str(u.descriptor.resource)))) for u in pipe.GetReadOnlyResources(rd.ShaderStage.Pixel,True)]
  states.append(row);write('original-draws.json',states)
 write('verification.json',dict(replay_succeeded=True,game_launched=False,frame=controller.GetFrameInfo().frameNumber,pixel=selected,rejection_events=[r['event'] for r in changes],scope='actual GPU pixel history and immediately preceding original draw bindings/bytecode'))
 code=0;write('progress.json',dict(stage='complete'))
except BaseException:
 (out/'error.txt').write_text(traceback.format_exc());write('progress.json',dict(stage='failed'))
finally:
 if controller:controller.Shutdown()
 if cap:cap.Shutdown()
 (out/'exit.txt').write_text(str(code));os._exit(code)
