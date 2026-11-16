"""Actual texture metadata consumers inside real SDK physical-provider faults.

No game, OS window or GPU. D3D header ABI, SDK residency/protection/faults and
native function bodies execute. Host texture/descriptor/depth-alias boundaries
and resource structure storage are injected; fake textures contain no pixels.
"""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess
p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);p.add_argument('--source',type=Path,required=True);p.add_argument('--previous-source',type=Path,required=True);p.add_argument('--runtime-dir',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[2];sdk=root.parent/'dev-assets/sdk-install';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
def block(text,signature):
 start=text.index(signature);end=text.index('{',start)+1;depth=1
 while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
abi=(root/'rexlego/src/gpu_native/d3d.h').read_text()
(out/'header-abi.inc').write_text('\n'.join(block(abi,s)+';' for s in ['struct FetchConstant {','struct D3DResource {','enum class D3DResourceType :','struct D3DTexture {'])+'\nstatic_assert(sizeof(D3DTexture)==52&&offsetof(D3DTexture,format)==28);\n')
signatures=['std::shared_ptr<TextureResource> FindTexture(', 'bool TexturePlacementHeaderMatches(',
 'bool RefreshTextureResourceHeader(u32 guest_address,\n    const std::shared_ptr<TextureResource>& resource) {',
 'std::shared_ptr<void> RefreshTextureHeaderOwner(', 'void RefreshTextureHeader(', 'std::shared_ptr<TextureResource> FindTextureAtUse(',
 'std::shared_ptr<TextureResource> AdoptTexture(', 'TexturePreparationSnapshot CaptureTexturePreparation(u32 address, std::shared_ptr<void> resolved) {']
for name in ['rexruntime.dll','amd_fidelityfx_dx12.dll']:shutil.copy2(a.runtime_dir/name,out/name)
env=dict(os.environ,LEGO_TEST_RUNTIME_EXPECTED=str(out/'rexruntime.dll'))
records=[]
for label,path in [('production',a.source),('previous',a.previous_source)]:
 source=path.read_text();bodies=[block(source,s) for s in signatures]
 (out/'metadata-production.inc').write_text('\n'.join(bodies))
 exe=out/(label+'.exe');fixture=Path(__file__).with_suffix('.cpp')
 r=subprocess.run(['clang-cl',str(fixture),'/std:c++latest','/O2','/MD','/EHsc','/D_CRT_SECURE_NO_WARNINGS',
  '/I'+str(out),'/I'+str(root/'rexlego/src'),'/I'+str(sdk/'include'),'/I'+str(root/'rexglue-sdk/include'),
  '/Fo'+str(out)+'/', '/Fe'+str(exe),'/link',str(sdk/'lib/rexruntime.lib'),'kernel32.lib','user32.lib'],capture_output=True,text=True,timeout=90)
 (out/(label+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
 for mode in range(6):
  # Placement was already corrected in V21; old refresh/capture/adopt were not.
  if label=='previous' and mode==0:continue
  for legacy in ([False,True] if mode==3 else [False]):
   name=f'{label}-{mode}'+('-legacy' if legacy else '')
   r=subprocess.run([str(exe),str(mode)],cwd=out,env=dict(env,LEGO_NATIVE_REUSE_TEXTURE_LOOKUP='0' if legacy else '1'),capture_output=True,text=True,timeout=30)
   (out/(name+'.log')).write_text(r.stdout+r.stderr)
   expected=0 if label=='production' else 23
   assert r.returncode==expected,(name,r.returncode,r.stdout,r.stderr)
   if expected:assert 'REENTERED_PROVIDER_DURING_TEXTURE_METADATA_REFRESH' in r.stderr,(name,r.stderr)
   record=dict(name=name,exit=r.returncode,output=r.stdout,error=r.stderr);records.append(record);print(r.stdout or r.stderr,end='',flush=True)
(out/'metadata-production.inc').write_text('\n'.join(block(a.source.read_text(),s) for s in signatures))
(out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,gpu_tested=False,actual_sdk_memory=True,actual_provider_faults=True,
 header_abi_bytes=52,records=records,sources={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [a.source,Path(__file__),fixture,root/'rexlego/src/gpu_native/guest_metadata.h',root/'rexlego/src/gpu_native/d3d.h']},
 previous_source=dict(path=str(a.previous_source),sha256=hashlib.sha256(a.previous_source.read_bytes()).hexdigest()),
 runtime_sha256=hashlib.sha256((a.runtime_dir/'rexruntime.dll').read_bytes()).hexdigest(),functions=signatures,
 scope=__doc__,limits=['No native renderer provider is registered; controlled fixture payload is delivered. No recorder/GPU execution, ownership ordering or memexport correctness is proved.',
 'Resource storage, host allocation/descriptor retirement, pinned lookup and depth aliases are injected. Tests use unchanged placement; relocation/replacement is covered by the separate use-time CPU fixture.']),indent=2))
