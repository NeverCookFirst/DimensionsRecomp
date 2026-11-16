"""Audit scene-derived pixel programs and exact linked-bytecode aliases.

Profiles are explicit inspected programs, not a heuristic applied to arbitrary
shaders that happen to sample a large texture. No game or GPU is launched.
"""
import argparse,hashlib,json,re,zlib
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--hlsl',type=Path,required=True);p.add_argument('--bank',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--header',type=Path,default=Path(__file__).resolve().parents[2]/'rexlego/src/gpu_native/temporal_scene_pass.h')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
# These source programs derive lighting/fog from the existing scene buffers.
known=('081435C474061F7E','0E2FA249CEC37FF0','3DF8FF80E2A08043','745F43DE42185A41',
       '7DDDE95EB4B5A14D','82DB1B788F68E33B','C700ED5C1AAD294A','CEC8AAACB206BA39')
profiles=[]
for h in known:
    path=a.hlsl/('ps-'+h+'.hlsl');source=path.read_text()
    main=source[source.index('void main('):]
    reads=sorted(set(re.findall(r'tfetch\w*\(g_(\w+)_Texture2DDescriptorIndex',main)))
    slots={}
    for name in reads:
        m=re.search(r'#define g_'+re.escape(name)+r'_Texture2DDescriptorIndex vk::RawBufferLoad<uint>\(g_PushConstants.SharedConstants \+ (\d+)\)',source)
        if not m:raise RuntimeError('Missing binding for '+name)
        slots[name]=int(m[1])//4
    if slots.get('SceneDepthTex')!=0:raise RuntimeError('Unexpected scene depth slot')
    for name,expected in [('NormalsTex',2),('AlbedoTex',6)]:
        if name in slots and slots[name]!=expected:raise RuntimeError('Unexpected G-buffer slot')
    sampled_scene_mask=sum(1<<slots[name] for name in ('SceneDepthTex','NormalsTex','AlbedoTex') if name in slots)
    # Inspected081/FC2 volume lights use depth to reconstruct geometry. Their
    # albedo multiplier can legally be the captured4x4 constant-white image.
    mask=1 if h=='081435C474061F7E' else sampled_scene_mask
    profiles.append(dict(hash=h,source=str(path),source_sha256=hashlib.sha256(source.encode()).hexdigest(),
        sampled_roles=slots,sampled_scene_slots=sampled_scene_mask,required_scene_slots=mask))

blob=zlib.decompress((a.bank/'linked-bank.bin').read_bytes());variants={}
for h,spec,offset,size in re.findall(r'\{\s*0x([\dA-Fa-f]+),\s*0x([\dA-Fa-f]+),\s*(\d+),\s*(\d+)\s*\}',(a.bank/'linked-bank-metadata.cpp').read_text()):
    start,size=int(offset),int(size)
    if start+size>len(blob):raise RuntimeError('Bank range outside payload')
    variants.setdefault(h.upper().zfill(16),[]).append((int(spec,16),hashlib.sha256(blob[start:start+size]).hexdigest()))
families={}
for h,items in variants.items():families.setdefault(tuple(sorted(items)),[]).append(h)
for profile in profiles:
    h=profile['hash']
    if h not in variants:raise RuntimeError('Inspected program absent from linked bank: '+h)
    profile['variants']=[dict(specialization=s,sha256=sha) for s,sha in sorted(variants[h])]
    profile['exact_aliases']=sorted(families[tuple(sorted(variants[h]))])
    if set(s for s,_ in variants[h])!={0,2}:raise RuntimeError('Expected both PS alpha specializations')
header=a.header.read_text();pending=[];table={}
for token in re.finditer(r'case\s+0x([\dA-Fa-f]+)ull\s*:|return\s+(0x[\dA-Fa-f]+)\s*;',header):
    if token[1]:pending.append(token[1].upper().zfill(16))
    else:
        for h in pending:table[h]=int(token[2],16)
        pending=[]
expected={h:profile['required_scene_slots'] for profile in profiles for h in profile['exact_aliases']}
if pending or table!=expected:raise RuntimeError('Production table differs from inspected source/linked-byte aliases')
(a.output/'profiles.json').write_text(json.dumps(dict(game_launched=False,bank=str(a.bank),
    scope='source semantic/binding inspection and exact equality of every linked PS specialization',
    production_header=str(a.header),production_header_sha256=hashlib.sha256(header.encode()).hexdigest(),
    production_table_verified=True,profiles=profiles),indent=2)+'\n')
print('PASS',len(profiles),'inspected scene-derived programs;',sum(len(p['exact_aliases']) for p in profiles),'program/alias associations')
for row in profiles:print(row['hash'],hex(row['required_scene_slots']),','.join(row['exact_aliases']))
