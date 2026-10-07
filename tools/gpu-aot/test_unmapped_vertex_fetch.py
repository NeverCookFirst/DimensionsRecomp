#!/usr/bin/env python3
"""Execute the pinned compiler's actual fetch lookup guard without game assets."""
import argparse
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--compiler', default='clang++')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
checkout = root / 'research/reblue/thirdparty/XenosRecomp/XenosRecomp'
source = checkout / 'shader_recompiler.cpp'
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
patched = output / 'shader_recompiler.cpp'
patched.write_bytes(source.read_bytes())
patch = root / 'tools/gpu-aot/patches/xenosrecomp-unmapped-vertex-fetch.patch'
subprocess.run(['git', 'apply', '--unsafe-paths', '-p2',
                '--directory=' + str(output), str(patch)], check=True, timeout=10,
               cwd=root)
patched_text = patched.read_text()
if (patched_text.count('no semantic input mapping') != 1 or
    'assert(findResult != vertexElements.end());' in patched_text):
    raise RuntimeError('Vertex-fetch patch did not update the fixture source')
body = re.search(r'    auto findResult = vertexElements.find\(address\);\n.*?(?=\n#ifdef REBLUE_RECOMP)',
                 patched_text, re.S)
assert body and 'no semantic input mapping' in body.group()
legacy = re.search(r'    auto findResult = vertexElements.find\(address\);\n.*?(?=\n#ifdef REBLUE_RECOMP)',
                   source.read_text(), re.S)
assert legacy and 'assert(findResult != vertexElements.end());' in legacy.group()
preparation = (root / 'tools/gpu-aot/prepare_native_compiler.py').read_text()
assert patch.name in preparation and 'unmapped_vertex_fetch_patch_sha256' in preparation
fixture = r'''
#define FMT_HEADER_ONLY
#include <fmt/format.h>
#include <cassert>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
uint32_t lookup(const std::map<uint32_t, uint32_t>& vertexElements, uint32_t address) {
@BODY@
    // The fixture avoids dereferencing end in the old implementation. A missing
    // error still fails, rather than relying on nondeterministic undefined behavior.
    return findResult == vertexElements.end() ? 0 : findResult->second;
}
int main() {
    for (auto address : {0u, 100u, 0xffffffffu}) {
        for (const auto& inputs : {std::map<uint32_t,uint32_t>{},
                                  std::map<uint32_t,uint32_t>{{7, 42}}}) {
            try { lookup(inputs, address); return 17; }
            catch (const std::runtime_error& error) {
                if (std::string(error.what()) != fmt::format(
                    "unsupported vertex fetch address {} (no semantic input mapping)", address)) return 18;
            }
        }
    }
    if (lookup({{7,42}},7) != 42 || lookup({{0xffffffffu,99}},0xffffffffu) != 99) return 19;
}
'''
results = {}
for name, block, expected in [('guarded', body.group(), 0), ('legacy', legacy.group(), 17)]:
    cpp, exe = output / (name + '.cpp'), output / name
    cpp.write_text(fixture.replace('@BODY@', block))
    subprocess.run([args.compiler, '-std=c++20', '-DNDEBUG',
                    '-I' + str(checkout.parent / 'thirdparty/fmt/include'),
                    str(cpp), '-o', str(exe)], check=True, timeout=45)
    result = subprocess.run([str(exe)], timeout=10)
    assert result.returncode == expected, (name, result.returncode, expected)
    results[name] = result.returncode
(output / 'verification.json').write_text(json.dumps(dict(
    status='passed', production_guard=True, release_asserts_disabled=True,
    missing_mapping_cases=6, valid_mapping_cases=2, negative_control=results,
    boundary='Fetch lookup guard only; shader instruction decoding and DXC are tested separately with actual image inputs.'
), indent=2) + '\n')
print('unmapped vertex fetch guard PASS; legacy negative control rejected')
