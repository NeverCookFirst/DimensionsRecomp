"""Compile native settings TUs with actual app flags, without changing the build."""
from pathlib import Path
import argparse,ctypes,hashlib,json,subprocess

def main():
    p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path)
    p.add_argument('--units',nargs='+',choices=['main_menu_extras','settings_transaction','settings_config','settings_registry','settings_service','settings_revision','settings_native_snapshot','settings_native_runtime','settings_preferences','settings_frontend','settings_native_bridge','settings_native_hooks','ui_theme','main'])
    a=p.parse_args()
    root=Path(__file__).resolve().parents[2];pkg=root.parent;build=pkg/'build/native';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    result=subprocess.run(['ninja','-C',str(build),'-t','commands','CMakeFiles/legodimensions.dir/src/main_menu_extras.cpp.obj'],capture_output=True,text=True,check=True,timeout=20)
    command=result.stdout.splitlines()[-1]
    shell=ctypes.WinDLL('shell32');kernel=ctypes.WinDLL('kernel32')
    shell.CommandLineToArgvW.argtypes=[ctypes.c_wchar_p,ctypes.POINTER(ctypes.c_int)]
    shell.CommandLineToArgvW.restype=ctypes.POINTER(ctypes.c_wchar_p)
    kernel.LocalFree.argtypes=[ctypes.c_void_p];kernel.LocalFree.restype=ctypes.c_void_p
    count=ctypes.c_int();raw=shell.CommandLineToArgvW(command,ctypes.byref(count));assert raw
    template=[raw[i] for i in range(count.value)];kernel.LocalFree(ctypes.cast(raw,ctypes.c_void_p))
    source_indices=[i for i,arg in enumerate(template) if Path(arg).name=='main_menu_extras.cpp'];assert len(source_indices)==1
    reports=[]
    for name in a.units or ['settings_transaction','settings_config','settings_registry','settings_service','settings_revision','settings_preferences','settings_frontend','settings_native_bridge','settings_native_hooks','main']:
        relative={'settings_native_snapshot':'gpu_native/settings_snapshot','settings_native_runtime':'gpu_native/settings_runtime'}.get(name,name)
        source=root/'rexlego/src'/(relative+'.cpp');args=template.copy();args[source_indices[0]]=str(source)
        obj=out/(name+'.obj')
        for flag,value in [('-o',obj),('-MF',out/(name+'.d')),('-MT',obj)]:args[args.index(flag)+1]=str(value)
        result=subprocess.run(args,cwd=build,capture_output=True,text=True,timeout=90)
        (out/(name+'-compile.log')).write_text(result.stdout+result.stderr);result.check_returncode()
        reports.append(dict(source=str(source),source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                            object=str(obj),object_sha256=hashlib.sha256(obj.read_bytes()).hexdigest(),command=args))
    (out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,units=reports,
        scope='Selected production translation units with real native CMake/Ninja compile flags; see units for exact source coverage. Private objects only; no build regeneration, app link, game launch, native UI or renderer acceptance.'),indent=2))
    print('PASS: '+', '.join(Path(r['source']).stem for r in reports)+' compiled with actual native application flags.')
if __name__=='__main__':main()
