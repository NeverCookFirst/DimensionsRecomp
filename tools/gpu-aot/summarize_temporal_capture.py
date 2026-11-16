"""Inspect actual production temporal inputs at FSR consumption, saved GPU frame."""
import collections,hashlib,json,os,re,struct,traceback
from pathlib import Path
import renderdoc as rd
job=json.loads(Path(job_file).read_text(encoding='utf-8-sig'));out=Path(job['output']);out.mkdir(parents=True,exist_ok=True)
cap=controller=None;code=1
def write(name,value):(out/name).write_text(json.dumps(value,indent=2)+'\n')
try:
 write('progress.json',dict(stage='opening'))
 cap=rd.OpenCaptureFile();status=cap.OpenFile(job['capture'],'',None)
 if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
 status,controller=cap.OpenCapture(rd.ReplayOptions(),None)
 if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
 resources={str(r.resourceId):r for r in controller.GetResources()};names={r.name:r.resourceId for r in resources.values()}
 reactive=names['LEGO FSR Input Reactive'];motion=names['LEGO FSR Input Motion']
 textures={str(t.resourceId):dict(width=t.width,height=t.height,format=t.format.Name()) for t in controller.GetTextures()}
 def flatten(items):
  for a in items:yield a;yield from flatten(a.children)
 actions=list(flatten(controller.GetRootActions()));by_event={a.eventId:a for a in actions}
 usages=controller.GetUsage(reactive)
 write('reactive-usage.json',[dict(event=u.eventId,usage=str(u.usage)) for u in usages])
 consumers=sorted(set(u.eventId for u in usages if 'CS_Resource' in str(u.usage)))
 if not consumers:raise RuntimeError('No actual FSR compute consumer for labelled reactive texture')
 def snapshot(event,name):
  controller.SetFrameEvent(event,True)
  mask=bytes(controller.GetTextureData(reactive,rd.Subresource()));mv=bytes(controller.GetTextureData(motion,rd.Subresource()))
  assert len(mv)==len(mask)*4
  (out/(name+'-reactive.r8')).write_bytes(mask);(out/(name+'-motion.rg16f')).write_bytes(mv)
  histogram=collections.Counter(mask)
  return dict(event=event,pixels=len(mask),rejected_pixels=histogram[255],zero_rejection_pixels=histogram[0],
   fractional_rejection_pixels=sum(n for v,n in histogram.items() if 0<v<255),
   nonzero_motion_pixels=sum(abs(x)>1e-7 or abs(y)>1e-7 for x,y in struct.iter_unpack('<ee',mv)),
   rejection_histogram=dict(sorted(histogram.items())),reactive_sha256=hashlib.sha256(mask).hexdigest(),motion_sha256=hashlib.sha256(mv).hexdigest())
 final=snapshot(consumers[0]-1,'consumed');write('consumed.json',final)
 bank=Path(job['bank']);payload=(bank/'temporal-bank.bin').read_bytes();identities={}
 for h,stage,spec,offset,size,camera in re.findall(r'\{0x([\dA-Fa-f]+)ull,(\d+),(\d+),(\d+),(\d+),(\d+)\}',(bank/'temporal-bank.cpp').read_text()):
  offset,size=int(offset),int(size);sha=hashlib.sha256(payload[offset:offset+size]).hexdigest()
  identities.setdefault(sha,[]).append(dict(hash=h,stage=int(stage),spec=int(spec),camera=int(camera)))
 writes=sorted(set(u.eventId for u in usages if 'ColorTarget' in str(u.usage) and u.eventId<consumers[0] and u.eventId in by_event))
 rows=[];shaders={};totals=collections.Counter()
 for i,event in enumerate(writes):
  controller.SetFrameEvent(event,True);pipe=controller.GetPipelineState();rid=str(pipe.GetShader(rd.ShaderStage.Pixel))
  if rid not in shaders:
   refl=pipe.GetShaderReflection(rd.ShaderStage.Pixel);data=bytes(refl.rawBytes);sha=hashlib.sha256(data).hexdigest()
   (out/(sha+'.dxil')).write_bytes(data);shaders[rid]=dict(resource=rid,sha256=sha,identities=identities.get(sha,[]))
  ps=shaders[rid];stages=set(x['stage'] for x in ps['identities']);kind='material' if any(2<=s<=6 for s in stages) else 'fallback' if any(x['hash']=='0000000000000000' for x in ps['identities']) else 'motion' if 1 in stages else 'unmatched'
  totals[kind]+=1;row=dict(event=event,count=by_event[event].numIndices,kind=kind,ps=ps)
  if kind=='material':
   state=controller.GetD3D12PipelineState();blends=state.outputMerger.blendState.blends
   row.update(motion_write_mask=blends[0].writeMask,reactive_write_mask=blends[1].writeMask,
    reactive_blend_enabled=blends[1].enabled,reactive_blend_operation=str(blends[1].colorBlend.operation),
    reactive_source=str(blends[1].colorBlend.source),reactive_destination=str(blends[1].colorBlend.destination))
  rows.append(row)
  if i%32==0:write('progress.json',dict(stage='inventory',visited=i+1,total=len(writes),totals=dict(totals),event=event))
 write('writes.json',rows)
 material=[r for r in rows if r['kind']=='material']
 assert all(r['motion_write_mask']==0 and r['reactive_blend_enabled'] and 'Max' in r['reactive_blend_operation'] for r in material)
 write('verification.json',dict(replay_succeeded=True,game_launched=False,captured_game_exe=job.get('exe_sha256'),
  frame=controller.GetFrameInfo().frameNumber,last_event=max(by_event),consumer_events=consumers,
  texture=textures[str(reactive)],consumed=final,write_totals=dict(totals),material_events=[r['event'] for r in material],
  scope='actual production captured GPU shaders and blend state; labelled temporal inputs immediately before first compute consumption; no FPS or complete FSR claim'))
 code=0;write('progress.json',dict(stage='complete',totals=dict(totals)))
except BaseException:
 (out/'error.txt').write_text(traceback.format_exc());write('progress.json',dict(stage='failed'))
finally:
 if controller:controller.Shutdown()
 if cap:cap.Shutdown()
 (out/'exit.txt').write_text(str(code));os._exit(code)
