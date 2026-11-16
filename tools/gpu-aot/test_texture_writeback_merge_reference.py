"""Compare native texture publication with an existing local ReXGlue SDK merge."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--reference-sdk', type=Path, required=True)
p.add_argument('--compiler', default='clang-cl')
a = p.parse_args()
a.output = a.output.resolve()
a.output.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
reference_path = a.reference_sdk / 'src/graphics/d3d12/command_processor.cpp'
text = reference_path.read_text()
start = text.index('bool MergeMemexportBytes(')
end = text.index('{', start) + 1
depth = 1
while depth:
    depth += (text[end] == '{') - (text[end] == '}')
    end += 1
reference = text[start:end]
(a.output / 'reference-merge.inc').write_text(reference)
exe = a.output / 'merge-reference.exe'
cmd = [a.compiler, str(Path(__file__).with_name('test_texture_writeback_merge.cpp')),
       '/std:c++20', '/O2', '/MD', '/EHsc', '/UNDEBUG', '/DREFERENCE_MERGE',
       '/I'+str(root/'rexlego/src'), '/I'+str(a.output),
       '/Fo'+str(a.output/'fixture.obj'), '/Fe'+str(exe)]
r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
(a.output/'compile.log').write_text(r.stdout+r.stderr)
r.check_returncode()
r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
(a.output/'positive.log').write_text(r.stdout+r.stderr)
r.check_returncode()
(a.output/'verification.json').write_text(json.dumps(dict(game_launched=False,
    window_opened=False, cases=33024, actual_reference_source=str(reference_path.resolve()),
    merge_sha256=hashlib.sha256(reference.encode()).hexdigest(),
    positive=r.stdout.strip()), indent=2)+'\n')
print(r.stdout.strip())
