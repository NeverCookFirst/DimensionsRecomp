"""Append declaration-correct material-input variants to an existing bank.

Retains every existing payload, including closed position-fix families.
Integer declarations continue to use the original shaders at runtime.
"""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import argparse,hashlib,json,re,subprocess,runpy

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
 p.add_argument('--base',type=Path,required=True);p.add_argument('--sources',type=Path,required=True)
 p.add_argument('--dxc',type=Path,required=True);p.add_argument('--llvm-mc',type=Path,required=True)
 a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
 if out==a.base.resolve():raise ValueError('Refusing to overwrite the base bank')
 blob=(a.base/'float-blend-indices-bank.bin').read_bytes();entries={}
 for h,s,o,n,k in re.findall(r'\{0x([A-Fa-f0-9]+)ull,(\d+),(\d+),(\d+),(\d+)\}',(a.base/'float-blend-indices-bank.cpp').read_text()):
  key=int(h,16),int(s),int(k);o,n=int(o),int(n)
  if key in entries or o+n>len(blob) or blob[o:o+4]!=b'DXBC':raise ValueError('Invalid base entry')
  entries[key]=o,n
 if not entries:raise ValueError('Empty base bank')
 original=dict(entries);jobs=[];existing=set();seen=set()
 float_input=runpy.run_path(str(Path(__file__).with_name('build_float_blend_indices_bank.py')))['float_input']
 for path in sorted(a.sources.glob('*.hlsl')):
  text=path.read_text()
  if not re.search(r'\bin\s+uint4\s+iBlendIndices0\s*:\s*BLENDINDICES0\b',text):continue
  m=re.search(r'// vertex shader\s+hash=0x([A-Fa-f0-9]+)',text[:300])
  if not m or 'packoffset(c38)' not in text:raise ValueError('Expected ABI624 native source: '+str(path))
  h=int(m[1],16)
  if h in seen:raise ValueError('Duplicate source hash')
  seen.add(h)
  if all((h,s,0) in entries for s in range(2)):existing.add(h);continue
  if any((h,s,1) in entries for s in range(2)):
   raise ValueError('A position-corrected source must be explicitly composed before adding its float variant')
  if text.count('uint g_SpecConstants();')==1:
   text=text.replace('uint g_SpecConstants();','uint g_SpecConstants() { return 0; }')
  if text.count('uint g_SpecConstants() { return 0; }')!=1:raise ValueError('Unexpected specialization')
  text=float_input(text)
  for spec in range(2):
   if (h,spec,0) not in entries:jobs.append((h,spec,path,text))
 def one(job):
  h,spec,path,text=job;text=text.replace('uint g_SpecConstants() { return 0; }',f'uint g_SpecConstants() {{ return {spec}; }}')
  stem=f'{h:016X}-vs-{spec}-float';src=out/(stem+'.hlsl');binary=out/(stem+'.dxil');src.write_text(text)
  r=subprocess.run([str(a.dxc),str(src),'-T','vs_6_0','-E','main','-DREBLUE_RECOMP','-DLEGO_NATIVE_GPU',
   '-HV','2021','-Wno-ignored-attributes','-Fo',str(binary)],capture_output=True,text=True,timeout=120)
  (out/(stem+'.log')).write_text(r.stdout+r.stderr);r.check_returncode()
  data=binary.read_bytes();assert data[:4]==b'DXBC'
  dis=subprocess.run([str(a.dxc),'-dumpbin',str(binary)],check=True,capture_output=True,text=True,timeout=30).stdout
  if not re.search(r'^; BLENDINDICES\s+0\s+\w+\s+\d+\s+NONE\s+float',dis,re.M):
   raise ValueError('Wrong BLENDINDICES type after compilation: '+stem)
  return h,spec,data,dict(hash=f'{h:016X}',spec=spec,kind=0,source=str(path.resolve()),
   source_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),sha256=hashlib.sha256(data).hexdigest(),policy='float-material-input')
 with ThreadPoolExecutor(max_workers=6) as pool:results=sorted(pool.map(one,jobs))
 combined=bytearray(blob);records=[]
 for h,spec,data,record in results:
  while len(combined)%16:combined.append(0)
  entries[h,spec,0]=len(combined),len(data);combined.extend(data);records.append(record)
 assert bytes(combined[:len(blob)])==blob and all(entries[k]==v for k,v in original.items())
 bank=out/'float-blend-indices-bank.bin';bank.write_bytes(combined)
 rows=[f'{{0x{h:016X}ull,{s},{o},{n},{k}}}' for (h,s,k),(o,n) in sorted(entries.items())]
 (out/'float-blend-indices-bank.cpp').write_text('const FloatBlendIndicesEntry g_float_blend_indices_entries[] = {\n'+',\n'.join(rows)+'\n};\n')
 asm=out/'float-blend-indices-bank.s';asm.write_text('.section .rdata,"dr"\n.p2align 4\n.globl g_floatBlendIndicesBlob\ng_floatBlendIndicesBlob:\n.incbin "'+bank.as_posix()+'"\n')
 subprocess.run([str(a.llvm_mc),'-triple=x86_64-pc-windows-msvc','-filetype=obj',str(asm),'-o',str(out/'float-blend-indices-bank.obj')],check=True,timeout=120)
 report=dict(abi=624,discovered_source_families=len(seen),already_covered_source_families=len(existing),
  added_families=len(results)//2,added_entries=len(results),base_entries=len(original),total_entries=len(entries),
  base_bytes_preserved=len(blob),base_sha256=hashlib.sha256(blob).hexdigest(),bank_sha256=hashlib.sha256(combined).hexdigest(),entries=records)
 (out/'verification.json').write_text(json.dumps(report,indent=2))
 print(f'Preserved {len(original)} entries; appended {len(results)} float-input variants for {len(results)//2} families.')
if __name__=='__main__':main()
