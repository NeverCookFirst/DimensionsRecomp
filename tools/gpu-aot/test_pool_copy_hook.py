"""Execute the production CPU copy hook, including its original-helper scope."""
import argparse
from pathlib import Path
import subprocess
STUB_INCLUDE = '#define LEGO_NATIVE_WORKER_STUB\n#include "gpu_native/native_worker.h"\n'


def body(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--compiler', default='clang++')
    parser.add_argument('--verify-regression', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    text = (root/'rexlego/src/gpu_native/hooks_buffer.cpp').read_text()
    production = '\n'.join(body(text, signature) for signature in (
        'bool ConsumeCpuPoolCopyDraw(', 'void PoolCopyHook('))
    code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "gpu_native/pool_copy.h"
using namespace legodimensions::gpu_native;
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using be_u32=u32;
#undef assert
#define assert(c) do{if(!(c)){std::fprintf(stderr,"check failed: %s\n",#c);std::exit(23);}}while(0)
struct Register{uint32_t u32=0;};
struct PPCContext{Register r1,r3,r4,r5;u64 lr=0;};
bool OrderedPoolCopy(PPCContext&,u8*){return false;}
struct Memory{u8 data[4096]{};u8* TranslatePhysical(u32 n){return data+n;}
 template<class T>T TranslateVirtual(u32 n){return reinterpret_cast<T>(data+n);}}memory;
#define REX_KERNEL_MEMORY() (&::memory)
#define REXLOG_ERROR(...) ((void)0)
#define REXLOG_INFO(...) ((void)0)
struct SlowNativeOperation{SlowNativeOperation(const char*){}void Mark(const char*){}};
struct HostDevice{static int LockRecording(){return 0;}};
enum class ShaderStage{kVertex};u64 BoundShaderHash(ShaderStage){return 0;}
int _dupenv_s(char**v,size_t*n,const char*){*v=nullptr;*n=0;return 0;}
u64 XXH3_64bits(const void*,u32){return 0;}
bool readable=true,writable=true;
PhysicalCopyRange host{};u32 cpu_invalidations=0,cache_invalidations=0,original_calls=0,skipped=0;
bool PhysicalRange(u32 a,u32 n,PhysicalCopyRange&r){r={a,n};return n&&u64(a)+n<=sizeof(memory.data);}
bool AccessiblePhysicalRange(PhysicalCopyRange,bool write){return write?writable:readable;}
bool PoolCopyHasHostTextureSource(PhysicalCopyRange r){return CopyRangesOverlap(host,r);}
bool writeback_success=false;u32 writebacks=0;
bool MaterializePoolCopyTextureSource(PhysicalCopyRange){++writebacks;if(!writeback_success)return false;
 std::memset(memory.data+host.address,0xa5,host.length);host={};return true;}
bool MaterializePoolCopyTextureDestination(PhysicalCopyRange){return true;}
bool MaterializePoolCopyTextureDependencies(PhysicalCopyRange src,PhysicalCopyRange dst){
 if(PoolCopyHasHostTextureSource(src)&&!MaterializePoolCopyTextureSource(src))return false;
 return MaterializePoolCopyTextureDestination(dst);
}
void InvalidateCpuPhysicalMemory(u32,u32){++cpu_invalidations;}
u32 InvalidatePoolCopyBuffers(PhysicalCopyRange){++cache_invalidations;return 1;}
u32 InvalidatePoolCopyTextures(PhysicalCopyRange r){++cache_invalidations;if(CopyRangesOverlap(host,r))host={};return 1;}
u32 InvalidatePoolCopyShaders(PhysicalCopyRange){++cache_invalidations;return 1;}
thread_local PoolCopyDraws*g_cpu_pool_copy=nullptr;
struct PoolCopyScope{PoolCopyDraws draws;PoolCopyDraws*previous;
 explicit PoolCopyScope(u32 n):draws{n/64,0},previous(g_cpu_pool_copy){g_cpu_pool_copy=&draws;}
 ~PoolCopyScope(){g_cpu_pool_copy=previous;}};
void __imp__sub_82BD2868(PPCContext&ctx,u8*){
 ++original_calls;if(ConsumeCpuPoolCopyDraw(1,0,ctx.r5.u32/64))++skipped;
 assert(!ConsumeCpuPoolCopyDraw(1,0,ctx.r5.u32/64));
}
namespace legodimensions::gpu_native {
''' + production + r'''
}
int main(){
 PPCContext ctx;ctx.r3.u32=128;ctx.r4.u32=64;ctx.r5.u32=64;
 std::memset(memory.data+64,0x5a,64);host={128,64};
 // A GPU-owned destination is replaced by CPU bytes and loses its authority.
 PoolCopyHook(ctx,nullptr);
 assert(memory.data[128]==0x5a&&cpu_invalidations==1&&cache_invalidations==3);
 assert(original_calls==1&&skipped==1&&!host.length&&!g_cpu_pool_copy);
 assert(!ConsumeCpuPoolCopyDraw(1,0,1));
 // A GPU-produced source remains guarded; stale CPU backing is never copied.
 host={64,64};memory.data[128]=0x7b;PoolCopyHook(ctx,nullptr);
 assert(memory.data[128]==0x7b&&cpu_invalidations==1&&original_calls==2&&skipped==1);
 host={};readable=false;PoolCopyHook(ctx,nullptr);
 assert(cpu_invalidations==1&&original_calls==3&&skipped==1);
 readable=true;writable=false;PoolCopyHook(ctx,nullptr);
 assert(cpu_invalidations==1&&original_calls==4&&skipped==1);
 writable=true;ctx.r5.u32=65;PoolCopyHook(ctx,nullptr);
 assert(cpu_invalidations==1&&original_calls==5&&skipped==1);
 // Physical overlap uses memmove, and the original helper still runs once.
 ctx.r3.u32=96;ctx.r5.u32=128;
 for(u32 i=0;i<128;++i)memory.data[64+i]=u8(i);
 PoolCopyHook(ctx,nullptr);
 for(u32 i=0;i<128;++i)assert(memory.data[96+i]==u8(i));
 assert(cpu_invalidations==2&&original_calls==6&&skipped==2&&!g_cpu_pool_copy);
 // A successful GPU-to-guest materialization happens before the byte copy.
 ctx.r3.u32=256;ctx.r4.u32=64;ctx.r5.u32=64;host={64,64};writeback_success=true;
 auto previous=writebacks;PoolCopyHook(ctx,nullptr);
 assert(writebacks==previous+1&&memory.data[256]==0xa5);
 assert(cpu_invalidations==3&&original_calls==7&&skipped==3&&!g_cpu_pool_copy);
}
'''
    args.output.mkdir(parents=True, exist_ok=True)
    def run(name, source_text):
        source=args.output.resolve()/(name+'.cpp')
        binary=args.output.resolve()/(name+'.exe')
        source.write_text(STUB_INCLUDE + source_text)
        subprocess.run([args.compiler,'-std=c++20','-I'+str(root/'rexlego/src'),
                        str(source),'-o',str(binary)],check=True,timeout=45)
        return subprocess.run([str(binary)],capture_output=True,timeout=10)
    result=run('pool-copy-hook',code)
    assert result.returncode==0,result.stderr.decode(errors='replace')
    if args.verify_regression:
        mutant=code.replace('accessible && PoolCopyHasHostTextureSource(src)',
                            'accessible && (PoolCopyHasHostTextureSource(src) || PoolCopyHasHostTextureSource(dst))')
        result=run('pool-copy-old-destination-guard',mutant)
        assert result.returncode==23,result.stderr.decode(errors='replace')
    print('Production CPU copy destination replacement, source guard, overlap and draw scope PASS')


if __name__=='__main__':
    main()
