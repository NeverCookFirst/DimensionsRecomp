"""Actual texture footprint/layout and draw metadata consumers against delivered SDK memory/providers; no game or GPU."""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess
p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);p.add_argument('--runtime-dir',type=Path,required=True);p.add_argument('--previous-draw',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[2];src=root/'rexlego/src';sdk=root.parent/'dev-assets/sdk-install';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
def block(text,signature,last=False):
    start=text.rindex(signature) if last else text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
texture=(src/'gpu_native/textures.cpp').read_text();draw=(src/'gpu_native/draw.cpp').read_text();hooks=(src/'gpu_native/hooks_buffer.cpp').read_text()
functions=[block(texture,'std::array<PhysicalCopyRange, 2> TextureDataRanges('),
 block(texture,'TexturePreparationFootprint TexturePreparationFootprintOf(',True),
 block(texture,'TexturePreparationFootprint CaptureTexturePreparationFootprint('),
 block(texture,'TexturePreparationSnapshot CaptureTexturePreparation(u32 address, std::shared_ptr<void> resolved)'),
 block(draw,'bool TemplateHeadersRepeat('),block(draw,'bool ReadResolveWords('),
 block(hooks,'bool PhysicalRange(').replace('bool PhysicalRange(','bool legodimensions::gpu_native::PoolCopyPhysicalRange(',1)]
fixture=Path(__file__).with_suffix('.cpp');records=[]
for name in ['rexruntime.dll','amd_fidelityfx_dx12.dll']:shutil.copy2(a.runtime_dir/name,out/name)
def run(label,bodies):
    (out/'footprint-functions.inc').write_text('\n'.join(bodies));exe=out/(label+'.exe')
    r=subprocess.run(['clang-cl',str(fixture),'/std:c++latest','/O2','/MD','/EHsc','/D_CRT_SECURE_NO_WARNINGS','/I'+str(out),'/I'+str(src),
     '/I'+str(sdk/'include'),'/I'+str(root/'rexglue-sdk/include'),'/Fo'+str(out)+'/', '/Fe'+str(exe),'/link',str(sdk/'lib/rexruntime.lib'),'kernel32.lib','user32.lib'],capture_output=True,text=True,timeout=60)
    (out/(label+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
    r=subprocess.run([str(exe)],cwd=out,env=dict(os.environ,LEGO_TEST_RUNTIME_EXPECTED=str(out/'rexruntime.dll')),capture_output=True,text=True,timeout=15)
    (out/(label+'.log')).write_text(r.stdout+r.stderr);records.append(dict(name=label,exit=r.returncode,output=r.stdout,error=r.stderr));return r
assert run('production',functions).returncode==0
old=a.previous_draw.read_text();previous=functions.copy();previous[4]=block(old,'bool TemplateHeadersRepeat(');previous[5]=block(old,'bool ReadResolveWords(')
assert run('previous-protected-metadata',previous).returncode==23
broken=functions.copy();broken[0]=broken[0].replace('if (resource.levels > 1 && fetch.mip_address)','if (false && resource.levels > 1 && fetch.mip_address)')
assert run('missing-mip-range',broken).returncode==23
broken=functions.copy();broken[-1]=broken[-1].replace('memory->GetPhysicalAddress(address);','(address & 0x1FFFFFFFu);').replace('memory->GetPhysicalAddress(address + size - 1);','((address + size - 1) & 0x1FFFFFFFu);')
assert run('missing-ef-bias',broken).returncode==23
broken=functions.copy();broken[1]=broken[1].replace('std::unique_lock lock(resource.mutex, std::try_to_lock);','std::unique_lock lock(resource.mutex);')
assert run('blocking-resource-capture',broken).returncode==23
(out/'footprint-functions.inc').write_text('\n'.join(functions))
sources=[src/n for n in ['gpu_native/textures.cpp','gpu_native/textures.h','gpu_native/draw.cpp','gpu_native/hooks_buffer.cpp',
 'gpu_native/guest_metadata.h','gpu_native/preparation_footprint.h','gpu_native/texture_endian.h']]+[fixture,Path(__file__)]
(out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,gpu_tested=False,actual_sdk_memory=True,records=records,
 sources={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},scope=__doc__,
 limits=['Native resource storage/map and depth-alias branch are injected. Layout utility, SDK mappings/protection/provider faults, copied production capture/query/header/resolve functions execute.',
 'This does not register renderer ownership or prove immutable queued GPU versions, late guest placement edits, visual correctness or performance.']),indent=2))
print(json.dumps(records))
