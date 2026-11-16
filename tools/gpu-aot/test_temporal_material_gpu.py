"""Compare real material PS coverage to exact companion bytes from a saved bank.

Windowless controlled textures/constants and a signature-matched generated VS
exercise all alpha functions, both history-valid states and four PS variants.
This does not replay the game's scene geometry, texture assets, or camera path.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import runpy
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path)
p.add_argument('--bank',type=Path,required=True)
p.add_argument('--expanded',type=Path,required=True,help='Directory containing HASH-ps-SPEC-expanded.hlsl')
p.add_argument('--hash',action='append',required=True)
p.add_argument('--dxc',type=Path,required=True)
p.add_argument('--compiler',default='clang-cl')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
(a.output/'verification.json').unlink(missing_ok=True)
scripts=Path(__file__).resolve().parent
generator=runpy.run_path(str(scripts/'prepare_temporal_motion.py'))
bank=(a.bank/'temporal-bank.bin').read_bytes()
entries={ (int(h,16),int(stage),int(spec)):(int(offset),int(size))
 for h,stage,spec,offset,size,_ in re.findall(r'\{0x([0-9A-Fa-f]+)ull,(\d+),(\d+),(\d+),(\d+),(\d+)\}',(a.bank/'temporal-bank.cpp').read_text()) }
def run(args,timeout=120):
    result=subprocess.run([str(x) for x in args],capture_output=True,text=True,timeout=timeout)
    if result.returncode:raise RuntimeError(' '.join(map(str,args))+':\n'+result.stdout+result.stderr)
    return result
def compile_(source,name,stage):
    path=a.output/(name+'.hlsl');path.write_text(source)
    binary=a.output/(name+'.dxil')
    run([a.dxc,path,'-T',stage+'_6_0','-E','main','-HV','2021','-Wno-ignored-attributes','-Fo',binary])
    return binary
exe=a.output/'test-temporal-material.exe'
run([a.compiler,scripts/'test_temporal_material_gpu.cpp','/std:c++20','/O2','/MD','/EHsc',
     '/Fo'+str(a.output/'fixture.obj'),'/Fe'+str(exe),'/link','d3d12.lib','dxgi.lib','dxguid.lib'])
results=[];negative=[]
for hash_text in a.hash:
    hash_value=int(hash_text,16)
    for spec in range(4):
        stem=f'{hash_value:016X}-{spec}'
        source=(a.expanded/f'{hash_value:016X}-ps-{spec}-expanded.hlsl').read_text()
        clean=re.sub(r'^\s*#(?:line\b.*|\s*\d+.*)$','',source,flags=re.M)
        prefix,params,body=generator['split_entry'](clean)
        inputs=[v for v in params if v[0]=='in' and not v[3].upper().startswith('SV_')]
        # Emit every physical interpolator, including otherwise unused ones,
        # preserving the original translator's signature register ordering.
        outputs=['out float4 position:SV_Position']+[f'out {v[1]} v{i}:{v[3]}' for i,v in enumerate(inputs)]
        outputs+=['out float4 currentClip:LEGOCURRENTCLIP0','out float4 previousClip:LEGOPREVIOUSCLIP0']
        vs='cbuffer Fixture:register(b0,space4){float valid;}\nvoid main(uint id:SV_VertexID,'+','.join(outputs)+'){\n'
        vs+='float2 xy=id==0?float2(-1,-1):id==1?float2(3,-1):float2(-1,3);position=float4(xy,.5,1);\n'
        for i,v in enumerate(inputs):
            assert v[1]=='float4',v
            vs+=f'v{i}=float4(xy*.25+.5,{.2+i*.01},1);\n'
        vs+='currentClip=position;previousClip=valid>0?position+float4(.25,-.125,0,0):0;}'
        vertex=compile_(vs,stem+'-vs','vs')
        original=compile_(clean,stem+'-original','ps')
        offset,size=entries[hash_value,1,spec]
        assert size>0 and offset+size<=len(bank),'Truncated companion bank entry'
        companion=a.output/(stem+'-bank.dxil');companion.write_bytes(bank[offset:offset+size])
        result=run([exe,vertex,original,companion,int(bool(spec&2))],timeout=120)
        print(stem+': '+result.stdout.strip(),flush=True)
        results.append({'hash':hash_text,'specialization':spec,'stdout':result.stdout,
            'companion_sha256':hashlib.sha256(companion.read_bytes()).hexdigest(),'exit_code':result.returncode})
        if spec==2:
            modified=generator['build'](clean,'ps')
            assert 'if (!LegoAlphaPass(oC0.w, g_AlphaThreshold, g_AlphaFunction)) discard;' in modified
            modified=modified.replace('if (!LegoAlphaPass(oC0.w, g_AlphaThreshold, g_AlphaFunction)) discard;','')
            mutant=compile_(modified,stem+'-removed-discard','ps')
            rejected=subprocess.run([str(exe),str(vertex),str(original),str(mutant),'1'],capture_output=True,text=True,timeout=120)
            assert rejected.returncode==23,(rejected.returncode,rejected.stdout,rejected.stderr)
            negative.append({'hash':hash_text,'exit_code':rejected.returncode,'stdout':rejected.stdout,'stderr':rejected.stderr})
report={'game_launched':False,'window_opened':False,'exact_bank_bytes':True,
        'scope':'controlled textures and constants; all alpha functions, thresholds and history validity; not game scene replay',
        'results':results,'removed_discard_negatives':negative,'passed':True}
(a.output/'verification.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS actual material coverage/depth and bank companion motion/reactivity; discard mutations rejected')
