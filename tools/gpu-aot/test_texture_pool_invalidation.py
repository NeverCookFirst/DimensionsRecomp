"""Execute production pool-copy invalidation and host-authority decisions."""
import argparse
from pathlib import Path
import subprocess


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
    text = (root/'rexlego/src/gpu_native/textures.cpp').read_text()
    production = '\n'.join(body(text, signature) for signature in (
        'bool PoolCopyHasHostTextureSource(',
        'template<class Iterator>\nbool InvalidatePoolCopyTexture(',
        'void RetirePoolCopyTextures(',
        'u32 InvalidatePoolCopyTextures('))
    code = r'''
#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "gpu_native/pool_copy.h"
#include "gpu_native/texture_writeback_coverage.h"
using namespace legodimensions::gpu_native;
using u32=uint32_t;using u64=uint64_t;
#define REXLOG_WARN(...) ((void)0)
#undef assert
#define assert(c) do {if(!(c)){std::fprintf(stderr,"check failed: %s\n",#c);std::exit(23);}}while(0)
struct D3DTexture {uint8_t bytes[52];};
struct TextureResource {
 TextureWritebackCoverage writeback_coverage;
 std::mutex mutex;
 u32 guest_address=0x1000,mirror_address=0x9000;
 u32 descriptor_index=3,resolved_descriptor_index=4,sampled_descriptor_index=5;
 std::vector<u32> previous_surface_descriptors{6};
 PhysicalCopyRange data{0x4000,0x100};
 bool owns_guest_memory=false,surface=false;
 bool guest_uploaded=true,upload_source_key_valid=true,resolved_on_host=true,sampled_valid=true,guest_backing_current=false;
 u64 depth_alias_generation=7,depth_resolve_generation=8;
 u32 cpu_stamp=99;
};
std::mutex g_textures_mutex;
std::unordered_map<u32,std::shared_ptr<TextureResource>>g_textures;
std::vector<std::shared_ptr<TextureResource>>retained;
std::vector<u32>unregistered,freed,invalidated;
struct HostDevice {
 static int LockRecording(){return 0;}
 static void UnregisterTexture(u32 n){unregistered.push_back(n);}
 static void RetireResource(std::shared_ptr<TextureResource> n){retained.push_back(n);}
};
struct Memory {void SystemHeapFree(u32 n){freed.push_back(n);}}memory;
#define REX_KERNEL_MEMORY() (&memory)
bool legodimensions::gpu_native::GuestRangeOverlapsPoolCopy(u32 a,u32 n,PhysicalCopyRange r){return CopyRangesOverlap({a,n},r);}
bool TextureDataOverlapsPoolCopy(const TextureResource&r,PhysicalCopyRange p){
 return !r.surface&&CopyRangesOverlap(r.data,p);
}
void InvalidateFramebufferReferences(u32 n){invalidated.push_back(n);}
''' + production + r'''
int main(){
 auto r=std::make_shared<TextureResource>();g_textures[r->guest_address]=r;
 assert(PoolCopyHasHostTextureSource({0x4001,1}));
 assert(!PoolCopyHasHostTextureSource({0x1010,4})); // CPU header, not GPU pixels.
 r->guest_backing_current=true;r->depth_alias_generation=0;
 assert(!PoolCopyHasHostTextureSource({0x4001,1}));
 r->guest_backing_current=false;r->depth_alias_generation=7;
 assert(InvalidatePoolCopyTextures({0x4100,1})==0);
 assert(r->resolved_on_host&&r->sampled_valid&&r->depth_resolve_generation==8);
 assert(InvalidatePoolCopyTextures({0x4080,1})==1);
 assert(!r->resolved_on_host&&!r->sampled_valid&&!r->guest_uploaded);
 assert(!r->guest_backing_current);
 assert(!r->depth_resolve_generation&&!r->depth_alias_generation&&!r->cpu_stamp);
 assert(!r->upload_source_key_valid&&!PoolCopyHasHostTextureSource({0x4080,1}));
 assert(g_textures.size()==1&&retained.empty()&&unregistered.empty()&&freed.empty());
 // A CPU-uploaded image is only a comparison candidate after an overwrite:
 // the clean-page stamp must be discarded so current bytes get hashed again.
 r->guest_uploaded=true;r->upload_source_key_valid=true;r->cpu_stamp=99;
 assert(InvalidatePoolCopyTextures({0x4080,1})==1);
 assert(r->guest_uploaded&&r->upload_source_key_valid&&!r->cpu_stamp);
 assert(!r->resolved_on_host&&!r->sampled_valid);
 // Never resurrect a previous failed upload or its invalid source key.
 r->guest_uploaded=false;r->upload_source_key_valid=false;r->cpu_stamp=99;
 assert(InvalidatePoolCopyTextures({0x4080,1})==1);
 assert(!r->guest_uploaded&&!r->upload_source_key_valid&&!r->cpu_stamp);
 // Explicit header overwrite retires storage/descriptors rather than leaving
 // the adopted resource associated with an unrelated new guest object.
 assert(InvalidatePoolCopyTextures({0x1010,4})==1);
 assert(g_textures.empty()&&retained.size()==1&&retained[0]==r);
 assert((unregistered==std::vector<u32>{3,4,5,6}));
 assert((freed==std::vector<u32>{0x9000}));
 assert((invalidated==std::vector<u32>{0x1000}));
 auto surface=std::make_shared<TextureResource>();surface->surface=true;
 g_textures[surface->guest_address]=surface;
 assert(InvalidatePoolCopyTextures({0x4000,0x100})==0&&surface->resolved_on_host);
}
'''
    args.output.mkdir(parents=True, exist_ok=True)

    def run(name, cpp):
        source = args.output.resolve()/(name+'.cpp')
        binary = args.output.resolve()/(name+'.exe')
        source.write_text(cpp)
        subprocess.run([args.compiler, '-std=c++20', '-I'+str(root/'rexlego/src'),
                        str(source), '-o', str(binary)], check=True, timeout=45)
        return subprocess.run([str(binary)], capture_output=True, timeout=10)

    result = run('texture-pool', code)
    assert result.returncode == 0, result.stderr.decode(errors='replace')
    if args.verify_regression:
        header_guard = code.replace('TextureDataOverlapsPoolCopy(*resource, source)',
            '(GuestRangeOverlapsPoolCopy(address, sizeof(D3DTexture), source) || TextureDataOverlapsPoolCopy(*resource, source))')
        result = run('texture-pool-old-header-guard', header_guard)
        assert result.returncode == 23 and b'!PoolCopyHasHostTextureSource({0x1010,4})' in result.stderr
        mutant = code.replace('resource->resolved_on_host = false;', '')
        result = run('texture-pool-old-authority', mutant)
        assert result.returncode == 23 and b'!r->resolved_on_host' in result.stderr
    print('Production CPU pool writes supersede GPU texture authority; retirement and EDRAM checks PASS')


if __name__ == '__main__':
    main()
