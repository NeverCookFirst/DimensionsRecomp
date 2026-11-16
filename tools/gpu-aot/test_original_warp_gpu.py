"""Reproduce the original captured draw before testing its production FSR graph."""
import argparse,hashlib,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path);p.add_argument('--inputs',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);inputs=a.inputs.resolve()
manifest=json.loads((inputs/'verification.json').read_text())
assert manifest['game_launched'] is False and manifest['replay_succeeded']
assert manifest['frame']==9869 and manifest['event']==17856
assert manifest['action']['numIndices']==1536 and manifest['action']['numInstances']==1
expected_shaders={'vs':'d4de48c66d9877d36612e1345d7b264b712d5d366316922df4217bd1f09c63b7',
 'ps':'969196d9445b993d282aece0916fe183a4ab69703670d546126cd24f763fa9df'}
checked=[]
for item in [*manifest['constants'],*manifest['shaders'].values(),manifest['post_vs'],*manifest['images'],manifest['vertex_buffers'][0]]:
 data=(inputs/item['file']).read_bytes();actual=hashlib.sha256(data).hexdigest()
 assert len(data)==item['bytes'] and actual==item['sha256'],item['file']
 checked.append({'file':item['file'],'sha256':actual})
for stage,sha in expected_shaders.items():assert manifest['shaders'][stage]['sha256']==sha
used=[x for x in manifest['vertex_inputs'] if x['used']]
assert len(used)==1 and used[0]['name']=='POSITION0' and used[0]['format']['compCount']==3
assert used[0]['format']['compByteWidth']==4 and used[0]['format']['compType']==1 and used[0]['vertexBuffer']==0 and used[0]['byteOffset']==0
assert manifest['vertex_buffers'][0]['stride']==36 and manifest['post_vs']['stride']==304
assert all(x['width']==1129 and x['height']==635 and x['format']=='R8G8B8A8_UNORM' for x in manifest['images'])
assert len(manifest['samplers'])==1
sampler=manifest['samplers'][0]['sampler']
assert sampler['addressU']==sampler['addressV']==0 and sampler['addressW']==3
assert sampler['filter']['magnify']==sampler['filter']['minify']==2 and sampler['filter']['mip']==1
assert sampler['minLOD']==0 and sampler['maxLOD']==.25 and sampler['mipBias']==0
source=Path(__file__).with_suffix('.cpp');exe=out/'warp.exe'
cmd=['clang-cl',str(source),'/std:c++latest','/O2','/MD','/EHsc','/UNDEBUG','/Fo'+str(out/'warp.obj'),'/Fe'+str(exe),'/link','d3d12.lib','dxgi.lib','dxguid.lib']
result=subprocess.run(cmd,capture_output=True,text=True,timeout=120);(out/'compile.log').write_text(result.stdout+result.stderr);result.check_returncode()
results=[]
for mutate in (False,True):
 result=subprocess.run([str(exe),str(inputs),*(['wrong-gain'] if mutate else [])],capture_output=True,text=True,timeout=60)
 name='wrong-gain' if mutate else 'captured-original';(out/(name+'.log')).write_text(result.stdout+result.stderr)
 assert result.returncode==(24 if mutate else 0),result.stdout+result.stderr
 results.append(dict(name=name,exit=result.returncode,stdout=result.stdout));print(result.stdout)
(out/'verification.json').write_text(json.dumps(dict(game_launched=False,window_opened=False,checked_inputs=checked,results=results,
 scope='original captured VS/PS, raw IA, exact CBVs, sampler/view and RGBA source/target; target initialized from pre-draw bytes; full-pixel comparison with independently captured post-draw output; one saved draw only, not FPS/full engine'),indent=2))
print('PASS original captured warp and negative pixel-gain control')
