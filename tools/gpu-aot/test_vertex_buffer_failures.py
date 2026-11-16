"""Check production stream binding rejects failed nonnull buffer preparation."""
import argparse,json,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
p.add_argument('--compiler',default='clang++');p.add_argument('--expect-old-failure',action='store_true')
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
subprocess.run([os.sys.executable,str(Path(__file__).with_name('test_nonindexed_buffer_windows.py')),
    str(out/'stream-boundary'),'--compiler',a.compiler],check=True,timeout=60)
code=(out/'stream-boundary/nonindexed-windows.cpp').read_text()
code=code.replace('u32 full_resolves=0,translations=0,inspections=0;','u32 full_resolves=0,translations=0,inspections=0,failed_guest=0;')
needle='BufferResourceView ResolveBufferResourceWindow(u32 guest,BufferKind,u32 offset,u32 length,const VertexByteOrder& order){'
assert code.count(needle)==1;code=code.replace(needle,needle+'\n if(guest && guest==failed_guest)return {};')
needle=' ++full_resolves;if(!guest)return {};'
assert code.count(needle)==1;code=code.replace(needle,' ++full_resolves;if(!guest || guest==failed_guest)return {};')
needle=' std::mt19937 random(123136);'
assert code.count(needle)==1
code=code.replace(needle,r'''
 // Active bound streams cannot turn an upload failure into zero attributes.
 for(bool windows:{false,true})for(u32 slot:{0u,1u}){
  reset();commands={};failed_guest=bindings.vertex_streams[slot].buffer;
  assert(!Draw(windows,false,6,123,7,0,bindings,declaration,&commands));
  assert(commands.draws==0);failed_guest=0;
  // A later successful upload recovers; explicit null streams remain valid.
  reset();commands={};Draw(windows,false,6,123,7,0,bindings,declaration,&commands);
  assert(commands.draws==1);
 }
'''+needle)
def run(name,body):
    cpp=out/(name+'.cpp');exe=out/(name+'.exe');cpp.write_text(body)
    r=subprocess.run([a.compiler,'-std=c++20','-UNDEBUG','-I'+str(root/'rexlego/src'),str(cpp),'-o',str(exe)],
        capture_output=True,text=True,timeout=45)
    (out/(name+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
    r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
    (out/(name+'.log')).write_text(r.stdout+r.stderr);return r
r=run('active-buffer',code)
if a.expect_old_failure:
    assert r.returncode==23,r.stdout+r.stderr
    (out/'verification.json').write_text(json.dumps(dict(before_fix=True,exit=r.returncode,stderr=r.stderr),indent=2))
    print('REPRODUCED failed bound vertex upload still submitting draw:',r.stderr)
else:
    assert r.returncode==0,r.stdout+r.stderr
    guard='if (binding.buffer && !buffer.buffer) return false;'
    assert code.count(guard)==1
    n=run('omitted-failure-guard',code.replace(guard,''))
    assert n.returncode==23,n.stdout+n.stderr
    (out/'verification.json').write_text(json.dumps(dict(game_launched=False,positive_exit=r.returncode,
        negative_exit=n.returncode,negative_stderr=n.stderr,
        scope='actual stream selection/binding/draw issue; mocked upload failure in first/second active stream with windows on/off and recovery; no GPU or FPS'),indent=2))
    print('PASS failed active buffer preparation rejects draw, windows on/off, recovery and omitted-guard control')
