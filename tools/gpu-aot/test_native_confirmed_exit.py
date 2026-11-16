"""Test confirmed native Quit dispatch and immediate process exit without running the game."""
from pathlib import Path
import argparse,hashlib,json,subprocess,time

p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);a=p.parse_args()
src=Path(__file__).resolve().parents[2]/'rexlego/src';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
app=(src/'legodimensions_app.h').read_text();extras=(src/'main_menu_extras.cpp').read_text()
def block(text,signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
dispatch=block(extras,'if (request == kNativeQuit)')
callback=block(app,'host.native_quit = []')+';'
request=block(extras,'void RequestNativeQuit()')
code=r'''
#define NOMINMAX
#include "native_process_exit.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <fstream>
#include <string>
#include <thread>
#define CHECK(x) do{if(!(x))return 23;}while(0)
enum { kNone=0,kNativeQuit=3 };
std::atomic<int>g_request{kNone};
REQUEST
struct Host {std::function<void()>native_quit,quit;}host_;
void Frame(){const auto request=g_request.exchange(kNone);DISPATCH}
void MustNotRun(){std::ofstream("destructor-ran.txt")<<"bad";}
int main(int argc,char**argv){
 if(argc==1){unsigned native=0,graceful=0;host_.native_quit=[&]{++native;};host_.quit=[&]{++graceful;};
  Frame();CHECK(!native&&!graceful);RequestNativeQuit();Frame();CHECK(native==1&&!graceful);
  host_.native_quit={};RequestNativeQuit();Frame();CHECK(native==1&&graceful==1);return 0;}
 HANDLE file=CreateFileA("committed.txt",GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
 CHECK(file!=INVALID_HANDLE_VALUE);DWORD written=0;CHECK(WriteFile(file,"committed",9,&written,nullptr)&&written==9);
 CHECK(FlushFileBuffers(file));CloseHandle(file);
 std::atexit(MustNotRun);
 struct Destructor{~Destructor(){MustNotRun();}}local;
 std::thread blocked([]{Sleep(INFINITE);}); // joining or ordinary stack unwinding would fail
 Host host;CALLBACK
 host_.native_quit=host.native_quit;host_.quit=[]{std::exit(29);};RequestNativeQuit();Frame();return 31;
}
'''.replace('REQUEST',request).replace('DISPATCH',dispatch).replace('CALLBACK',callback)
records=[]
def run(label,source,child=False):
    path=out/(label+'.cpp');path.write_text(source);exe=out/(label+'.exe')
    result=subprocess.run(['clang++','-std=c++23','-O2','-I'+str(src),str(path),'-o',str(exe)],capture_output=True,text=True,timeout=45)
    (out/(label+'-compile.log')).write_text(result.stdout+result.stderr);result.check_returncode()
    started=time.monotonic();result=subprocess.run([str(exe)]+(['exit'] if child else []),cwd=out,capture_output=True,text=True,timeout=3)
    seconds=time.monotonic()-started;records.append(dict(name=label,exit=result.returncode,seconds=seconds));return result
assert run('dispatch',code).returncode==0
assert run('confirmed-exit',code,True).returncode==0
assert records[-1]['seconds']<1.5
assert (out/'committed.txt').read_bytes()==b'committed' and not (out/'destructor-ran.txt').exists()
assert run('wrong-graceful-dispatch',code.replace('if (host_.native_quit) host_.native_quit();','if (host_.quit) host_.quit();')).returncode==23
report=dict(passed=True,game_launched=False,records=records,
 sources={str(src/n):hashlib.sha256((src/n).read_bytes()).hexdigest() for n in ['native_process_exit.h','legodimensions_app.h','main_menu_extras.h','main_menu_extras.cpp']},
 scope='Actual native Quit request, OnDraw dispatch branch, app native_quit callback and process-exit helper. Separate process retains flushed committed sentinel and skips blocked joins/destructors.',
 limits=['UI frame scheduling and actual gameplay saves are outside this fixture. Quit confirmation/cancel is covered by the native bridge fixture.'])
(out/'verification.json').write_text(json.dumps(report,indent=2));print(json.dumps(report['records']))
