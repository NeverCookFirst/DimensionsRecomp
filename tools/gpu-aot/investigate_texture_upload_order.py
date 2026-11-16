"""Reproduce pending GPU-tape cache ordering with an executed production cache branch. Investigation, not a fix verification."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);a=p.parse_args()
src=Path(__file__).resolve().parents[2]/'rexlego/src/gpu_native/textures.cpp';text=src.read_text();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
needle='    if (resource->guest_uploaded && resource->guest_content_hash == content_hash) {'
start=text.index(needle,text.index('bool UploadTextureResourceStorage('));end=text.index('{',start)+1;depth=1
while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
branch=text[start:end]
code=r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(23);}}while(0)
struct Resource{bool guest_uploaded=false,upload_source_key_valid=false;uint64_t guest_content_hash=0,upload_source_key=0;unsigned cpu_stamp=0;};
struct Tape{std::vector<uint64_t>uploads;};
bool Prepare(Resource*resource,Tape&tape,uint64_t content_hash){uint64_t source_key=content_hash;bool source_key_valid=true;unsigned cpu_stamp=1;
BRANCH
 tape.uploads.push_back(content_hash);resource->guest_uploaded=true;resource->guest_content_hash=content_hash;
 resource->upload_source_key=source_key;resource->upload_source_key_valid=source_key_valid;return true;
}
uint64_t Replay(Tape&tape,uint64_t&gpu){for(auto value:tape.uploads)gpu=value;return gpu;}
int main(){
 {Resource r;Tape first,second;uint64_t gpu=~uint64_t{0};CHECK(Prepare(&r,second,0xAA)&&Prepare(&r,first,0xAA));
  CHECK(Replay(first,gpu)!=0xAA&&Replay(second,gpu)==0xAA);std::puts("REPRODUCED: earliest draw skips initialization recorded only in a later tape.");}
 {Resource r;Tape first,second,third;uint64_t gpu=0;
  CHECK(Prepare(&r,first,0xAA)&&Prepare(&r,third,0xAA)&&Prepare(&r,second,0xBB));
  CHECK(Replay(first,gpu)==0xAA&&Replay(second,gpu)==0xBB&&Replay(third,gpu)!=0xAA);
  std::puts("REPRODUCED: capture values A/B/A, preparation order 1/3/2, third draw sees B after a shared-cache skip.");}
}
'''.replace('BRANCH',branch)
fixture=out/'upload-order.cpp';fixture.write_text(code);exe=out/'upload-order.exe'
r=subprocess.run(['clang++','-std=c++23','-O2',str(fixture),'-o',str(exe)],capture_output=True,text=True,timeout=45)
(out/'compile.log').write_text(r.stdout+r.stderr);r.check_returncode()
r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10);(out/'reproduction.log').write_text(r.stdout+r.stderr);r.check_returncode()
(out/'issue.json').write_text(json.dumps(dict(status='open',reproduced=True,fixed=False,game_launched=False,gpu_tested=False,
 output=r.stdout,source=str(src),source_sha256=hashlib.sha256(src.read_bytes()).hexdigest(),cache_branch_sha256=hashlib.sha256(branch.encode()).hexdigest(),
 scope='Actual decoded-content cache skip branch; resource/upload/tape/GPU publication are CPU model boundaries. Demonstrates the scheduling hazard that immutable capture-time sources must address.',
 next='Record version initialization in capture order before its first GPU consumer, or use immutable GPU versions with an equivalent publication proof. Do not claim full queued upload ordering from per-upload byte retention.'),indent=2))
print(r.stdout)
