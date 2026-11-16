"""Run prepared selective history counterfactual in qrenderdoc; job_file loader."""
import hashlib,json,os,struct,traceback
from pathlib import Path
import renderdoc as rd
job=json.loads(Path(job_file).read_text(encoding='utf-8-sig'))
out=Path(job['output']);out.mkdir(parents=True,exist_ok=True)
cap=controller=None;replacement=None;code=1
def write(name,value): (out/name).write_text(json.dumps(value,indent=2)+'\n')
try:
 write('progress.json',dict(stage='opening'))
 cap=rd.OpenCaptureFile();status=cap.OpenFile(job['capture'],'',None)
 if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
 status,controller=cap.OpenCapture(rd.ReplayOptions(),None)
 if status!=rd.ResultCode.Succeeded:raise RuntimeError(str(status))
 resources={str(r.resourceId):r for r in controller.GetResources()}
 def rid(text):return resources[text].resourceId
 textures={r.name:r.resourceId for r in controller.GetResources()}
 reactive=textures['LEGO FSR Input Reactive'];motion=textures['LEGO FSR Input Motion']
 color=rid(job['color_target']);original=rid(job['fallback_ps'])
 def flatten(items):
  for a in items:yield a;yield from flatten(a.children)
 last=max(a.eventId for a in flatten(controller.GetRootActions()))
 events=sorted(set([job['selected_events'][0]-3,*job['selected_events'],20917,23459,26594,last]))
 def sample(event):
  controller.SetFrameEvent(event,True)
  data=bytes(controller.GetTextureData(reactive,rd.Subresource()))
  mv=bytes(controller.GetTextureData(motion,rd.Subresource()))
  scene=bytes(controller.GetTextureData(color,rd.Subresource()))
  return dict(event=event,accepted_pixels=len(data)-data.count(255),pixels=len(data),
   nonzero_motion_pixels=sum(abs(x)>1e-7 or abs(y)>1e-7 for x,y in struct.iter_unpack('<ee',mv)),
   reactive_sha256=hashlib.sha256(data).hexdigest(),motion_sha256=hashlib.sha256(mv).hexdigest(),
   scene_color_sha256=hashlib.sha256(scene).hexdigest())
 # Verify original/replay constants agree before using exact binding signatures.
 for event in job['selected_events']:
  controller.SetFrameEvent(event,True);state=controller.GetD3D12PipelineState()
  descriptor=next(p.descriptor for p in state.rootSignature.parameters if p.space==4 and p.reg==2)
  data=bytes(controller.GetBufferData(descriptor.resource,descriptor.byteOffset,128))
  assert list(struct.unpack('<32I',data))==job['selected_bindings'][str(event)]
 baseline=[]
 for event in events:
  write('progress.json',dict(stage='baseline',event=event));baseline.append(sample(event))
 write('baseline.json',baseline)
 replacement,errors=controller.BuildTargetShader('main',rd.ShaderEncoding.DXIL,
  Path(job['replacement']).read_bytes(),rd.ShaderCompileFlags(),rd.ShaderStage.Pixel)
 (out/'shader-build.txt').write_text(errors)
 if replacement==rd.ResourceId.Null():raise RuntimeError('Replacement shader failed: '+errors)
 controller.ReplaceResource(original,replacement)
 modified=[]
 for event in events:
  write('progress.json',dict(stage='modified',event=event));modified.append(sample(event))
 write('modified.json',modified)
 b={r['event']:r for r in baseline};m={r['event']:r for r in modified}
 first=job['selected_events'][0]
 assert b[first]['accepted_pixels']==0 and m[first]['accepted_pixels']>0
 assert m[first]['nonzero_motion_pixels']>0
 assert all(b[e]['scene_color_sha256']==m[e]['scene_color_sha256'] for e in events), 'Original scene color changed'
 assert b[first-3]==m[first-3], 'History changed before the selected lighting pass'
 controller.RemoveReplacement(original)
 restored=sample(first);assert restored==b[first], 'Baseline not restored'
 write('verification.json',dict(replay_succeeded=True,game_launched=False,
  selected_events=job['selected_events'],first_wipe_before=b[first]['accepted_pixels'],
  first_wipe_after=m[first]['accepted_pixels'],motion_pixels_after=m[first]['nonzero_motion_pixels'],
  final_accepted_pixels=m[last]['accepted_pixels'],original_scene_color_identical=True,baseline_restored=True,
  material_events=job.get('material_events',[]),
  scope=job.get('scope','actual saved GPU commands, capture-specific seven-pass fallback suppression; no production runtime or FPS claim')))
 code=0;write('progress.json',dict(stage='complete'))
except BaseException:
 (out/'error.txt').write_text(traceback.format_exc());write('progress.json',dict(stage='failed'))
finally:
 if controller:
  if replacement:
   controller.RemoveReplacement(rid(job['fallback_ps']));controller.FreeTargetResource(replacement)
  controller.Shutdown()
 if cap:cap.Shutdown()
 (out/'exit.txt').write_text(str(code));os._exit(code)
