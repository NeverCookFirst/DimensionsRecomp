"""Preserve original material coverage and estimate its visible RGB contribution.

These PS variants use the original VS signature. They never create motion;
the pipeline preserves target0 and MAX-combines rejection in target1.
"""
import re,runpy
from pathlib import Path
split_entry=runpy.run_path(str(Path(__file__).with_name('prepare_temporal_motion.py')))['split_entry']

MODES={'opaque':2,'alpha':3,'add_alpha':4,'add':5,'premultiplied':6}

def build(source,mode):
    if mode not in MODES:raise ValueError('Unsupported blend contribution')
    source=re.sub(r'^\s*#(?:line\b.*|\s*\d+.*)$','',source,flags=re.M)
    prefix,params,body=split_entry(source)
    outputs=[p for p in params if p[0]=='out']
    color=next((p for p in outputs if p[3].upper()=='SV_TARGET0'),None)
    if color is None or color[1]!='float4':raise ValueError('Expected float4 material color0')
    if any(not(re.fullmatch(r'SV_TARGET[0-3]',p[3].upper()) or p[3].upper()=='SV_DEPTH') for p in outputs):
        raise ValueError('Unsupported material output')
    signature=',\n'.join(p[4] for p in params)
    original='void LegoMaterial('+signature+')\n'+body
    locals_='\n'.join(p[1]+' material_'+p[2]+';' for p in outputs if p[3].upper()!='SV_DEPTH')
    args=','.join(p[2] if p[0]=='in' or p[3].upper()=='SV_DEPTH' else 'material_'+p[2] for p in params)
    wrapper=[p[4] for p in params if p[0]=='in']
    wrapper+=['out float2 oVelocity:SV_Target0','out float oReactive:SV_Target1']
    wrapper+=[p[4] for p in outputs if p[3].upper()=='SV_DEPTH']
    contribution={'opaque':'1.0','alpha':'saturate(color.a)','add_alpha':'saturate(energy * abs(color.a))',
                  'add':'saturate(energy)','premultiplied':'saturate(max(energy, abs(color.a)))'}[mode]
    return prefix+original+'\nvoid main('+',\n'.join(wrapper)+') {\n'+locals_+'\nLegoMaterial('+args+');\n'+\
        'float4 color=material_'+color[2]+';\nfloat energy=max(abs(color.r), max(abs(color.g), abs(color.b)));\n'+\
        'oVelocity=0.0;\noReactive=all(isfinite(color)) ? '+contribution+' : 1.0;\n}\n'
