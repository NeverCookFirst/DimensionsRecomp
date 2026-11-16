"""Reproduce missing capture-time payload retention using actual native functions.

Diagnostic only: success means the three source scheduling gaps were reproduced,
not that the desired immutable-input invariant passed. No GPU or game is used.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def block(text, signature):
    first = text.index(signature)
    end = text.index('{', first) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[first:end]


parser = argparse.ArgumentParser(__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--runtime-dir', required=True, type=Path)
args = parser.parse_args()
out = args.output.resolve()
assert not (out / 'verification.json').exists(), 'Preserve earlier diagnostic evidence.'
out.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
src = root / 'rexlego/src/gpu_native'
sdk = root.parent / 'dev-assets/sdk-install'
runtime = args.runtime_dir.resolve()
include = out / 'include/rex/system'
include.mkdir(parents=True, exist_ok=True)
(include.parent / 'cvar.h').write_text('''#pragma once
namespace rex::cvar { enum class Lifecycle{kRequiresRestart}; struct FixtureCvar{FixtureCvar lifecycle(Lifecycle){return *this;}}; }
#define REXCVAR_DEFINE_BOOL(name,...) static auto fixture_##name=rex::cvar::FixtureCvar{}
#define REXCVAR_GET(...) true
''')
(include / 'kernel_state.h').write_text('''#pragma once
#include <rex/system/xmemory.h>
namespace fixture {extern rex::memory::Memory* memory;}
#define REX_KERNEL_MEMORY() (fixture::memory)
''')
textures = (src / 'textures.cpp').read_text()
worker = (src / 'native_worker.cpp').read_text()
signatures = ['bool QueuePreparedTextureUpload(',
              'TexturePreparationSnapshot CaptureTexturePreparation(u32 address,',
              'TextureResourceView PrepareCachedTexture(',
              'bool ReadableUploadSpan(', 'bool CopyTextureUploadSpan(']
body = '\n'.join(block(textures, name) for name in signatures)
body += '\n' + block(worker, 'void NativeWorkerSubmitCapture(')
constant = re.search(r'constexpr size_t kStagedCaptureRun = \d+;', worker)[0]
(out / 'capture-constant.inc').write_text(constant)
records = []
variants = [('production', body),
    ('ignore-cached-header', body.replace('resource->adopted_header_type != snapshot.header[0]', 'false')),
    ('ignore-cached-watch', body.replace('!CpuMemoryUnchanged(resource->cpu_stamp)', 'false'))]
for name, content in variants:
    folder = out / name
    folder.mkdir(exist_ok=True)
    (folder / 'texture-capture-production.inc').write_text(content)
    exe = out / (name + '.exe')
    command = ['clang-cl', str(Path(__file__).with_suffix('.cpp')),
        str(src / 'memory_watch.cpp'), '/std:c++latest', '/O2', '/MD', '/EHsc', '/UNDEBUG',
        '/I' + str(folder), '/I' + str(out), '/I' + str(out / 'include'),
        '/I' + str(root / 'rexlego/src'), '/I' + str(sdk / 'include'),
        '/I' + str(root / 'thirdparty/plume'), '/Fo' + str(folder) + '/',
        '/Fe' + str(exe), '/link', str(sdk / 'lib/rexruntime.lib'), 'kernel32.lib', 'user32.lib']
    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
    (out / (name + '-compile.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        print((result.stdout + result.stderr)[-5500:])
    result.check_returncode()
    for regions in ['0', '1'] if name == 'production' else ['1']:
        env = dict(os.environ, PATH=str(runtime) + os.pathsep + os.environ['PATH'],
                   LEGO_TEST_RUNTIME_EXPECTED=str(runtime / 'rexruntime.dll'),
                   LEGO_NATIVE_WATCH_REGIONS=regions, LEGO_NATIVE_STATIC_TEXTURE_WATCH='1',
                   LEGO_NATIVE_BUFFER_WATCH='1')
        env.pop('LEGO_NATIVE_NO_MEMORY_WATCH', None)
        env.pop('LEGO_NATIVE_AUDIT_TEXTURE_WATCH', None)
        for desired in [False, True] if name == 'production' else [False]:
            result = subprocess.run([str(exe)] + (['--require-immutable'] if desired else []),
                                    capture_output=True, text=True, env=env, timeout=30)
            expected = 0 if name == 'production' and not desired else 23
            (out / f'{name}-regions-{regions}-invariant-{desired}.log').write_text(result.stdout + result.stderr)
            assert result.returncode == expected, (name, regions, desired, result.returncode, result.stdout, result.stderr)
            records.append(dict(name=name, regions=regions, desired_invariant=desired,
                                exit=result.returncode, stdout=result.stdout, stderr=result.stderr))
            print(name, regions, 'desired-invariant' if desired else 'reproduction', result.returncode)
source_files = [src / n for n in ['textures.cpp', 'textures.h', 'native_worker.cpp',
    'memory_watch.cpp', 'memory_watch.h', 'd3d.h', 'guest_metadata.h', 'texture_upload_source.h']]
source_files += [Path(__file__).resolve(), Path(__file__).with_suffix('.cpp')]
report = dict(reproduction_verified=True, desired_invariant_passed=False,
    production_modified=False, game_launched=False, window_opened=False, actual_gpu=False,
    actual_sdk_memory=True, actual_write_faults=True, sources={str(p): sha(p) for p in source_files},
    runtime_sha256=sha(runtime / 'rexruntime.dll'), runtime_import_sha256=sha(sdk / 'lib/rexruntime.lib'),
    extracted_functions={name: hashlib.sha256(block(textures, name).encode()).hexdigest() for name in signatures},
    staged_capture_body_sha256=hashlib.sha256(block(worker, 'void NativeWorkerSubmitCapture(').encode()).hexdigest(),
    records=records,
    seams=['Texture resource registry/schema and storage/view creation are injected.',
           'Publication is a retained CPU byte token; actual weak-publication queue selection executes but GPU tape replay is not exercised.',
           'Worker startup/execution scheduling is injected to hold and drain the actual submission entry point deterministically.',
           'Host/depth aliases and physical footprint derivation are outside this CPU-backed texture reproduction.'],
    limits=['The intended immutable capture invariant FAILS in the extracted current source; this report is not a fix or release gate pass.',
            'No frequency or cause for any reported world artifact is established.',
            'Earlier GPU publication/depth tests cover later upload-to-replay retention, a separate interval.'])
(out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
print('Reproduced capture-time gaps; desired immutable-input assertion fails as expected. No GPU/game.')
