"""Verify copied native render snapshot decoding against actual FSR/SDK code."""
from pathlib import Path
import argparse, hashlib, json, os, shutil, subprocess


def main():
    p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);p.add_argument('--compiler',default='clang++');a=p.parse_args()
    root=Path(__file__).resolve().parents[2];src=root/'rexlego/src';sdk=root.parent/'dev-assets/sdk-install'
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    runtime=root.parent/'build/worlds-candidate-v18'
    for name in ['rexruntime.dll','amd_fidelityfx_dx12.dll']:shutil.copy2(runtime/name,out/name)
    fixture=Path(__file__).with_suffix('.cpp');production=src/'gpu_native/settings_snapshot.cpp'
    flags=['-std=c++23','-O2','-DWIN32_LEAN_AND_MEAN','-DNOMINMAX','-D_CRT_SECURE_NO_WARNINGS',
           '-Wno-deprecated-literal-operator','-I'+str(src),'-I'+str(sdk/'include')]
    objects=[]
    for source in [fixture,src/'gpu_native/fsr3_settings.cpp',src/'settings_registry.cpp',src/'settings_config.cpp']:
        obj=out/(source.stem+'.obj');r=subprocess.run([a.compiler,*flags,'-c',str(source),'-o',str(obj)],capture_output=True,text=True,timeout=75)
        (out/(source.stem+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode();objects.append(obj)
    env=os.environ.copy()
    for name in ['LEGO_NATIVE_FSR3','LEGO_NATIVE_FSR3_QUALITY','LEGO_NATIVE_720P']:env.pop(name,None)
    def run(label,text):
        source=out/(label+'.cpp');source.write_text(text);exe=out/(label+'.exe')
        r=subprocess.run([a.compiler,*flags,str(source),*map(str,objects),str(sdk/'lib/rexruntime.lib'),'-o',str(exe)],capture_output=True,text=True,timeout=75)
        (out/(label+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
        r=subprocess.run([str(exe)],env=env,capture_output=True,text=True,timeout=20)
        (out/(label+'.log')).write_text(r.stdout+r.stderr);return r
    text=production.read_text();positive=run('production',text);positive.check_returncode();print(positive.stdout,end='',flush=True)
    negative=[]
    for label,old,new in [
        ('ignore-vsync-override','overrides.vsync.value_or(Boolean(values,"native_vsync"))','Boolean(values,"native_vsync")'),
        ('ignore-720-priority','overrides.native_720p ? 1u : Quality','false ? 1u : Quality'),
        ('disable-fsr1-choice','Value(values,"present_effect") == "fsr"','false'),
    ]:
        assert text.count(old)==1,(label,text.count(old));r=run(label,text.replace(old,new));assert r.returncode==23,(label,r.returncode,r.stdout,r.stderr)
        negative.append(dict(name=label,exit=r.returncode,error=r.stderr))
    files=[production,production.with_suffix('.h'),src/'gpu_native/fsr3_settings.cpp',src/'gpu_native/fsr3_settings.h',
           src/'settings_registry.cpp',src/'settings_registry.h',src/'settings_revision.h',fixture,Path(__file__).resolve(),runtime/'rexruntime.dll']
    (out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,output=positive.stdout,negative=negative,
        sources={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
        scope='Actual snapshot decoder and FSR request capture/scale functions, real SDK registry DLL for FSR differential. No renderer/window consumer wiring, device allocation or gameplay acceptance.'),indent=2))


if __name__=='__main__':main()
