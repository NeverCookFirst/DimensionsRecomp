"""Exercise the production variant cache with concurrent preparation callers."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--bank', type=Path, required=True)
p.add_argument('--compiler', default='clang-cl')
a = p.parse_args()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
src = Path(__file__).resolve().parents[2]/'rexlego/src/gpu_native/float_blend_indices.cpp'
production = src.read_text()
mock = out/'gpu_native'
mock.mkdir(exist_ok=True)
(mock/'float_blend_indices.h').write_text(r'''
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
inline std::atomic<int> created{}, destroyed{};
namespace plume {
enum class RenderShaderFormat { DXIL };
struct RenderShader { ~RenderShader(){++destroyed;} };
struct Device {
 std::unique_ptr<RenderShader> createShader(const void*,size_t,const char*,RenderShaderFormat){
  ++created; std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return std::make_unique<RenderShader>();
 }
};
}
namespace legodimensions::gpu_native {
struct ShaderBytecode { const void* data{};size_t size{};explicit operator bool() const{return data&&size;} };
ShaderBytecode FindFloatBlendIndicesDxil(uint64_t,uint32_t);
ShaderBytecode FindPositionPrecisionDxil(uint64_t,uint32_t);
plume::RenderShader* ResolveFloatBlendIndicesShader(uint64_t,uint32_t);
plume::RenderShader* ResolvePositionPrecisionShader(uint64_t,uint32_t);
void ResetFloatBlendIndicesShaders();
struct HostDevice { static plume::Device* Device(){static plume::Device device;return &device;} };
}
''')
(mock/'device.h').write_text('#pragma once\n#include "float_blend_indices.h"\n')
fixture = out/'fixture.cpp'
fixture.write_text(r'''
#include "gpu_native/float_blend_indices.h"
#include <array>
#include <barrier>
#include <cstdio>
#include <vector>
#include <cstring>
using namespace legodimensions::gpu_native;
#define CHECK(x) do {if(!(x)){printf("FAIL line %d created=%d destroyed=%d\n",__LINE__,int(created),int(destroyed));return 23;}}while(0)
int main(){
 constexpr std::array<uint64_t,6> hashes{0x51A51F31865A9BEAull,0x98117CD19D383B96ull,0x758F1CBD24D47419ull,0x872E9F9219CC6C3Eull,0xDB9ABE353AF31FB1ull,0x9B80DE62C017EC70ull};
 constexpr int workers=16;
 std::barrier start(workers);std::array<plume::RenderShader*,workers> results{};
 std::vector<std::thread> threads;
 for(int i=0;i<workers;++i)threads.emplace_back([&,i]{start.arrive_and_wait();results[i]=ResolvePositionPrecisionShader(hashes[0],0);});
 for(auto&t:threads)t.join();threads.clear();
 CHECK(created==1&&destroyed==0&&results[0]);
 for(auto*result:results)CHECK(result==results[0]);
 // Independent keys use the same cache, despite having different per-PSO gates.
 std::atomic<int> failures{};
 for(int i=0;i<workers;++i)threads.emplace_back([&,i]{for(int pass=0;pass<1000;++pass){
  auto hash=hashes[(pass+i)%6];auto code=FindPositionPrecisionDxil(hash,pass&1);
  if(!code||std::memcmp(code.data,"DXBC",4)||!ResolvePositionPrecisionShader(hash,pass&1))++failures;
 }});
 for(auto&t:threads)t.join();
 CHECK(!failures&&created==12&&destroyed==0);
 CHECK(!FindPositionPrecisionDxil(0,0)&&!ResolvePositionPrecisionShader(0,0));
 for(auto hash:hashes)CHECK(ResolvePositionPrecisionShader(hash,0)==ResolvePositionPrecisionShader(hash,2));
 ResetFloatBlendIndicesShaders();CHECK(created==destroyed);
 CHECK(ResolvePositionPrecisionShader(hashes[0],0));CHECK(created==13);
 ResetFloatBlendIndicesShaders();CHECK(created==destroyed);
 puts("PASS production variant cache: 16 callers, 16000 lookups, 12 variants, canonical ownership and reset");
}
''')

def run(name, text):
    cpp, exe = out/(name+'.cpp'), out/(name+'.exe')
    cpp.write_text(text)
    compile_result = subprocess.run([a.compiler, str(cpp), str(fixture), '/std:c++20', '/O2', '/MD', '/EHsc',
        '/I'+str(out), '/DLEGO_FLOAT_BLEND_INDICES_METADATA="'+str((a.bank/'float-blend-indices-bank.cpp').resolve())+'"',
        '/Fo'+out.as_posix()+'/', '/Fe'+str(exe), '/link', str((a.bank/'float-blend-indices-bank.obj').resolve())],
        capture_output=True, text=True, timeout=60)
    (out/(name+'-compile.log')).write_text(compile_result.stdout+compile_result.stderr)
    compile_result.check_returncode()
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    (out/(name+'.log')).write_text(result.stdout+result.stderr)
    return result

r = run('variant-cache', production)
if r.returncode:
    raise RuntimeError(r.stdout+r.stderr)
print(r.stdout.strip())
mutant = production.replace('  std::lock_guard lock(g_shaders_mutex);\n', '')
assert mutant != production
r = run('variant-cache-unlocked', mutant)
if r.returncode != 23:
    raise RuntimeError('Concurrent first-use negative control escaped the ownership check')
print('PASS negative control: unlocked production cache creates duplicate variant ownership')
