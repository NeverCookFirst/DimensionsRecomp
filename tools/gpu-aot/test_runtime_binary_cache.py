"""Link the actual runtime wrapper against its separately embedded COFF payload."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import zlib

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path)
p.add_argument('--compiler',default='clang++')
p.add_argument('--llvm-mc',default='llvm-mc')
p.add_argument('--llvm-objcopy',default='llvm-objcopy')
p.add_argument('--linker',default='lld-link')
a=p.parse_args()
root=Path(__file__).resolve().parents[2]
work=a.output.resolve();work.mkdir(parents=True,exist_ok=True)
include=work/'include'; header=include/'gpu/shaders/linked_shader_cache.h'
header.parent.mkdir(parents=True,exist_ok=True)
shutil.copy2(root/'rexlego/src/gpu_native/linked_shader_cache.h',header)
(include/'cstddef').write_text('#pragma once\nusing size_t=__SIZE_TYPE__;\n')
(include/'cstdint').write_text('#pragma once\nusing uint8_t=unsigned char;using uint32_t=unsigned int;using uint64_t=unsigned long long;\n')
data=bytes(range(256))*4;payload=zlib.compress(data)
bank=work/'runtime-linked.cpp'
bank.write_text('#include "gpu/shaders/linked_shader_cache.h"\n'
 'LinkedShaderCacheEntry g_linkedShaderCacheEntries[] = {\n'
 f'{{ 0x1234, 0, 0, {len(data)} }},\n}};\n'
 'const size_t g_linkedShaderCacheEntryCount = 1;\n'
 'const uint8_t g_compressedLinkedDxilCache[] = {\n'+
 ''.join(','.join(str(v) for v in payload[i:i+32])+',\n' for i in range(0,len(payload),32))+
 '};\n'+f'const size_t g_linkedDxilCacheCompressedSize = {len(payload)};\n'
 f'const size_t g_linkedDxilCacheDecompressedSize = {len(data)};\n')
generated=work/'runtime object with spaces'
subprocess.run([sys.executable,str(root/'tools/gpu-aot/embed_linked_bank.py'),str(bank),str(generated),'--llvm-mc',a.llvm_mc,'--runtime'],check=True,stdout=subprocess.DEVNULL)
manifest=json.loads((generated/'linked-bank-manifest.json').read_text())
assert manifest['coff_symbol']=='?g_runtimeCompressedLinkedDxilCache@@3QBEB'
assert (generated/'linked-bank.bin').read_bytes()==payload
dump=work/'object-payload.bin'
subprocess.run([a.llvm_objcopy,'--dump-section',f'.rdata={dump}',str(generated/'linked-bank.obj')],check=True)
assert dump.read_bytes()==payload
common=[a.compiler,'--target=x86_64-pc-windows-msvc','-std=c++20','-nostdinc','-nostdinc++','-I'+str(include)]
metadata=generated/'linked-bank-metadata.cpp'
wrapper=work/'runtime-wrapper.obj'
subprocess.run(common+[f'-DLEGO_NATIVE_RUNTIME_LINKED_METADATA="{metadata.as_posix()}"','-c',str(root/'rexlego/src/gpu_native/runtime_linked_shader_cache.cpp'),'-o',str(wrapper)],check=True)
probe=work/'probe.cpp'
probe.write_text('#include "gpu/shaders/linked_shader_cache.h"\n'
 'extern "C" __declspec(dllexport) const void* RuntimePayload(){return g_runtimeCompressedLinkedDxilCache;}\n'
 'extern "C" __declspec(dllexport) const void* RuntimeTable(){return g_runtimeLinkedShaderCacheEntries;}\n'
 'extern "C" __declspec(dllexport) size_t RuntimeSize(){return g_runtimeLinkedDxilCacheCompressedSize;}\n')
probe_obj=work/'probe.obj'
subprocess.run(common+['-c',str(probe),'-o',str(probe_obj)],check=True)
link=[a.linker,'/dll','/noentry','/nodefaultlib','/out:'+str(work/'runtime-fixture.dll'),str(probe_obj),str(wrapper)]
subprocess.run(link+[str(generated/'linked-bank.obj')],check=True,capture_output=True)
negative=subprocess.run([v for v in link if not v.startswith('/out:')]+['/out:'+str(work/'missing-payload.dll')],capture_output=True,text=True)
assert negative.returncode!=0 and 'g_runtimeCompressedLinkedDxilCache' in negative.stdout+negative.stderr
print('PASS: actual runtime wrapper links only with distinct runtime COFF symbol; object bytes equal generated payload; missing-payload control fails')
