"""Link/run the actual compiled trace object with console-only guest boundaries."""
import argparse,csv,ctypes,hashlib,json,os,shutil,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('build',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
entry=next(r for r in json.loads((a.build/'compile_commands.json').read_text()) if Path(r['file']).name=='tt_lock_trace.cpp')
if 'arguments' in entry:args=entry['arguments']
else:
 count=ctypes.c_int();shell=ctypes.WinDLL('shell32');shell.CommandLineToArgvW.argtypes=[ctypes.c_wchar_p,ctypes.POINTER(ctypes.c_int)]
 shell.CommandLineToArgvW.restype=ctypes.POINTER(ctypes.c_wchar_p);pointer=shell.CommandLineToArgvW(entry['command'],ctypes.byref(count))
 args=[pointer[i] for i in range(count.value)];ctypes.WinDLL('kernel32').LocalFree(ctypes.cast(pointer,ctypes.c_void_p))
filtered=[];index=0
while index<len(args):
 value=args[index]
 if value in ('-o','-MT','-MF','-c'):index+=2;continue
 if value in ('-MD','-MMD'):index+=1;continue
 filtered.append(value);index+=1
obj=a.build/'CMakeFiles/legodimensions.dir/src/tt_lock_trace.cpp.obj';copy=out/'tt_lock_trace.cpp.obj';shutil.copy2(obj,copy)
source=Path(__file__).with_name('test_tt_lock_trace_windows.cpp').resolve();exe=out/'test-tt-lock.exe'
command=filtered+[str(source),str(copy),'-fms-runtime-lib=dll','-Wl,/nodefaultlib:libcmt','-UNDEBUG','-o',str(exe)]
(out/'compile-command.json').write_text(json.dumps(command,indent=2))
result=subprocess.run(command,cwd=entry['directory'],capture_output=True,text=True,timeout=60)
(out/'compile.log').write_text(result.stdout+result.stderr);assert result.returncode==0,(result.stdout+result.stderr)[-4000:]
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
results=[]
for route in ('0','1'):
 path=out/('enabled-route'+route+'.csv')
 result=subprocess.run([str(exe),str(path),'enabled'],capture_output=True,text=True,timeout=10,
  env=dict(os.environ,LEGO_NATIVE_PM4_REFERENCE=route))
 assert result.returncode==0,result.stderr
 with path.open() as f:metadata=f.readline().strip();rows=list(csv.DictReader(f))
 assert len(rows)==2 and len({r['tid'] for r in rows})==2
 assert all(r['lock']=='0x84959EDC' and r['wall_valid']=='1' and r['cpu_valid']=='1' and r['scopes_closed']=='1' and r['orphan_release']=='0' for r in rows)
 assert sum(int(r['native_draw_attempts']) for r in rows)==1 and sum(int(r['render_calls']) for r in rows)==1
 assert all(int(r['wait_begin_qpc'])<=int(r['acquired_qpc'])<=int(r['released_qpc']) for r in rows)
 assert any(int(r['acquired_qpc'])>int(r['wait_begin_qpc']) for r in rows)
 results.append(dict(route=route,exit=result.returncode,rows=len(rows),stdout=result.stdout,metadata=metadata))
disabled=out/'disabled.csv';r=subprocess.run([str(exe),str(disabled),'disabled'],capture_output=True,text=True,timeout=10)
assert r.returncode==0 and not disabled.exists(),r.stderr
existing=out/'preserved.csv';existing.write_text('Preserve this original diagnostic\n');before=sha(existing)
r=subprocess.run([str(exe),str(existing),'enabled','existing'],capture_output=True,text=True,timeout=10)
assert r.returncode==0 and sha(existing)==before,r.stderr
proof=dict(game_launched=False,window_opened=False,actual_compiled_diagnostic_object_sha256=sha(copy),
 positive=results,disabled_no_file=True,existing_file_unchanged=True,
 scope='actual Win32 QPC/GetThreadTimes/CSV sink/raw hooks, synthetic original guest mutex/render functions; PM4 env bypass test, not either renderer executing or FPS')
(out/'verification.json').write_text(json.dumps(proof,indent=2));print(json.dumps(proof,indent=2))
