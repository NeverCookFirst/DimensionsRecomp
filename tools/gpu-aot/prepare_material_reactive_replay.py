"""Prepare a conservative saved-frame material-mask counterfactual.

All unknown draws retain full rejection. Exact signatures identify only audited
lighting and65 additive UI draws. UI contribution rounding to zero discards;
all other UI pixels retain full rejection. Production instead preserves motion
and MAX-combines the actual fractional contribution. This diagnostic is stricter
than production and is not a claim to replay the new PSO blend configuration.
"""
import argparse,json,re,runpy,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--inventory',type=Path,required=True);p.add_argument('--lighting-job',type=Path,required=True)
p.add_argument('--expanded',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
p.add_argument('--dxc',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
split=runpy.run_path(str(Path(__file__).with_name('prepare_temporal_motion.py')))['split_entry']
rows=json.loads((a.inventory/'draws.json').read_text());job=json.loads(a.lighting_job.read_text())
selected=[r for r in rows if r['ps'].get('sha256')=='e3c0dacbe477050c15c7a47f14f7fe1dbf8f2a9e3b0dc105b290b049de6f1239' and not r['depth_enable']]
assert len(selected)==65
patterns=set(tuple(r['texture_indices']) for r in selected)
assert [r['replay'] for r in rows if tuple(r['texture_indices']) in patterns]==[r['replay'] for r in selected]
assert all(r['color_source']=='BlendMultiplier.SrcAlpha' and r['color_destination']=='BlendMultiplier.One' and r['color_operation']=='BlendOperation.Add' for r in selected)
source=a.expanded.read_text();source=re.sub(r'^\s*#(?:line\b.*|\s*\d+.*)$','',source,flags=re.M)
prefix,params,body=split(source)
slots={int(register)*4+'xyzw'.index(component):name for name,register,component in re.findall(r'uint\s+(\w+)\s*:\s*packoffset\(c(\d+)\.([xyzw])\)',prefix) if int(register)<8}
assert set(slots)==set(range(32))
def condition(pattern):return '('+' && '.join(f'{slots[i]}=={value}u' for i,value in enumerate(pattern))+')'
lighting=sorted(set(tuple(v) for v in job['selected_bindings'].values()))
signature=','.join(p[4] for p in params)
original='void LegoMaterial('+signature+')\n'+body
outputs=[p for p in params if p[0]=='out'];assert all(p[3].upper().startswith('SV_TARGET') for p in outputs)
wrapper=','.join([p[4] for p in params if p[0]=='in']+['out float2 velocity:SV_Target0','out float reactive:SV_Target1'])
locals_='\n'.join(p[1]+' material_'+p[2]+';' for p in outputs)
args=','.join(p[2] if p[0]=='in' else 'material_'+p[2] for p in params)
text=prefix+original+'\nvoid main('+wrapper+') {\nif ('+' || '.join(condition(p) for p in lighting)+') discard;\n'
text+='if ('+' || '.join(condition(p) for p in patterns)+') {\n'+locals_+'\nLegoMaterial('+args+');\n'
text+='float4 color=material_oC0;float contribution=max(abs(color.r),max(abs(color.g),abs(color.b)))*abs(color.a);\n'
text+='if (all(isfinite(color)) && contribution < (0.5/255.0)) discard;\n}\nvelocity=0;reactive=1;\n}\n'
path=a.output/'conservative-material-fallback.hlsl';binary=a.output/'conservative-material-fallback.dxil';path.write_text(text)
subprocess.run([str(a.dxc),str(path),'-T','ps_6_0','-E','main','-HV','2021','-Wno-ignored-attributes','-Fo',str(binary)],check=True)
job.update(output=str(a.output.resolve()),replacement=str(binary.resolve()),material_events=[r['replay'] for r in selected],
 scope='saved GPU frame: seven exact lighting suppressions and65 additive UI material evaluations; full rejection retained for any UI contribution rounding nonzero; stricter than production fractional MAX mask, not production PSO replay')
(a.output/'job.json').write_text(json.dumps(job,indent=2)+'\n')
(a.output/'selection.json').write_text(json.dumps(dict(game_launched=False,lighting_replays=7,ui_replays=len(selected),unknown_replays_retained=len(rows)-7-len(selected),
 exact_ui_bindings=len(patterns),ui_events=job['material_events'],scope=job['scope']),indent=2)+'\n')
print('PASS conservative material capture selection;65 additive UI replays,480 unknown replays retained')
