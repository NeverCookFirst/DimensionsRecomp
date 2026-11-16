"""Prepare an exact saved-frame counterfactual; no game launch.

The fallback shader is shared by unknown geometry. Match all 32 original texture
indices, and prove those signatures select only the seven audited lighting/fog
events across the complete inventory before allowing a replacement replay.
This is capture-specific diagnostic code, not the production classification.
"""
import argparse,hashlib,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--inventory',type=Path,required=True);p.add_argument('--audit',type=Path,required=True)
p.add_argument('--dxc',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
rows=json.loads((a.inventory/'draws.json').read_text());profiles=json.loads(a.audit.read_text())['profiles']
known={v['sha256']:x for x in profiles for v in x['variants']}
selected=[r for r in rows if r['ps'].get('sha256') in known]
assert len(selected)==7 and sorted(r['replay'] for r in selected)==[16298,16339,16380,16421,16462,16503,16524]
for row in selected:
 assert not row['depth_enable']
 required=known[row['ps']['sha256']]['required_scene_slots'];reads={r['index']:r['resource'] for r in row['reads']} if 'reads' in row else None
 if reads:
  target=row['targets'][0]
  for slot in range(32):
   if required&(1<<slot):
    texture=reads[row['texture_indices'][slot]]
    assert (texture['width'],texture['height'])==(target['width'],target['height'])
patterns=sorted(set(tuple(r['texture_indices']) for r in selected))
matches=[r for r in rows if tuple(r['texture_indices']) in patterns]
assert [r['replay'] for r in matches]==[r['replay'] for r in selected], 'Would suppress unknown geometry'
conditions=[]
for pattern in patterns:
 conditions.append('('+' && '.join(f'all(textures[{i}] == uint4('+','.join(str(n)+'u' for n in pattern[i*4:i*4+4])+'))' for i in range(8))+')')
source='cbuffer Shared : register(b2, space4) {uint4 textures[8];};\nvoid main(out float2 velocity:SV_Target0,out float reactive:SV_Target1) {\nif ('+' ||\n'.join(conditions)+') discard;\nvelocity=0;reactive=1;\n}\n'
hlsl=a.output/'selective-fallback.hlsl';dxil=a.output/'selective-fallback.dxil';hlsl.write_text(source)
subprocess.run([str(a.dxc),str(hlsl),'-T','ps_6_0','-E','main','-Fo',str(dxil)],check=True)
original=json.loads((a.inventory/'job.json').read_text())
job=dict(capture=original['capture'],output=str(a.output.resolve()),fallback_ps=original['fallback_ps'],
 replacement=str(dxil.resolve()),selected_events=[r['replay'] for r in selected],
 selected_bindings={str(r['replay']):r['texture_indices'] for r in selected},
 color_target=selected[0]['targets'][0]['id'])
(a.output/'job.json').write_text(json.dumps(job,indent=2)+'\n')
(a.output/'selection.json').write_text(json.dumps(dict(game_launched=False,inventory_draws=len(rows),
 selected_events=job['selected_events'],unknown_replays_retained=len(rows)-len(selected),
 distinct_exact_bindings=len(patterns),replacement_sha256=hashlib.sha256(dxil.read_bytes()).hexdigest(),
 scope='capture-specific counterfactual only; production uses verified shader identity, active input extents and scene viewport'),indent=2)+'\n')
print('PASS exact capture selection:',len(selected),'lighting/fog replays;',len(rows)-len(selected),'unknown replays retained')
