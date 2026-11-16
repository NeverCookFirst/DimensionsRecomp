"""Execute real texture access bodies/hooks to detect lost borrowed CPU writes.

The original XDK call is a routing boundary stub that supplies guest storage;
this fixture does not emulate XDK lock layout arithmetic or GPU readback.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def block(text, signature):
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
    parser.add_argument('--expect-failure', action='store_true')
    parser.add_argument('--verify-regression', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    args.output.mkdir(parents=True, exist_ok=True)
    textures = (root/'rexlego/src/gpu_native/textures.cpp').read_text()
    hooks = (root/'rexlego/src/gpu_native/hooks_texture.cpp').read_text()
    functions = '\n'.join(block(textures, sig) for sig in (
        'bool LockTextureResource(', 'bool DescribeTextureResource(', 'bool DescribeSurfaceResource('))
    if 'bool OwnsNativeTextureHeader(' in textures:
        functions += '\n' + block(textures, 'bool OwnsNativeTextureHeader(')
    wrappers = '\n'.join(block(hooks, f'REX_HOOK_RAW(sub_{address})') for address in ('83FB5540','83FB5560','83FB57C8'))
    prefix = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
using u32=uint32_t;using u8=uint8_t;
#undef assert
#define assert(c) do{if(!(c)){std::fprintf(stderr,"check failed: %s\n",#c);std::exit(23);}}while(0)
alignas(16) u8 ram[65536]{};
template<class T> T* GuestAt(u32 address){assert(address+sizeof(T)<=sizeof(ram));return reinterpret_cast<T*>(ram+address);}
struct D3DLockedRect{u32 pitch,bits;};
struct D3DSurfaceDesc{u32 format,type,usage,pool,multi_sample_type,multi_sample_quality,width,height;};
struct Register{u32 u32;};
struct PPCContext{Register r3,r4,r5,r6,r7;};
#define REX_HOOK_RAW(n) void n(PPCContext& ctx,u8* base)
namespace legodimensions::gpu_native {
struct TextureResource{std::mutex mutex;bool owns_guest_memory=false,surface=false;u32 levels=3,width=16,height=8,format=6,guest_format=6,d3d_type=3,mirror_address=0;};
std::mutex g_textures_mutex;
std::unordered_map<u32,std::shared_ptr<TextureResource>>g_textures;
auto FindTexture(u32 n){auto it=g_textures.find(n);return it==g_textures.end()?nullptr:it->second;}
bool IsNativeTexture(u32 n){return g_textures.contains(n);}
// GPU publication is verified separately by test_texture_cpu_alias_gpu.py.
u32 materializations=0,last_materialized_address=0;
bool MaterializeTextureForCpuRead(u32 n){++materializations;last_materialized_address=n;return false;}
u32 mirrors=0;
bool EnsureMirror(TextureResource& r){++mirrors;r.mirror_address=0x8000;return true;}
u32 MipDimension(u32 n,u32 l){return n>>l ? n>>l : 1;}
u32 RowPitch(u32,u32 width){return width*4;}
u32 LevelOffset(const TextureResource&,u32 l){return l*0x100;}
}
u32 original_lock=0,original_texture_desc=0,original_surface_desc=0;
void __imp__sub_83FB5540(PPCContext& ctx,u8*){assert(legodimensions::gpu_native::last_materialized_address==ctx.r3.u32);++original_lock;*GuestAt<D3DLockedRect>(ctx.r5.u32)={64,0x4000+ctx.r4.u32*0x100+(ctx.r6.u32?4u:0u)};}
void __imp__sub_83FB5560(PPCContext& ctx,u8*){++original_texture_desc;*GuestAt<D3DSurfaceDesc>(ctx.r5.u32)={20,18,77,88,0,0,32,32};}
void __imp__sub_83FB57C8(PPCContext& ctx,u8*){++original_surface_desc;*GuestAt<D3DSurfaceDesc>(ctx.r4.u32)={22,1,11,12,0,0,64,64};}
'''
    suffix = r'''
int main(){
 using namespace legodimensions::gpu_native;
 auto borrowed=std::make_shared<TextureResource>();g_textures[0x100]=borrowed;
 PPCContext c{{0x100},{0},{0x1000},{0},{0}};
 sub_83FB5540(c,ram);
 assert(original_lock==1&&mirrors==0&&materializations==1&&last_materialized_address==0x100);
 auto locked=*GuestAt<D3DLockedRect>(0x1000);
 assert(locked.bits==0x4000&&locked.pitch==64);
 // A real renderer upload for a borrowed texture reads its guest allocation,
 // not the unused mirror. Both black->color and a second animated update count.
 std::memset(ram+locked.bits,0x72,64);
 assert(ram[0x4000]==0x72&&ram[0x8000]==0);
 std::memset(ram+locked.bits,0xA3,64);assert(ram[0x4000]==0xA3);
 // Preserve level, rect and flags at the original routing boundary.
 c.r4.u32=2;c.r6.u32=0x1200;c.r7.u32=0x20;sub_83FB5540(c,ram);
 assert(GuestAt<D3DLockedRect>(0x1000)->bits==0x4204&&c.r7.u32==0x20&&materializations==2);
 sub_83FB5560(c,ram);assert(original_texture_desc==1);
 assert(GuestAt<D3DSurfaceDesc>(0x1000)->format==20&&GuestAt<D3DSurfaceDesc>(0x1000)->usage==77);
 auto surface=std::make_shared<TextureResource>();surface->surface=true;g_textures[0x180]=surface;
 c.r3.u32=0x180;c.r4.u32=0x1000;sub_83FB57C8(c,ram);
 assert(original_surface_desc==1&&GuestAt<D3DSurfaceDesc>(0x1000)->type==1);
 // Native-created zero-fetch resources must never reach the XDK decoder.
 auto owned=std::make_shared<TextureResource>();owned->owns_guest_memory=true;g_textures[0x200]=owned;
 c={{0x200},{1},{0x1000},{0},{0}};auto calls=original_lock;sub_83FB5540(c,ram);
 assert(original_lock==calls&&mirrors==1&&GuestAt<D3DLockedRect>(0x1000)->bits==0x8100);
 c.r4.u32=owned->levels;sub_83FB5540(c,ram);assert(original_lock==calls);
 c.r4.u32=0;sub_83FB5560(c,ram);assert(original_texture_desc==1&&GuestAt<D3DSurfaceDesc>(0x1000)->width==16);
 c.r4.u32=99;sub_83FB5560(c,ram);assert(original_texture_desc==1);
 c.r4.u32=0x1000;sub_83FB57C8(c,ram);assert(original_surface_desc==1);
 // Registry removal/address reuse must select the original path again.
 g_textures.erase(0x200);c={{0x200},{0},{0x1000},{0},{0}};sub_83FB5540(c,ram);assert(original_lock==calls+1);
 std::puts("PASS borrowed writes reach uploaded allocation; native zero-header guards and description routing preserved");
}
'''
    code = prefix + '\nnamespace legodimensions::gpu_native {\n' + functions + '\n}\n' + wrappers + suffix
    records = []
    def run(name, source):
        cpp = args.output/(name+'.cpp');cpp.write_text(source)
        exe = args.output/(name+'.exe')
        subprocess.run([args.compiler,'-std=c++20','-O2',str(cpp),'-o',str(exe)],check=True)
        result = subprocess.run([str(exe.resolve())],capture_output=True,text=True)
        record=dict(name=name,exit_code=result.returncode,stdout=result.stdout,stderr=result.stderr)
        records.append(record)
        print(name,result.returncode,result.stdout.strip(),result.stderr.strip())
        return result.returncode
    result = run('guest-access',code)
    assert result == (23 if args.expect_failure else 0)
    if args.verify_regression:
        old=code.replace('!resource || !resource->owns_guest_memory','!resource').replace('OwnsNativeTextureHeader(ctx.', 'IsNativeTexture(ctx.')
        assert old!=code
        assert run('old-mirror-routing',old)==23
        missing=code.replace('  legodimensions::gpu_native::MaterializeTextureForCpuRead(ctx.r3.u32);','')
        assert missing!=code
        assert run('missing-cpu-publication',missing)==23
    (args.output/'verification.json').write_text(json.dumps(dict(game_launched=False,
        original_xdk_layout_emulated=False,source_sha256=hashlib.sha256(textures.encode()).hexdigest(),
        hooks_sha256=hashlib.sha256(hooks.encode()).hexdigest(),records=records),indent=2)+'\n')


if __name__ == '__main__':
    main()
