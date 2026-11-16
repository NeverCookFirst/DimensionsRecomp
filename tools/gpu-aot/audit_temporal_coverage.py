"""Audit actual captured shader pairs; missing companions remain visible.

Optionally stage the corresponding saved Xbox containers for offline shader
translation. This never changes the game, source inventory or existing bank.
"""
import argparse, hashlib, json, re, shutil
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--bank',type=Path,required=True)
p.add_argument('--capture',type=Path,action='append',required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--inventory',type=Path)
p.add_argument('--prepare-corpus',type=Path)
p.add_argument('--require-complete',action='store_true')
a=p.parse_args()
available={(int(h,16),int(stage),int(spec)) for h,stage,spec in re.findall(
    r'\{0x([\dA-Fa-f]+)ull,(\d+),(\d+),\d+,\d+,\d+\}',
    (a.bank/'temporal-bank.cpp').read_text())}
pairs=set()
for capture in a.capture:
    text=capture.read_text(errors='replace')
    pairs.update((int(v,16),int(s,16)) for v,s in re.findall(
        r'\bVS=([\dA-Fa-f]+) PS=([\dA-Fa-f]+)',text))
assert pairs,'No captured shader pairs'
missing=set();rows=[]
for vs,ps in sorted(pairs):
    absent=[]
    if ps:
        for h,stage,specs in ((vs,0,range(2)),(ps,1,range(4))):
            if any((h,stage,spec) not in available for spec in specs):
                absent.append('vertex' if stage==0 else 'pixel');missing.add((h,stage))
    rows.append(dict(vs=f'{vs:016X}',ps=f'{ps:016X}',status='depth_only' if not ps else
                     'missing_companion' if absent else 'covered',missing=absent))
staged=[]
if a.prepare_corpus:
    assert a.inventory,'--prepare-corpus requires --inventory'
    a.prepare_corpus.mkdir(parents=True,exist_ok=True)
    sources=json.loads(a.inventory.read_text())['sources']
    for h,stage in sorted(missing):
        matches=[Path(row['source']) for row in sources if int(row['hash'],16)==h]
        source=next((path for path in matches if path.is_file()),None)
        assert source is not None,f'No saved container for {h:016X}'
        target=a.prepare_corpus/f'{h:016X}.bin'
        assert not target.exists(),f'Refusing to overwrite {target}'
        shutil.copy2(source,target)
        data=target.read_bytes()
        staged.append(dict(hash=f'{h:016X}',stage=stage,source=str(source),path=str(target),
                           bytes=len(data),sha256=hashlib.sha256(data).hexdigest()))
report=dict(pairs=rows,color_pairs=sum(bool(ps) for _,ps in pairs),
    covered_color_pairs=sum(row['status']=='covered' for row in rows),
    missing_programs=[dict(hash=f'{h:016X}',stage=stage) for h,stage in sorted(missing)],
    staged=staged,bank=str(a.bank),captures=[str(path) for path in a.capture])
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text(json.dumps(report,indent=2)+'\n')
print(f"Covered {report['covered_color_pairs']}/{report['color_pairs']} captured color pairs; "
      f'{len(missing)} programs lack complete companions')
if a.require_complete and missing:raise SystemExit(23)
