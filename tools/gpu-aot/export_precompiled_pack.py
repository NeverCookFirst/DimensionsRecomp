"""Export an additive development pack from verified offline AOT outputs.

This compiles the generated C++ tables with a bounded host compiler. It never
compiles guest shaders, edits a bank, installs a pack or touches the game.
"""
import argparse
import ctypes
import ctypes.util
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

COMMON = '11fb0c3e07bd0d8cf4930a73035f98ab623f9877c3d167e82b0c0fccf56f1a9a'
REVISION = '339af41df2c23dbe3256c1c377716b81a0e0fe6b'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def identity(path):
    data = path.read_bytes()
    return dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())


def load_hash64(root, compiler, scratch):
    # Use the pinned XXHash implementation, including on machines without a
    # system XXHash library. This small host library contains no game assets.
    source = scratch / 'pack-hash.cpp'
    source.write_text('#define XXH_INLINE_ALL\n#include <xxhash.h>\n'
                      '#include <cstdint>\n'
                      '#ifdef _WIN32\n#define API __declspec(dllexport)\n'
                      '#else\n#define API\n#endif\n'
                      'extern "C" API uint64_t PackHash(const void* p,size_t n)'
                      '{return XXH3_64bits(p,n); }\n')
    library_path = scratch / ('pack-hash.dll' if os.name == 'nt' else 'pack-hash.so')
    subprocess.run([compiler, '-std=c++20', '-shared',
                    *([] if os.name == 'nt' else ['-fPIC']),
                    '-I' + str(root / 'rexglue-sdk/thirdparty/xxHash'),
                    str(source), '-o', str(library_path)], check=True, timeout=45)
    library = ctypes.CDLL(str(library_path))
    library.PackHash.argtypes = (ctypes.c_void_p, ctypes.c_size_t)
    library.PackHash.restype = ctypes.c_uint64
    return lambda data: library.PackHash(data, len(data))


def verify_provenance(path):
    result = json.loads(path.read_text())
    require(result['constants_bytes'] == 624, 'Constants ABI must be624')
    require(result['tools']['header']['sha256'] == COMMON, 'Unrecognized common header')
    require(result['settings']['XENOS_RECOMP_DXIL_ONLY'] == '1' and
            result['settings']['XENOS_RECOMP_LEGO_NATIVE_SCALE'] == '1',
            'Candidate must be DXIL_ONLY and NATIVE_SCALE')
    for name in ('runtime-cache.cpp', 'runtime-linked-dxil-cache.cpp', 'lego-microcode-index.cpp'):
        expected = result['outputs'][name]
        require(identity(Path(expected['path'])) ==
                {key: expected[key] for key in ('bytes', 'sha256')},
                f'Changed candidate output: {name}')
    for tool in ('header', 'compiler', 'dxc', 'linker', 'prelink'):
        expected = result['tools'][tool]
        require(identity(Path(expected['path'])) ==
                {key: expected[key] for key in ('bytes', 'sha256')},
                f'Changed candidate tool: {tool}')
    return result


BRIDGE = r'''
#include "gpu_native/shader_archive.h"
#include "gpu_native/linked_shader_cache.h"
#include <zstd.h>
#define MINIZ_HEADER_FILE_ONLY
#include <miniz.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstdint>
extern ShaderMicrocodeEntry baselineMicrocodes[];
extern const size_t baselineMicrocodeCount;
void u32(FILE* f,uint32_t v){for(unsigned i=0;i<4;++i)std::fputc(v>>(8*i),f);}
void u64(FILE* f,uint64_t v){u32(f,v);u32(f,v>>32);}
int main(int argc,char** argv){
 if(argc!=2 || g_dxilCacheDecompressedSize>64*1024*1024 ||
    g_linkedDxilCacheDecompressedSize>64*1024*1024)return 2;
 std::vector<uint8_t> raw(g_dxilCacheDecompressedSize),linked(g_linkedDxilCacheDecompressedSize);
 if(ZSTD_decompress(raw.data(),raw.size(),g_compressedDxilCache,g_dxilCacheCompressedSize)!=raw.size())return 3;
 mz_ulong n=linked.size();
 if(mz_uncompress(linked.data(),&n,g_compressedLinkedDxilCache,g_linkedDxilCacheCompressedSize)!=MZ_OK || n!=linked.size())return 4;
 FILE* f=std::fopen(argv[1],"wb");if(!f)return 5;
 u32(f,g_shaderCacheEntryCount);u32(f,g_shaderMicrocodeEntryCount);
 u32(f,g_linkedShaderCacheEntryCount);u32(f,baselineMicrocodeCount);
 u32(f,raw.size());u32(f,linked.size());
 for(size_t i=0;i<g_shaderCacheEntryCount;++i){auto& e=g_shaderCacheEntries[i];
   u64(f,e.hash);u32(f,e.dxilOffset);u32(f,e.dxilSize);u32(f,e.specConstantsMask);}
 for(size_t i=0;i<g_shaderMicrocodeEntryCount;++i){auto& e=g_shaderMicrocodeEntries[i];
   u64(f,e.prefixHash);u64(f,e.microcodeHash);u64(f,e.containerHash);
   u32(f,e.microcodeSize);u32(f,e.stage);u32(f,e.textureMask);}
 for(size_t i=0;i<g_linkedShaderCacheEntryCount;++i){auto& e=g_linkedShaderCacheEntries[i];
   u64(f,e.hash);u32(f,e.specConstants);u32(f,e.dxilOffset);u32(f,e.dxilSize);}
 for(size_t i=0;i<baselineMicrocodeCount;++i)u64(f,baselineMicrocodes[i].containerHash);
 std::fwrite(raw.data(),1,raw.size(),f);std::fwrite(linked.data(),1,linked.size(),f);
 bool failed=std::ferror(f);return std::fclose(f)||failed?6:0;
}
'''


def compile_tables(root, candidate, baseline, compiler, scratch):
    bridge = scratch / 'bridge.cpp'
    bridge.write_text(BRIDGE)
    base = scratch / 'baseline.cpp'
    index = Path(baseline['outputs']['lego-microcode-index.cpp']['path'])
    base.write_text('#define g_shaderMicrocodeEntries baselineMicrocodes\n'
                    '#define g_shaderMicrocodeEntryCount baselineMicrocodeCount\n'
                    '#include ' + json.dumps(str(index)) + '\n')
    sources = [bridge, base] + [Path(candidate['outputs'][name]['path']) for name in
                               ('runtime-cache.cpp', 'runtime-linked-dxil-cache.cpp',
                                'lego-microcode-index.cpp')] + [root / 'thirdparty/miniz/miniz.cpp']
    flags = ['-std=c++20', '-O0', '-I' + str(root / 'rexlego/src'),
             '-I' + str(root / 'thirdparty/miniz'), '-I' + str(root / 'thirdparty/zstd')]
    objects = []
    for i, source in enumerate(sources):
        output = scratch / f'part{i}.o'
        subprocess.run([compiler, *flags, '-c', str(source), '-o', str(output)],
                       check=True, timeout=90)
        objects.append(output)
    # Use the pinned decoder, with its C language linkage intact.
    decoder = scratch / 'decoder.o'
    subprocess.run([compiler, '-x', 'c', '-O0', '-c',
                    str(root / 'thirdparty/zstd/zstddeclib.c'), '-o', str(decoder)],
                   check=True, timeout=90)
    exe = scratch / 'export-tables'
    subprocess.run([compiler, *map(str, objects), str(decoder), '-o', str(exe)],
                   check=True, timeout=45)
    dump = scratch / 'tables.bin'
    subprocess.run([str(exe), str(dump)], check=True, timeout=15)
    require(dump.stat().st_size <= 128 * 1024 * 1024 + 8 * 1024 * 1024,
            'Decoded table dump too large')
    return dump.read_bytes()


def make_pack(dump, fingerprint, previous, hash64):
    ns, nm, nv, nb, nr, nl = struct.unpack_from('<6I', dump)
    require(ns <= 65536 and nm <= 131072 and nv <= 262144 and nb <= 131072,
            'Unbounded generated table counts')
    at = 24
    def records(fmt, count):
        nonlocal at
        size = struct.calcsize(fmt)
        require(at + size * count <= len(dump), 'Truncated generated tables')
        result = [struct.unpack_from(fmt, dump, at + i * size) for i in range(count)]
        at += size * count
        return result
    shaders = records('<QIII', ns)
    micros = records('<QQQIII', nm)
    variants = records('<QIII', nv)
    excluded = {item[0] for item in records('<Q', nb)}
    for pack in previous:
        data = pack.read_bytes()
        require(len(data) >= 160 and data[:8] == b'LEGODX1\0' and
                data[32:48] == fingerprint.encode() and
                data[48:112] == COMMON.encode() and data[112:152] == REVISION.encode() and
                hash64(data[160:]) == struct.unpack_from('<Q', data, 152)[0],
                'Previous pack identity/checksum mismatch')
        count = struct.unpack_from('<I', data, 16)[0]
        require(0 < count <= 256 and 160 + count * 24 <= len(data), 'Invalid previous pack')
        excluded.update(struct.unpack_from('<Q', data, 160 + i * 24)[0] for i in range(count))
    require(at + nr + nl == len(dump), 'Decoded blob size mismatch')
    raw, linked = dump[at:at+nr], dump[at+nr:]
    selected = sorted(item for item in shaders if item[0] not in excluded)
    require(0 < len(selected) <= 256, 'No new shaders, or delta exceeds256')
    selected_ids = {item[0] for item in selected}
    micro_rows = [item for item in micros if item[2] in selected_ids]
    require(0 < len(micro_rows) <= 2048, 'Delta microcode count outside bounds')
    shader_rows, variant_rows, payload = [], [], bytearray()
    for hash_value, offset, size, mask in selected:
        rows = [item for item in micro_rows if item[2] == hash_value]
        stages = {item[4] for item in rows}
        require(len(stages) == 1, 'Missing or conflicting shader stage')
        stage = stages.pop()
        texture_mask = 0
        for row in rows:
            texture_mask |= row[5]
        shader_rows.append((hash_value, stage, mask, texture_mask, 0))
        programs = sorted((spec, voff, vsize, linked) for h, spec, voff, vsize in variants
                          if h == hash_value) if mask else [(0, offset, size, raw)]
        require([x[0] for x in programs] == ([0, mask] if mask else [0]),
                'Incomplete offline specialization variants')
        for spec, voff, vsize, source in programs:
            require(vsize > 0 and voff + vsize <= len(source), 'Generated DXIL outside decoded blob')
            code = source[voff:voff+vsize]
            require(code[:4] == b'DXBC' and len(code) >= 36 and
                    struct.unpack_from('<I', code, 24)[0] == len(code), 'Invalid offline DXIL container')
            variant_rows.append((hash_value, spec, len(payload), len(code), 0))
            payload.extend(code)
    table = b''.join(struct.pack('<Q4I', *row) for row in shader_rows)
    table += b''.join(struct.pack('<QQQ4I', *row, 0) for row in micro_rows)
    table += b''.join(struct.pack('<Q4I', *row) for row in variant_rows)
    content = table + payload
    header = b'LEGODX1\0' + struct.pack('<6I', 1, 624, len(shader_rows), len(micro_rows),
                                         len(variant_rows), len(payload))
    header += fingerprint.encode() + COMMON.encode() + REVISION.encode()
    header += struct.pack('<Q', hash64(content))
    result = header + content
    require(len(result) <= 64 * 1024 * 1024, 'Pack exceeds64MiB')
    return result, dict(shaders=len(shader_rows), microcodes=len(micro_rows),
                        variants=len(variant_rows), dxil_bytes=len(payload))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--build-info', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--previous-pack', type=Path, action='append', default=[])
    parser.add_argument('--compiler', default='clang++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    if os.name == 'posix':
        import resource
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        current = resource.getrlimit(resource.RLIMIT_AS)[0]
        ceiling = 3 * 1024**3
        limit = ceiling if current == resource.RLIM_INFINITY else min(current, ceiling)
        resource.setrlimit(resource.RLIMIT_AS, (limit, limit))
        os.nice(10)
    candidate = verify_provenance(args.candidate / 'provenance.json')
    baseline = verify_provenance(args.baseline / 'provenance.json')
    for tool in ('header', 'compiler', 'dxc', 'linker', 'prelink'):
        require(candidate['tools'][tool]['sha256'] == baseline['tools'][tool]['sha256'],
                f'Candidate/baseline compiler provenance disagreement: {tool}')
    revision = subprocess.run(['git', '-C', str(root / 'research/reblue/thirdparty/XenosRecomp'),
                               'rev-parse', 'HEAD'], check=True, capture_output=True,
                              text=True, timeout=10).stdout.strip()
    require(revision == REVISION, 'Compiler checkout revision mismatch')
    build = args.build_info.read_text()
    match = re.search(r'kNativeGpuBuildFingerprint\[\] = "([0-9a-f]{16})"', build)
    require(match is not None, 'Missing actual build fingerprint')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='pack-export-', dir=args.output.parent) as temporary:
        hash64 = load_hash64(root, args.compiler, Path(temporary))
        dump = compile_tables(root, candidate, baseline, args.compiler, Path(temporary))
        pack, counts = make_pack(dump, match.group(1), args.previous_pack, hash64)
        # Revalidate all inputs after compilation before publishing. The export
        # cannot silently change provenance during a concurrent candidate build.
        require(verify_provenance(args.candidate / 'provenance.json') == candidate and
                verify_provenance(args.baseline / 'provenance.json') == baseline and
                args.build_info.read_text() == build, 'Inputs changed during export')
        output = Path(temporary) / 'complete.pack'
        output.write_bytes(pack)
        os.replace(output, args.output)
    report = dict(status='exported-not-installed', **counts, output=str(args.output.resolve()),
                  output_identity=identity(args.output), build_fingerprint=match.group(1),
                  common_header_sha256=COMMON, compiler_revision=REVISION,
                  candidate=str(args.candidate.resolve()), baseline=str(args.baseline.resolve()))
    args.output.with_suffix(args.output.suffix + '.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
