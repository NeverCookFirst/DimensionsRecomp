"""Rebuild V18 additions offline in a new directory, preserving the V16 base."""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess,sys

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('output',type=Path,help='New, empty output directory')
 p.add_argument('--package',type=Path,default=Path(__file__).resolve().parents[3])
 p.add_argument('--llvm-mc',default='llvm-mc')
 p.add_argument('--verify-original',action='store_true',help='Require the unchanged shipped bank hash')
 a=p.parse_args();pkg=a.package.resolve();source=pkg/'dev-assets/worlds-material-bank-v18'
 out=a.output.resolve()
 if out==source or source in out.parents or out in source.parents:
  p.error('Use an independent output directory; the shipped bank must remain unchanged')
 if out.exists() and any(out.iterdir()):p.error('Output directory must be empty')
 out.mkdir(parents=True,exist_ok=True)
 recipe=json.loads((source/'recipe.json').read_text())
 def inventory():return {h:hashlib.sha256((source/'sources'/(h+'.hlsl')).read_bytes()).hexdigest() for h in recipe['sources']}
 actual_sources=inventory()
 tools=Path(__file__).resolve().parent;dxc=pkg/'dev-assets/dxc/bin/x64/dxc.exe'
 compiler=shutil.which(a.llvm_mc)
 if not compiler:p.error('llvm-mc is missing from the developer shell')
 base=source/'base-v16'
 for stage in ['wind','float','single_layer','two_layer']:
  target=out/stage
  args=[sys.executable,str(tools/('extend_float_input_bank.py' if stage=='float' else 'extend_position_precision_bank.py')),
   str(target),'--base',str(base),'--dxc',str(dxc),'--llvm-mc',compiler]
  if stage=='float':args+=['--sources',str(source/'sources')]
  else:
   listing=out/(stage+'-sources.json')
   listing.write_text(json.dumps([str(source/'sources'/(h+'.hlsl')) for h in recipe['stages'][stage]],indent=2))
   args+=['--position-source-list',str(listing)]
  subprocess.run(args,check=True)
  base=target
 bank=base/'float-blend-indices-bank.bin';digest=hashlib.sha256(bank.read_bytes()).hexdigest()
 if inventory()!=actual_sources:raise RuntimeError('Shader sources changed during the rebuild')
 matched=digest==recipe['expected_bank_sha256']
 result=dict(game_launched=False,bank=str(base),sha256=digest,matches_original=matched,
  expected=recipe['expected_bank_sha256'],source_inventory=actual_sources,
  sources_match_original=actual_sources==recipe['sources'])
 (out/'rebuild-verification.json').write_text(json.dumps(result,indent=2)+'\n')
 if a.verify_original and not matched:raise RuntimeError('Rebuilt bank differs from the shipped original')
 print('Material bank:',base,'sha256:',digest,'matches original:',matched)

if __name__=='__main__':main()
