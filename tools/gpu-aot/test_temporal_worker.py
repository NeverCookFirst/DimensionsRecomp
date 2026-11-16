"""Compile the actual Windows coordinator/recorder with a CPU command-tape boundary."""
from pathlib import Path
import argparse
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--worker-source', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
out = a.output.resolve()
for name, content in {
    'rex/logging.h': '#pragma once\n#define REXLOG_INFO(...) ((void)0)\n',
    'gpu_native/pool_copy.h': '#pragma once\nnamespace legodimensions::gpu_native {void FlushPoolCopyBatch();}\n',
    'gpu_native/native_recording.h': '''#pragma once
namespace legodimensions::gpu_native {
bool NativeFullDrawPreparationEnabled(); bool NativePrepareEnabled();
unsigned NativePreparationWorkerCount(); void StopNativePreparation();
}
''',
    'gpu_native/fsr3_settings.h': '#pragma once\nnamespace legodimensions::gpu_native {bool NativeFsr3Enabled();}\n',
}.items():
    dest = out / 'include' / name
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(content)
exe = out / 'temporal-worker.exe'
source = a.worker_source or root / 'rexlego/src/gpu_native/native_worker.cpp'
command = ['clang-cl', '/std:c++20', '/O2', '/EHsc', '/MD', '/UNDEBUG', '/DNOMINMAX',
           '/I' + str(out / 'include'), '/I' + str(root / 'rexlego/src'),
           str(source), str(Path(__file__).with_suffix('.cpp')),
           '/Fe' + str(exe), '/Fo' + str(out) + '/']
result = subprocess.run(command, capture_output=True, text=True)
(out / 'compile.log').write_text(result.stdout + result.stderr)
result.check_returncode()
result = subprocess.run([exe], capture_output=True, text=True, timeout=20)
print(result.stdout + result.stderr, end='')
raise SystemExit(result.returncode)
