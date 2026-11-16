"""Check actual publication LRU and native queue-hit promotion. CPU only.

Native command execution/staging are inert lifetime boundaries here. This test
counts retained-cache hits, not texture decode time or game frame performance.
"""
from pathlib import Path
import argparse,hashlib,json,subprocess
def block(text,signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[2];src=root/'rexlego/src/gpu_native'
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
fixture=Path(__file__).with_suffix('.cpp');header=(src/'texture_upload_publication.h').read_text()
queue=block((src/'textures.cpp').read_text(),'bool QueuePreparedTextureUpload(')
stub='''#pragma once
#include <functional>
#include <memory>
namespace plume {struct RenderCommandList{virtual ~RenderCommandList()=default;};}
namespace legodimensions::gpu_native {
struct NativeDrawCommands{virtual ~NativeDrawCommands()=default;virtual void Replay(plume::RenderCommandList*)=0;};
struct NativeDeferredCommands:plume::RenderCommandList{virtual void ResourceCommand(std::function<void(plume::RenderCommandList*)>)=0;};
}
'''
def run(name,text,queue_body=queue):
    include=out/name/'gpu_native';include.mkdir(parents=True,exist_ok=True)
    (include/'native_recording.h').write_text(stub);(include/'texture_upload_publication.h').write_text(text)
    (include.parent/'publication-queue.inc').write_text(queue_body);exe=out/(name+'.exe')
    result=subprocess.run(['clang++','-std=c++23','-O2','-I'+str(include.parent),str(fixture),'-o',str(exe)],capture_output=True,text=True,timeout=90)
    (out/(name+'-compile.log')).write_text(result.stdout+result.stderr);result.check_returncode()
    result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=35)
    (out/(name+'.log')).write_text(result.stdout+result.stderr)
    return dict(name=name,exit=result.returncode,stdout=result.stdout,stderr=result.stderr)
positive=run('production',header);assert positive['exit']==0,positive
negative=[]
touch=block(header,'bool Touch(')
for name,text,body in [
    ('fifo-without-hit-promotion',header.replace(touch,touch.replace('entries_.splice(entries_.end(), entries_, found->second);','')),queue),
    ('queue-forgets-promotion',header,queue.replace('g_texture_upload_publications.Touch(upload);','')),
    ('unbounded-retention',header.replace('while (!entries_.empty() && bytes_ > budget_ - publication->Bytes())','while (false)'),queue),
    ('eviction-under-cache-lock',header.replace('retired.splice(retired.end(), entries_, entries_.begin());','entries_.erase(entries_.begin());'),queue),
]:
    assert text!=header or body!=queue,name
    result=run(name,text,body);assert result['exit']==23,(name,result);negative.append(result)
sources=[src/'texture_upload_publication.h',src/'textures.cpp',fixture,Path(__file__).resolve()]
(out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,gpu_tested=False,scope=__doc__,
    sources={str(f):sha(f) for f in sources},positive=positive,negative=negative,
    limits=['Retention budget remains128MiB in production; LRU cannot avoid eviction when the hot working set exceeds the budget.',
            'No frame-time claim; actual GPU copies and live game performance need separate integration/owner validation.']),indent=2)+'\n')
print(positive['stdout'].strip());print('PASS four broken variants rejected; no GPU/game/window.')
