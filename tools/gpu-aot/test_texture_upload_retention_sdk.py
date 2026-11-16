"""Retained upload bytes through actual native decode loops and SDK layouts/mappings; CPU only."""
from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess
p=argparse.ArgumentParser(__doc__);p.add_argument('output',type=Path);p.add_argument('--runtime-dir',required=True,type=Path);p.add_argument('--previous-textures',required=True,type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[2];src=root/'rexlego/src';sdk=root.parent/'dev-assets/sdk-install';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
text=(src/'gpu_native/textures.cpp').read_text();format_text=(src/'gpu_native/format.cpp').read_text();fixture=Path(__file__).with_suffix('.cpp')
def block(text,signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
helpers='\n'.join([block(text,s) for s in ['bool ReadableUploadSpan(','bool CopyTextureUploadSpan(','u32 AlignUp(','u32 MipDimension(',
 'u32 RowPitch(','u32 RowCount(','u32 LevelSize(']]+[block(format_text,s) for s in ['bool IsBlockCompressedFormat(','u32 FormatBlockBytes(']])
def decode(text):
    upload=block(text,'bool UploadTextureResourceStorage(');start=upload.index('    u32 destination_offset = 0;',upload.index('std::vector<u8> decoded(upload_size)'))
    return upload[start:upload.index('    DumpTextureUpload',start)]
header=(src/'gpu_native/texture_upload_source.h').read_text();records=[]
for name in ['rexruntime.dll','amd_fidelityfx_dx12.dll']:shutil.copy2(a.runtime_dir/name,out/name)
def run(label,loop,source_header=header,copy_helpers=helpers):
    (out/'source-under-test.h').write_text(source_header);(out/'retained-helpers.inc').write_text(copy_helpers);(out/'retained-decode.inc').write_text(loop)
    exe=out/(label+'.exe');r=subprocess.run(['clang-cl',str(fixture),'/std:c++latest','/O2','/MD','/EHsc','/D_CRT_SECURE_NO_WARNINGS',
     '/I'+str(out),'/I'+str(src),'/I'+str(sdk/'include'),'/I'+str(root/'thirdparty/plume'),
     '/Fo'+str(out)+'/', '/Fe'+str(exe),'/link',str(sdk/'lib/rexruntime.lib'),'kernel32.lib','user32.lib'],capture_output=True,text=True,timeout=75)
    (out/(label+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
    r=subprocess.run([str(exe)],cwd=out,env=dict(os.environ,LEGO_TEST_RUNTIME_EXPECTED=str(out/'rexruntime.dll')),capture_output=True,text=True,timeout=75)
    (out/(label+'.log')).write_text(r.stdout+r.stderr);records.append(dict(name=label,exit=r.returncode,output=r.stdout,error=r.stderr));return r
loop=decode(text);assert run('production',loop).returncode==0
assert run('previous-live-decode',decode(a.previous_textures.read_text())).returncode==23
needle='mips ? std::pair(mip_address_, Mips()) : std::pair(base_address_, Base())';assert header.count(needle)==1
assert run('ignore-selected-mip-bank',loop,header.replace(needle,'std::pair(base_address_, Base())')).returncode==23
needle='length <= segment.second.size() - offset';assert header.count(needle)==1
assert run('missing-slice-bounds',loop,header.replace(needle,'true')).returncode==23
previous=a.previous_textures.read_text()
old_helpers=helpers.replace(block(text,'bool ReadableUploadSpan('),block(previous,'bool ReadableUploadSpan('))
assert run('previous-inherited-alias-rejection',loop,copy_helpers=old_helpers).returncode==23
(out/'source-under-test.h').write_text(header);(out/'retained-decode.inc').write_text(loop)
(out/'retained-helpers.inc').write_text(helpers)
sources=[src/n for n in ['gpu_native/textures.cpp','gpu_native/texture_upload_source.h','gpu_native/texture_upload_cache.h',
 'gpu_native/texture_volume_upload.h','gpu_native/texture_alpha4_upload.h','gpu_native/texture_endian.h','gpu_native/format.cpp']]+[fixture,Path(__file__)]
(out/'verification.json').write_text(json.dumps(dict(passed=True,game_launched=False,gpu_tested=False,actual_sdk_memory=True,records=records,
 sources={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},scope=__doc__,
 limits=['Production residency/copy/decode loop and SDK layouts execute; resource description and upload command submission are fixture boundaries.',
 'The snapshot is currently taken within one CPU upload, not on draw submission. Native GPU-provider settlement, immutable capture-time versions, replay cache order and GPU/world/performance acceptance remain incomplete.']),indent=2))
print(json.dumps(records))
