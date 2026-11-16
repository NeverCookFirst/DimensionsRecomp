"""Index exact executed DXIL for mask-zero shaders omitted by linked banks.

Uses the strict numeric-array reader and Python3.14's Zstandard decoder. No
compiler, GPU, game or generated C++ execution is involved.
"""
import argparse, hashlib, io, json, re, runpy
from compression import zstd
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--cache',type=Path,action='append',required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
reader=runpy.run_path(str(Path(__file__).with_name('embed_linked_bank.py')))
identities={};proof=[]
for cache in a.cache:
    rows=[];sink=io.BytesIO()
    with cache.open('rb') as source:
        lines=reader['Lines'](source)
        while True:
            raw,line=lines.next()
            if line==b'const uint8_t g_compressedDxilCache[] = {':break
            match=re.fullmatch(rb'\{\s*0x([\dA-Fa-f]+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\},',line)
            if match:rows.append(tuple(int(match[i],16 if i==1 else 10) for i in range(1,7)))
        compressed_bytes,compressed_sha=reader['numeric'](lines,sink,b'};')
        footer=source.read(4096)
    declared=re.search(rb'g_dxilCacheDecompressedSize\s*=\s*(\d+)',footer)
    if not rows or not declared:raise ValueError('Missing metadata/size: '+str(cache))
    data=zstd.decompress(sink.getvalue())
    if len(data)!=int(declared[1]):raise ValueError('Incorrect decoded size')
    indexed=0
    for h,offset,size,_,_,mask in rows:
        if mask:continue
        if not size or offset+size>len(data):raise ValueError('Invalid shader range')
        code=data[offset:offset+size]
        if code[:4]!=b'DXBC':raise ValueError('Invalid unspecialized container')
        digest=hashlib.sha256(code).hexdigest()
        identities.setdefault(digest,[]).append(dict(hash=f'{h:016X}',spec=0,base_cache=str(cache.resolve())))
        indexed+=1
    proof.append(dict(cache=str(cache.resolve()),entries=len(rows),unspecialized=indexed,compressed_bytes=compressed_bytes,compressed_sha256=compressed_sha,decompressed_bytes=len(data)))
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_text(json.dumps(dict(identities=identities,archives=proof,game_launched=False),indent=2)+'\n')
print('Indexed',sum(p['unspecialized'] for p in proof),'unspecialized programs from',len(proof),'exact base archives')
