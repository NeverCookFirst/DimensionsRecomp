"""Check placement metadata against real SDK provider-protected pages."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

def body(text,signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('output',type=Path)
parser.add_argument('--runtime-dir',required=True,type=Path)
parser.add_argument('--previous-source',required=True,type=Path)
parser.add_argument('--compiler',default='clang-cl')
a=parser.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2];sdk=root.parent/'dev-assets/sdk-install'
abi=(root/'rexlego/src/gpu_native/d3d.h').read_text();text=(root/'rexlego/src/gpu_native/textures.cpp').read_text()
(out/'header-abi.inc').write_text('\n'.join(body(abi,s)+';' for s in ['struct FetchConstant {','struct D3DResource {','struct D3DTexture {'])+'\nstatic_assert(sizeof(D3DTexture)==52 && offsetof(D3DTexture,format)==28);\n')
env=dict(os.environ,PATH=str(a.runtime_dir.resolve())+os.pathsep+os.environ['PATH'],LEGO_TEST_RUNTIME_EXPECTED=str(a.runtime_dir.resolve()/'rexruntime.dll'))
records=[]
for negative in [False,True]:
    source=a.previous_source.read_text() if negative else text
    code=body(source,'bool ReadableUploadSpan(')+'\n'+body(source,'bool TexturePlacementHeaderMatches(')
    (out/'placement-production.inc').write_text(code)
    name='old-alias-header' if negative else 'protected-header';exe=out/(name+'.exe')
    p=subprocess.run([a.compiler,str(Path(__file__).with_suffix('.cpp')),'/std:c++latest','/O2','/MD','/EHsc','/D_CRT_SECURE_NO_WARNINGS',
        '/I'+str(out),'/I'+str(root/'rexlego/src'),'/I'+str(sdk/'include'),'/I'+str(root/'rexglue-sdk/include'),
        '/Fo'+str(out)+'/', '/Fe'+str(exe),'/link',str(sdk/'lib/rexruntime.lib'),'kernel32.lib','user32.lib'],capture_output=True,text=True,timeout=90)
    (out/(name+'-compile.log')).write_text(p.stdout+p.stderr)
    if p.returncode:raise RuntimeError(p.stdout+p.stderr)
    p=subprocess.run([str(exe)],env=env,capture_output=True,text=True,timeout=30)
    (out/(name+'.log')).write_text(p.stdout+p.stderr)
    expected=23 if negative else 0
    if p.returncode!=expected:raise RuntimeError(f'exit {p.returncode}: '+p.stdout+p.stderr)
    if negative:assert 'REENTERED_PROVIDER_DURING_PLACEMENT_HEADER_READ' in p.stderr,p.stderr
    records.append(dict(negative=negative,exit_code=p.returncode,stdout=p.stdout,stderr=p.stderr))
    print(('EXPECTED OLD REENTRY ' if negative else '')+p.stdout.strip()+p.stderr.strip())
(out/'placement-production.inc').write_text(body(text,'bool ReadableUploadSpan(')+'\n'+body(text,'bool TexturePlacementHeaderMatches('))
(out/'verification.json').write_text(json.dumps(dict(game_launched=False,actual_sdk_memory=True,actual_provider_faults=True,actual_header_abi=True,
    actual_placement_validation=True,gpu_tested=False,records=records,
    production_source_sha256=hashlib.sha256(text.encode()).hexdigest(),
    runtime_sha256=hashlib.sha256((a.runtime_dir/'rexruntime.dll').read_bytes()).hexdigest(),
    scope='CPU metadata validation during a real SDK data-provider callback; controlled payload publication and recursive-entry guard, no native renderer registration or worker/GPU execution'),indent=2)+'\n')
