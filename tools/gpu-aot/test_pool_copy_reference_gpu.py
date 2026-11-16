"""Compare a saved ReXGlue pool-copy GPU shader with the native publication body."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def body(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--plume-lib', required=True, type=Path)
p.add_argument('--reference-vs', required=True, type=Path)
p.add_argument('--reference-sdk', required=True, type=Path)
p.add_argument('--compiler', default='clang-cl')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
a.output = a.output.resolve()
a.output.mkdir(parents=True, exist_ok=True)
reference = a.reference_vs.resolve()
source = (root / 'rexlego/src/gpu_native/hooks_buffer.cpp').read_text()
(a.output / 'pool-copy-recorded.inc').write_text(body(source, 'void PoolCopyRecorded('))
merge_source = a.reference_sdk / 'src/graphics/d3d12/command_processor.cpp'
(a.output / 'reference-merge.inc').write_text(body(merge_source.read_text(), 'bool MergeMemexportBytes('))
exe = a.output / 'pool-copy-reference-gpu.exe'
cmd = [a.compiler, str(Path(__file__).with_suffix('.cpp')), '/std:c++20', '/O2', '/MD', '/EHsc',
       '/I' + str(root / 'rexlego/src'), '/I' + str(root / 'thirdparty/plume'),
       '/I' + str(root / 'thirdparty/plume/contrib/D3D12MemoryAllocator/include'),
       '/I' + str(a.output), '/Fo' + str(a.output / 'fixture.obj'), '/Fe' + str(exe), '/link',
       str(a.plume_lib.resolve()), 'd3d12.lib', 'dxgi.lib', 'dxguid.lib', 'user32.lib']
r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
(a.output / 'compile.log').write_text(r.stdout + r.stderr)
r.check_returncode()
r = subprocess.run([str(exe), str(reference)], capture_output=True, text=True, timeout=60)
(a.output / 'positive.log').write_text(r.stdout + r.stderr)
if r.returncode:
    raise RuntimeError(r.stdout + r.stderr)
print(r.stdout.strip())
negative = subprocess.run([str(exe), str(reference), 'omit-export-endian'],
                          capture_output=True, text=True, timeout=60)
(a.output / 'negative.log').write_text(negative.stdout + negative.stderr)
if negative.returncode != 23:
    raise RuntimeError('Changing the reference export endianness escaped the byte comparison')
merge_negative = subprocess.run([str(exe), str(reference), 'omit-merge'],
                                capture_output=True, text=True, timeout=60)
(a.output / 'merge-negative.log').write_text(merge_negative.stdout + merge_negative.stderr)
if merge_negative.returncode != 23:
    raise RuntimeError('Omitting the CPU-write-preserving reference merge escaped the byte comparison')
(a.output / 'verification.json').write_text(json.dumps(dict(game_launched=False,
    window_opened=False, actual_reference_shader=str(reference),
    reference_shader_sha256=hashlib.sha256(reference.read_bytes()).hexdigest(),
    reference_merge_source=str(merge_source.resolve()),
    reference_merge_sha256=hashlib.sha256(body(merge_source.read_text(), 'bool MergeMemexportBytes(').encode()).hexdigest(),
    production_native_copy=True, positive=r.stdout.strip(),
    negative_exit=negative.returncode,
    merge_negative_exit=merge_negative.returncode,
    scope='Saved patched pool-copy VS and native copy bytes; not generic memexport or gameplay acceptance'), indent=2))
print('PASS negative control: wrong export endianness fails exact byte comparison')
print('PASS negative control: omitting reference byte merge loses newer CPU writes')
