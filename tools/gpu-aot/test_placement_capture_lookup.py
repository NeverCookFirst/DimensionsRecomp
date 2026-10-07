"""Verify exact captured shader sections through the production AOT lookup.

Inputs remain local: validated placement-containers, the installed index, and
runtime cache. No game shader bytes are copied into the repository.
"""
import argparse
from pathlib import Path
import re
import struct
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('captures', type=Path)
    p.add_argument('index', type=Path)
    p.add_argument('cache', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--main-cache', type=Path,
                   help='Also expose the compiled main archive, as gameplay does')
    a = p.parse_args()
    root = Path(__file__).resolve().parents[2]
    a.output.mkdir(parents=True, exist_ok=True)
    cache = a.cache.read_text()
    hashes = re.findall(r'\{ 0x([0-9A-Fa-f]+), \d+, \d+, \d+, \d+, \d+ \}', cache)
    if not hashes:
        raise ValueError('No compiled cache entries')
    if a.main_cache:
        hashes += re.findall(r'\{ 0x([0-9A-Fa-f]+), \d+, \d+, \d+, \d+, \d+ \}',
                             a.main_cache.read_text())
    index = a.index.read_text()
    sizes = re.findall(r'\{\s*0x[\dA-Fa-f]+,\s*0x[\dA-Fa-f]+,\s*0x[\dA-Fa-f]+,\s*(\d+),', index)
    padding = max(map(int, sizes))
    source = (root / 'rexlego/src/gpu_native/shader_archive.cpp').read_text()
    start = source.index('const ShaderCacheEntry* FindShaderByMicrocode(')
    end = source.index('\nuint64_t HashShaderContainer(', start)
    cases = []
    for path in sorted(a.captures.glob('*.bin')):
        data = path.read_bytes()
        flags, virtual, physical = struct.unpack_from('>III', data)
        assert flags in (0x102A1100, 0x102A1101)
        assert len(data) == virtual + 4 + physical
        assert struct.unpack_from('>I', data, virtual)[0] == physical
        shader = struct.unpack_from('>I', data, 24)[0]
        assert 36 <= shader and shader + 8 <= virtual
        offset, size = struct.unpack_from('>II', data, shader)
        assert size >= 8 and offset + size <= physical
        assert any(data[virtual + 4 + offset:virtual + 4 + offset + size])
        path_literal = '"' + str(path.resolve()).replace('\\', '/') + '"'
        cases.append(f'Check({path_literal}, {virtual + 4}, {offset}, {int(flags == 0x102A1100)});')
    if not cases:
        raise ValueError('No validated captures')
    code = r'''
#include <cassert>
#include <fstream>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <iostream>
#define XXH_INLINE_ALL
#include <xxhash.h>
#include "gpu_native/shader_archive.h"
''' + index + '\nnamespace {\nShaderCacheEntry cache[] = {\n' + ''.join(
        f'{{0x{h},0,0,0,0,0}},\n' for h in hashes) + r'''};
const ShaderCacheEntry* FindShader(uint64_t hash) {
 for (const auto& entry : cache) if (entry.hash == hash) return &entry;
 return nullptr;
}
''' + source[start:end] + f'\nconstexpr size_t padding={padding};\n' + r'''
void Check(const char* path, size_t physical, size_t offset, uint32_t stage) {
 std::ifstream file(path,std::ios::binary);
 std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
 const auto expected = XXH3_64bits(bytes.data(),bytes.size());
 // The production lookup tests several candidate lengths. Supply zeroed
 // readable trailing storage, as the guest committed allocation does.
 bytes.resize(bytes.size()+padding);
 for (auto start : {physical,physical+offset}) {
  auto* result=FindShaderByMicrocode(bytes.data()+start,stage,bytes.size()-start);
  if (!result || result->hash!=expected) {
   std::cerr << path << " section " << start << " failed exact lookup\n";
   std::abort();
  }
 }
}
}
int main() {
''' + '\n'.join(cases) + '\n}\n'
    cpp = a.output / 'captured-lookup.cpp'
    cpp.write_text(code)
    exe = a.output / 'captured-lookup.exe'
    subprocess.run(['clang++', '-std=c++20', '-O2', '-I' + str(root / 'rexlego/src'),
                    '-I' + str(root / 'rexglue-sdk/thirdparty/xxHash'), str(cpp),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe.resolve())], check=True)
    print(f'PASS: {len(cases)} captured containers, physical and instruction '
          'sections resolve to their exact compiled shader through production lookup')


if __name__ == '__main__':
    main()
