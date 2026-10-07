"""Compile actual prelink emission and verify literal/fallback bytes and deflate ABI."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--compiler', default='clang++')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
source = (root / 'tools/gpu-aot/prelink_shader_cache.cpp').read_text()
start = source.index('void EmitBytes(')
opening = source.index('{', start)
depth, end = 1, opening + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
helper = source[start:end]
start = source.index('  std::fprintf(f, "const uint8_t g_compressedLinkedDxilCache[]')
end = source.index('  if (!output.commit())', start)
footer = source[start:end] + '  assert(std::fclose(f) == 0);\n'
args.output.mkdir(parents=True, exist_ok=True)
output = args.output.resolve()
miniz = root / 'thirdparty/miniz'
includes = ['-I' + str(miniz), '-I' + str(root / 'rexlego/src')]
common = [args.compiler, '-std=c++20', '-UNDEBUG', *includes]
producer = r'''
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>
#define MINIZ_HEADER_FILE_ONLY
#include <miniz.h>
''' + helper + r'''
int main(int argc, char** argv) {
  assert(argc == 2);
  FILE* f = std::fopen(argv[1], "wb"); assert(f);
  std::fputs("#include \"gpu/shaders/linked_shader_cache.h\"\n", f);
  for (size_t size : {0, 1, 255, 256, 257, 65535, 65536, 131073}) {
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i) bytes[i] = uint8_t(i);
    std::fprintf(f, "const uint8_t case_%zu[] =\n", size);
    EmitBytes(f, bytes.data(), size);
    std::fputs(";\n", f);
  }
  std::vector<uint8_t> blob(8192);
  for (size_t i = 0; i < blob.size(); ++i) blob[i] = uint8_t((i * 197) ^ (i >> 5));
  mz_ulong comp_len = mz_compressBound(static_cast<mz_ulong>(blob.size()));
  std::vector<uint8_t> comp(comp_len);
  assert(mz_compress2(comp.data(), &comp_len, blob.data(),
                     static_cast<mz_ulong>(blob.size()), MZ_UBER_COMPRESSION) == MZ_OK);
''' + footer + '\n}\n'
producer_cpp = output / 'emit.cpp'
producer_cpp.write_text(producer)
producer_exe = output / 'emit.exe'
miniz_object = output / 'miniz.o'
subprocess.run([*common, '-c', str(miniz / 'miniz.cpp'), '-o', str(miniz_object)],
               check=True, timeout=45)
subprocess.run([*common, str(producer_cpp), str(miniz_object), '-o', str(producer_exe)],
               check=True, timeout=45)
bank = output / 'emitted-bank.cpp'
subprocess.run([str(producer_exe), str(bank)], check=True, timeout=10)
emitted = bank.read_text()
assert '#if defined(_MSC_VER) && !defined(__clang__)' in emitted
assert '\\x00' in emitted and '\\xFF' in emitted and '\\x22' in emitted and '\\x5C' in emitted
consumer = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>
#define MINIZ_HEADER_FILE_ONLY
#include <miniz.h>
// Only generated initializer selection is varied. Standard/miniz headers
// above use the actual host compiler configuration.
#if defined(TEST_MSVC_FALLBACK)
#undef __clang__
#define _MSC_VER 1940
#endif
#include "emitted-bank.cpp"
template<size_t N> void Check(const uint8_t (&bytes)[N], size_t payload) {
#if defined(TEST_MSVC_FALLBACK)
  assert(N == (payload ? payload : 1));
#else
  assert(N == payload + 1 && bytes[payload] == 0);
#endif
  for (size_t i = 0; i < payload; ++i) assert(bytes[i] == uint8_t(i));
}
int main() {
''' + ''.join(f'  Check(case_{size}, {size});\n' for size in
              (0, 1, 255, 256, 257, 65535, 65536, 131073)) + r'''
  assert(g_linkedDxilCacheDecompressedSize == 8192);
#if defined(TEST_MSVC_FALLBACK)
  assert(sizeof(g_compressedLinkedDxilCache) == g_linkedDxilCacheCompressedSize);
#else
  assert(sizeof(g_compressedLinkedDxilCache) == g_linkedDxilCacheCompressedSize + 1);
  assert(g_compressedLinkedDxilCache[g_linkedDxilCacheCompressedSize] == 0);
#endif
  std::vector<uint8_t> decoded(g_linkedDxilCacheDecompressedSize);
  mz_ulong length = decoded.size();
  assert(mz_uncompress(decoded.data(), &length, g_compressedLinkedDxilCache,
                       g_linkedDxilCacheCompressedSize) == MZ_OK);
  assert(length == decoded.size());
  for (size_t i = 0; i < decoded.size(); ++i)
    assert(decoded[i] == uint8_t((i * 197) ^ (i >> 5)));
}
'''
consumer_cpp = output / 'check.cpp'
consumer_cpp.write_text(consumer)
for name, definitions in [('strings', []), ('msvc-fallback', ['-DTEST_MSVC_FALLBACK'])]:
    binary = output / (name + '.exe')
    # Clang's intentional large-literal warning must be narrowly suppressed by
    # the real emitted branch, even when the consumer uses warnings as errors.
    warnings = ['-Werror', '-Woverlength-strings'] if 'clang' in Path(args.compiler).name else []
    subprocess.run([*common, *warnings, *definitions, str(consumer_cpp),
                    str(miniz_object), '-o', str(binary)], check=True, timeout=45)
    subprocess.run([str(binary)], check=True, timeout=10)
print('PASS: actual emitted literal/MSVC fallback preserve every byte at line/64KiB boundaries; '
      'production compressed metadata excludes literal NUL and miniz round trip matches')
