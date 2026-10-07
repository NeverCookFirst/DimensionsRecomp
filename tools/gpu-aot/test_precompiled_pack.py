"""Exercise the complete production additive archive and binding recovery."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import ctypes
import ctypes.util


def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--compiler', default='clang++')
    parser.add_argument('--verify-regression', action='store_true')
    parser.add_argument('--real-pack', type=Path)
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[2]
    spec = importlib.util.spec_from_file_location('exporter', root / 'tools/gpu-aot/export_precompiled_pack.py')
    exporter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(exporter)
    if os.name == 'posix':
        import resource
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    hash64 = exporter.load_hash64(root, args.compiler, args.output)
    fingerprint = '0123456789abcdef'
    common = exporter.COMMON
    revision = exporter.REVISION
    def dxil(stage):
        # Synthetic DXIL envelopes exercise validation and ownership, not GPU
        # execution. No test container is published as a real game shader.
        chunk = struct.pack('<6I', (0 if stage else 1) << 16 | 0x60, 7,
                            0x4c495844, 0x100, 16, 4) + b'BC\xc0\xde'
        size = 36 + 8 + len(chunk)
        return b'DXBC' + bytes(16) + struct.pack('<III', 1, size, 1) + struct.pack('<I', 36) + b'DXIL' + struct.pack('<I', len(chunk)) + chunk
    def pack(first, count=1, texture_override=None, unmasked=False):
        shaders, micros, variants, payload = [], [], [], bytearray()
        for index in range(count):
            h = first + index
            stage = index % 2
            mask = 0 if unmasked else (2 if stage else 1)
            texture = texture_override if texture_override is not None else (5 if stage else 0x30000)
            code = dxil(stage)
            microcode = struct.pack('<QQ', h, h + 5)
            shaders.append(struct.pack('<Q4I', h, stage, mask, texture, 0))
            micros.append(struct.pack('<QQQ4I', hash64(microcode[:8]), hash64(microcode), h, 16, stage, texture, 0))
            for variant in ((0, mask) if mask else (0,)):
                variants.append(struct.pack('<Q4I', h, variant, len(payload), len(code), 0))
                payload.extend(code)
        content = b''.join(shaders + micros + variants) + payload
        return (b'LEGODX1\0' + struct.pack('<6I', 1, 624, count, count, len(variants), len(payload)) +
                fingerprint.encode() + common.encode() + revision.encode() + struct.pack('<Q', hash64(content)) + content)
    valid = pack(9, 2)
    (args.output / 'valid.pack').write_bytes(valid)
    invalid = []
    def mutation(name, offset, data, checksum=True):
        result = bytearray(valid)
        result[offset:offset+len(data)] = data
        if checksum:
            struct.pack_into('<Q', result, 152, hash64(bytes(result[160:])))
        path = args.output / (name + '.pack')
        path.write_bytes(result)
        invalid.append(path)
    mutation('wrong-fingerprint', 32, b'badbadbadbadbadb')
    mutation('wrong-common', 48, b'f')
    mutation('wrong-revision', 112, b'0')
    mutation('wrong-abi', 12, struct.pack('<I', 625))
    mutation('zero-count', 16, bytes(4))
    mutation('too-many', 16, struct.pack('<I', 257))
    mutation('bad-stage', 168, struct.pack('<I', 2))
    mutation('bad-mask', 172, struct.pack('<I', 2))
    mutation('unknown-texture', 176, struct.pack('<I', 0x10000))
    mutation('reserved-shader', 180, struct.pack('<I', 1))
    mutation('duplicate-shader', 184, struct.pack('<Q', 9))
    mutation('missing-container', 160+48+16, struct.pack('<Q', 200))
    mutation('microcode-stage', 160+48+28, struct.pack('<I', 1))
    mutation('microcode-small', 160+48+24, struct.pack('<I', 7))
    mutation('texture-disagreement', 160+48+32, struct.pack('<I', 4))
    mutation('missing-variant', 160+48+80+24+8, bytes(4))
    mutation('variant-overflow', 160+48+80+12, struct.pack('<I', 0xffffffff))
    mutation('noncontiguous-variant', 160+48+80+12, struct.pack('<I', 1))
    mutation('bad-dxbc', 160+48+80+96, b'NOPE')
    mutation('bad-dxil', 160+48+80+96+36, b'NOPE')
    mutation('dxil-stage-disagreement', 384+44, struct.pack('<I', 0x60))
    mutation('bad-bitcode', 384+68, b'NOPE')
    mutation('bad-wordcount', 384+48, struct.pack('<I', 8))
    mutation('corrupt-checksum', 159, b'\xff', False)
    (args.output / 'next.pack').write_bytes(pack(20, unmasked=True))
    (args.output / 'builtin.pack').write_bytes(pack(1))
    for i in range(62):
        (args.output / f'budget{i}.pack').write_bytes(pack(100+i, texture_override=0x80000000 if i == 0 else None))
    (args.output / 'budget-over.pack').write_bytes(pack(300))
    # A table from a second pack cannot alias an existing physical program.
    alias = bytearray(pack(20))
    alias[184:224] = valid[208:248]
    struct.pack_into('<Q', alias, 184+16, 20)
    struct.pack_into('<Q', alias, 152, hash64(bytes(alias[160:])))
    (args.output / 'conflict.pack').write_bytes(alias)
    stubs = args.output / 'stubs'
    (stubs / 'rex').mkdir(parents=True, exist_ok=True)
    (stubs / 'rex/logging.h').write_text('#define REXLOG_WARN(...) ((void)0)\n#define REXLOG_ERROR(...) ((void)0)\n')
    (stubs / 'native_gpu_build_info.h').write_text('namespace legodimensions::gpu_native { inline constexpr char kNativeGpuBuildFingerprint[]="' + fingerprint + '"; }\n')
    shaders = (root / 'rexlego/src/gpu_native/shaders.cpp').read_text()
    registry = shaders[shaders.index('struct ShaderResource {'):shaders.index('std::shared_ptr<ShaderResource> FindResource(')]
    bodies = '\n'.join(function(shaders, signature) for signature in (
        'bool BindShader(', 'void PollPrecompiledShaders(', 'u32 BoundShaderAddress(',
        'u64 BoundShaderHash(', 'void ResetShaderResources(',
        'bool ShouldSkipBoundPixelShader('))
    code = r'''
#include "gpu_native/shader_archive.h"
#include "gpu_native/linked_shader_cache.h"
#include "gpu_native/pixel_shader_filter.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#define REXLOG_WARN(...) ((void)0)
#define REXLOG_INFO(...) ((void)0)
ShaderCacheEntry g_shaderCacheEntries[]={{1,0,0,0,0,0}};
const size_t g_shaderCacheEntryCount=1;
ShaderCacheEntry g_runtimeShaderCacheEntries[]={{2,0,0,0,0,0}};
const size_t g_runtimeShaderCacheEntryCount=1;
const uint8_t g_compressedDxilCache[]={0},g_runtimeCompressedDxilCache[]={0};
const size_t g_dxilCacheCompressedSize=0,g_dxilCacheDecompressedSize=0,
 g_runtimeDxilCacheCompressedSize=0,g_runtimeDxilCacheDecompressedSize=0;
LinkedShaderCacheEntry g_linkedShaderCacheEntries[]={{1,0,0,0}},g_runtimeLinkedShaderCacheEntries[]={{2,0,0,0}};
const size_t g_linkedShaderCacheEntryCount=0,g_runtimeLinkedShaderCacheEntryCount=0;
const uint8_t g_compressedLinkedDxilCache[]={0},g_runtimeCompressedLinkedDxilCache[]={0};
const size_t g_linkedDxilCacheCompressedSize=0,g_linkedDxilCacheDecompressedSize=0,
 g_runtimeLinkedDxilCacheCompressedSize=0,g_runtimeLinkedDxilCacheDecompressedSize=0;
ShaderMicrocodeEntry g_shaderMicrocodeEntries[]={{0,0,1,32,0,0}};
const size_t g_shaderMicrocodeEntryCount=1;
using namespace legodimensions::gpu_native;
using u32=uint32_t;using u64=uint64_t;
enum class ShaderStage:u32{kVertex=6,kPixel=7};
namespace plume {struct RenderShader{};}
struct HostDevice {static inline std::recursive_mutex recording;
 static auto LockRecording(){return std::unique_lock(recording);}};
struct Memory{void SystemHeapFree(u32){}} memory;
#define REX_KERNEL_MEMORY() (&memory)
std::atomic<uint32_t> g_probe_frame{0};
template<class... A>void LongProbeEvent(const char*,bool,A&&...){}
''' + registry + r'''
u32 adopts=0;
std::shared_ptr<ShaderResource> AdoptShader(u32 address,ShaderStage stage){
 ++adopts;assert(address==77&&stage==ShaderStage::kVertex);
 auto* shader=FindShader(9);if(!shader)return nullptr;
 auto r=std::make_shared<ShaderResource>();r->guest_address=address;r->stage=stage;
 r->cache_entry=shader;r->hash=9;r->owns_guest_memory=false;
 r->texture_mask=FindShaderTextureMask(9);g_registry[address]=r;return r;
}
bool BindShader(ShaderStage,u32);
u32 BoundShaderAddress(ShaderStage,bool* = nullptr);
u64 BoundShaderHash(ShaderStage);
std::string skip_list;
namespace rex::cvar {std::string GetFlagByName(const char* name){return std::string(name)=="skip_pixel_shaders"?skip_list:"true";}}
namespace cheats {constexpr const char* kDofPixelShaderHash="";}
''' + bodies + r'''
std::vector<uint8_t> read(const std::filesystem::path& p){
 std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};
}
int main(int argc,char** argv){
 assert(argc>=3);std::filesystem::path dir=argv[1];std::string error;
 auto valid=read(dir/"valid.pack");
 if(std::string(argv[2])=="real"){
   auto real=read(argv[3]);std::memcpy(real.data()+32,"0123456789abcdef",16);
   assert(LoadPrecompiledShaderPack(real,error));
   uint32_t n=real[16]|uint32_t(real[17])<<8;assert(n);
   for(uint32_t i=0;i<n;++i){uint64_t h=0;for(unsigned b=0;b<8;++b)h|=uint64_t(real[160+i*24+b])<<(8*b);
     auto* e=FindShader(h);assert(e&&FindDxil(h,0));
     if(e->specConstantsMask)assert(FindDxil(h,e->specConstantsMask));
   }return 0;
 }
 if(std::string(argv[2])=="recovery"){
   assert(!BindShader(ShaderStage::kVertex,77));assert(BindShader(ShaderStage::kPixel,0));
   auto owned=std::make_shared<ShaderResource>();owned->hash=10;owned->stage=ShaderStage::kPixel;
   owned->guest_address=88;g_registry[88]=owned;
   bool failed=false;assert(BoundShaderAddress(ShaderStage::kVertex,&failed)==77);
   if(std::getenv("LEGO_NATIVE_SHADER_PACK")){
     assert(!failed&&BoundShaderHash(ShaderStage::kVertex)==9&&adopts==2);
     assert(owned->cache_entry==FindShader(10)&&owned->texture_mask==5);
     assert(BoundShaderAddress(ShaderStage::kPixel)==0);
     assert(!std::filesystem::exists(dir/"trigger"));
     assert(PrecompiledShaderPackGeneration()==1);
     auto* stable=g_bound_shaders[0].get();
     std::ofstream(dir/"trigger").put('x');BoundShaderAddress(ShaderStage::kVertex);
     assert(std::filesystem::exists(dir/"trigger")); // no second poll this frame
     ++g_probe_frame;BoundShaderAddress(ShaderStage::kVertex);
     assert(PrecompiledShaderPackGeneration()==1&&g_bound_shaders[0].get()==stable);
     assert(adopts==2); // rejected replacement cannot re-adopt
     ResetShaderResources();assert(BoundShaderAddress(ShaderStage::kVertex)==0);
     assert(FindShader(9)); // reset cannot invalidate retained pack pointers
   }else{assert(failed&&PrecompiledShaderPackGeneration()==0&&adopts==1);}
   return 0;
 }
 for(int i=3;i<argc;++i){auto malformed=read(argv[i]);
   assert(!LoadPrecompiledShaderPack(malformed,error));assert(!error.empty());
   assert(PrecompiledShaderPackGeneration()==0&&!FindShader(9));
 }
 for(size_t i=0;i<valid.size();++i){assert(!LoadPrecompiledShaderPack(
     std::span(valid).first(i),error));assert(PrecompiledShaderPackGeneration()==0);}
 assert(!LoadPrecompiledShaderPack(read(dir/"builtin.pack"),error));
 assert(error=="pack attempts to replace an existing shader");
 uint64_t pixel_filter=0;for(unsigned i=0;i<8;++i)pixel_filter|=uint64_t(valid[256+i])<<(8*i);
 char filter_text[32];std::snprintf(filter_text,sizeof(filter_text),"%llx",static_cast<unsigned long long>(pixel_filter));
 skip_list=filter_text;auto filtered=std::make_shared<ShaderResource>();filtered->hash=10;
 g_bound_shaders[1]=filtered;assert(!ShouldSkipBoundPixelShader());
 assert(LoadPrecompiledShaderPack(valid,error)&&error.empty());
 assert(ShouldSkipBoundPixelShader()); // unchanged list must refresh on generation
 assert(PrecompiledShaderPackGeneration()==1);
 auto* entry=FindShader(9);auto dxil=FindDxil(9,0);assert(entry&&dxil&&FindDxil(9,1));
 assert(FindDxil(9,0x100).data==dxil.data);assert(!FindDxil(200,0));
 assert(FindShaderTextureMask(9)==0x30000&&FindShaderPhysicalSize(9,0)==16);
 assert(FindShaderPhysicalSize(9,1)==0);
 uint64_t words[]={9,14};auto* bytes=reinterpret_cast<uint8_t*>(words);
 assert(FindShaderByMicrocode(bytes,0,16)==entry);
 assert(!FindShaderByMicrocode(bytes,1,16)&&!FindShaderByMicrocode(bytes,0,15));
 // Read the exact pixel physical hash from the validated pack table.
 uint64_t pixel=0;for(unsigned i=0;i<8;++i)pixel|=uint64_t(valid[256+i])<<(8*i);
 assert(FindPixelShaderContainers({pixel}).contains(10));
 assert(!LoadPrecompiledShaderPack(valid,error));assert(PrecompiledShaderPackGeneration()==1);
 assert(!LoadPrecompiledShaderPack(read(dir/"conflict.pack"),error));
 std::atomic<bool> done{false};std::thread reader([&]{while(!done){
   assert(FindShader(9)==entry&&FindDxil(9,0).data==dxil.data&&FindShaderTextureMask(9)==0x30000);}});
 assert(LoadPrecompiledShaderPack(read(dir/"next.pack"),error));done=true;reader.join();
 assert(PrecompiledShaderPackGeneration()==2&&FindShader(9)==entry&&FindDxil(9,0).data==dxil.data);
 assert(FindShader(20)->specConstantsMask==0&&FindDxil(20,0)&&FindDxil(20,99).data==FindDxil(20,0).data);
 for(unsigned i=0;i<dxil.size;++i)assert(dxil.data[i]==valid[384+i]);
 for(unsigned i=0;i<62;++i)assert(LoadPrecompiledShaderPack(read(dir/("budget"+std::to_string(i)+".pack")),error));
 assert(FindShaderTextureMask(100)==0x80000000);
 assert(PrecompiledShaderPackGeneration()==64);
 assert(!LoadPrecompiledShaderPack(read(dir/"budget-over.pack"),error));
 assert(error=="process pack lifetime budget exceeded");
 assert(PrecompiledShaderPackGeneration()==64&&FindShader(9)==entry);
 std::vector<uint8_t> huge(64*1024*1024+1);assert(!LoadPrecompiledShaderPack(huge,error));
 assert(error=="pack size outside bounds");
}
'''
    cpp = args.output / 'fixture.cpp'
    cpp.write_text(code)
    flags = ['-std=c++20', '-O0', '-I' + str(stubs), '-I' + str(root / 'rexlego/src'),
             '-I' + str(root / 'thirdparty/miniz'), '-I' + str(root / 'thirdparty/zstd'),
             '-I' + str(root / 'rexglue-sdk/thirdparty/xxHash')]
    decoder = args.output / 'decoder.o'
    subprocess.run([args.compiler, '-x', 'c', '-c', str(root / 'thirdparty/zstd/zstddeclib.c'),
                    '-o', str(decoder)], check=True, timeout=45)
    xxhash = args.output / 'xxhash.o'
    subprocess.run([args.compiler, '-x', 'c', '-c', str(root / 'rexglue-sdk/thirdparty/xxHash/xxhash.c'),
                    '-o', str(xxhash)], check=True, timeout=45)
    def build(archive, suffix):
        exe = args.output / ('fixture' + suffix)
        subprocess.run([args.compiler, *flags, str(cpp), str(archive),
                        str(root / 'thirdparty/miniz/miniz.cpp'), str(decoder), str(xxhash),
                        '-pthread', '-o', str(exe)], check=True, timeout=45)
        return exe
    archive = root / 'rexlego/src/gpu_native/shader_archive.cpp'
    exe = build(archive, '')
    subprocess.run([str(exe), str(args.output), 'archive', *map(str, invalid)], check=True, timeout=10)
    if args.real_pack:
        subprocess.run([str(exe), str(args.output), 'real', str(args.real_pack.resolve())], check=True, timeout=10)
    env = dict(os.environ)
    env.pop('LEGO_NATIVE_SHADER_PACK', None)
    env.pop('LEGO_NATIVE_SHADER_PACK_TRIGGER', None)
    subprocess.run([str(exe), str(args.output), 'recovery'], env=env, check=True, timeout=10)
    (args.output / 'trigger').write_text('load')
    env.update(LEGO_NATIVE_SHADER_PACK=str(args.output / 'valid.pack'),
               LEGO_NATIVE_SHADER_PACK_TRIGGER=str(args.output / 'trigger'))
    subprocess.run([str(exe), str(args.output), 'recovery'], env=env, check=True, timeout=10)
    negative = None
    if args.verify_regression:
        bad = args.output / 'bad-archive.cpp'
        bad.write_text(archive.read_text().replace('return fail("pack build or compiler provenance mismatch");', ';'))
        result = subprocess.run([str(build(bad, '-negative')), str(args.output), 'archive', str(invalid[0])],
                                capture_output=True, timeout=10)
        assert result.returncode != 0
        negative = result.returncode
    report = dict(status='passed', production_archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
                  production_recovery_sha256=hashlib.sha256(bodies.encode()).hexdigest(),
                  malformed_cases=len(invalid), truncated_cases=len(valid),
                  lifecycle_and_concurrent_reads=True, recovery_and_disabled=True,
                  negative_control_exit=negative, real_pack_validated=bool(args.real_pack),
                  pack_count_lifetime_budget=64, oversized_file_rejected=True,
                  actual_skip_filter_generation=True, vertex_texture_slots_16_17=True, texture_bit31=True,
                  limits='Synthetic DXIL envelopes and mocked guest adoption/host resources; no GPU execution or campaign claim.')
    (args.output / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
