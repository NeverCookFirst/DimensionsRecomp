"""Real VS+PS readbacks through GRAPHICS barriers, including detached replay.

Compare the corrected Plume library with a preserved library. Missing debug
layers are reported; device/readback/state checks still run. Invalid old read
states are rejected before GPU submission, not submitted as a crash experiment.
"""
from pathlib import Path
import argparse,subprocess,json,os,hashlib
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
 p.add_argument('--plume-lib',required=True,type=Path);p.add_argument('--baseline-plume-lib',required=True,type=Path)
 p.add_argument('--dxc',required=True,type=Path);p.add_argument('--baseline-only',action='store_true');a=p.parse_args()
 root=Path(__file__).resolve().parents[2];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
 original=Path(__file__).with_name('test_vertex_texture_read_gpu.cpp');text=original.read_text()
 def change(old,new):
  nonlocal text
  if text.count(old)!=1:raise ValueError('Fixture structure changed: '+old)
  text=text.replace(old,new)
 change('const bool missing_barrier=argc==4;','const bool explicit_helper=argc==4;')
 change('CHECK(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))));\n  debug->EnableDebugLayer();debug->Release();',
  'const bool debug_available=SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));\n'
  '  if(debug_available){debug->EnableDebugLayer();debug->Release();}\n'
  '  std::printf("debug_layer_available=%u\\n",unsigned(debug_available));')
 change('if(!missing_barrier)PrepareVertexTextureShaderRead(record,source.get());',
        'if(explicit_helper)PrepareVertexTextureShaderRead(record,source.get());')
 change('ID3D12InfoQueue* messages=nullptr;', 'if(debug_available){ID3D12InfoQueue* messages=nullptr;')
 change('messages->Release();CHECK(errors==0);CHECK(raw->GetDeviceRemovedReason()==S_OK);',
        'messages->Release();CHECK(errors==0);}\n  CHECK(raw->GetDeviceRemovedReason()==S_OK);')
 change('zero D3D12 errors\\n",checks);', 'debug_layer_available=%u\\n",checks,unsigned(debug_available));')
 source=out/'graphics-read-gpu.cpp';source.write_text(text)
 shaders={
  'vs':'Texture2D<float4> t:register(t0); void main(uint id:SV_VertexID,out float4 p:SV_Position,out float4 c:TEXCOORD0){p=float4(id==1?3:-1,id==2?3:-1,0,1);c=t.Load(int3(0,0,0));}',
  'ps':'Texture2D<float4> t:register(t0); float4 main(float4 p:SV_Position,float4 c:TEXCOORD0):SV_Target{return .5*c+.5*t.Load(int3(0,0,0));}'}
 for stage,code in shaders.items():
  src=out/(stage+'.hlsl');src.write_text(code)
  subprocess.run([str(a.dxc),str(src),'-T',stage+'_6_0','-E','main','-Fo',str(out/(stage+'.dxil'))],check=True,timeout=30)
 records=[]
 libraries=[('baseline',a.baseline_plume_lib)]+([] if a.baseline_only else [('current',a.plume_lib)])
 for name,lib in libraries:
  exe=out/(name+'.exe')
  cmd=['clang-cl',str(source),str(root/'rexlego/src/gpu_native/native_recording.cpp'),'/std:c++20','/O2','/MD','/EHsc','/UNDEBUG',
   '/I'+str(root/'rexlego/src'),'/I'+str(root/'thirdparty/plume'),'/I'+str(root/'thirdparty/plume/contrib/D3D12MemoryAllocator/include'),
   '/Fo'+out.as_posix()+'/', '/Fe'+str(exe),'/link',str(lib.resolve()),'d3d12.lib','dxgi.lib','dxguid.lib','user32.lib']
  r=subprocess.run(cmd,capture_output=True,text=True,timeout=90);(out/(name+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
  cases=[('1',False),('1',True)] if name=='baseline' else [('0',False),('1',False)]
  for cache,helper in cases:
   label=name+'-cache'+cache+('-helper' if helper else '-graphics')
   r=subprocess.run([str(exe),str(out/'vs.dxil'),str(out/'ps.dxil')]+(['explicit-helper'] if helper else []),
    env=dict(os.environ,PLUME_D3D12_STATE_CACHE=cache),capture_output=True,text=True,timeout=90)
   (out/(label+'.log')).write_text(r.stdout+r.stderr)
   negative=name=='baseline' and not helper;expected=23 if negative else 0
   assert r.returncode==expected,(label,r.returncode,r.stdout,r.stderr)
   if negative:assert 'source_native->resourceStates' in r.stderr
   else:assert 'PASS 64 GPU readbacks' in r.stdout
   records.append(dict(case=label,exit=r.returncode,expected_exit=expected,actual_gpu_readbacks=0 if negative else 64,
    debug_layer_available='debug_layer_available=1' in r.stdout,library_sha256=hashlib.sha256(lib.read_bytes()).hexdigest(),
    output=r.stdout+r.stderr))
   print(label,'exit',r.returncode,flush=True)
 (out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,window_opened=False,
  actual_gpu=True,source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),baseline_fixture_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),
  records=records,scope='Readback correctness and legal VS+PS states using production Plume/native replay. No gameplay visual or performance proof.'),indent=2))
if __name__=='__main__':main()
