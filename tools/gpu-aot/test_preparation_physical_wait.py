"""Execute actual capture-filter admission and coordinator waits against aliased texture inputs.

CPU fixture only: captured footprints and task queue storage are injected. The
production wait, chunk claiming, physical union and address filters execute.
"""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);p.add_argument('--previous-draw',type=Path,required=True);a=p.parse_args()
src=Path(__file__).resolve().parents[2]/'rexlego/src';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
def block(text,signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
draw=(src/'gpu_native/draw.cpp').read_text();wait=block(draw,'void WaitPendingPreparationFor(')
admit=block(draw,'void AddFullDrawPhysicalDependencies(')
fixture=Path(__file__).with_suffix('.cpp');body=fixture.read_text()
records=[]
def run(label,wait_body=wait,admit_body=admit):
    path=out/(label+'.cpp');path.write_text(body.replace('ADMIT_BODY',admit_body).replace('WAIT_BODY',wait_body));exe=out/(label+'.exe')
    r=subprocess.run(['clang++','-std=c++23','-O2','-I'+str(src),str(path),'-o',str(exe)],capture_output=True,text=True,timeout=50)
    (out/(label+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
    r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=12);(out/(label+'.log')).write_text(r.stdout+r.stderr)
    records.append(dict(name=label,exit=r.returncode,output=r.stdout,error=r.stderr));return r
assert run('production').returncode==0
previous=block(a.previous_draw.read_text(),'void WaitPendingPreparationFor(')
assert run('previous-address-only',previous).returncode==23
old='return filter.Matches(addresses) || filter.physical.Matches(physical);';assert wait.count(old)==1
assert run('omit-physical-overlap',wait.replace(old,'return filter.Matches(addresses);')).returncode==23
assert run('omit-captured-footprints',admit_body=admit.replace('filter.Add(input.footprint);','(void)input;')).returncode==23
assert run('omit-fallback-unknown',admit_body=admit.replace('filter.unknown = true;','filter.unknown = false;')).returncode==23
sources=[src/'gpu_native/draw.cpp',src/'gpu_native/textures.cpp',src/'gpu_native/textures.h',
 src/'gpu_native/preparation_dependencies.h',src/'gpu_native/preparation_footprint.h',Path(__file__),fixture]
(out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,gpu_tested=False,records=records,
 sources={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},previous_draw=dict(path=str(a.previous_draw),sha256=hashlib.sha256(a.previous_draw.read_bytes()).hexdigest()),
 scope=__doc__,limits=['Footprint discovery/SDK texture layout has a separate fixture. This fixture injects captured ranges and task queue storage.',
 'No pending native resolve ownership, renderer registration, GPU execution or all-world visual/performance acceptance. Immutable metadata/payload ordering across later guest placement edits remains to be completed before provider activation.']),indent=2))
print(json.dumps(records))
