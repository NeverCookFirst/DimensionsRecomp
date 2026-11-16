"""Execute production use-time header refresh without rebinding a texture."""
import argparse
import json
import os
from pathlib import Path
import subprocess


def block(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output', type=Path)
    p.add_argument('--compiler', default='clang++')
    args = p.parse_args()
    root = Path(__file__).resolve().parents[2]
    text = (root/'rexlego/src/gpu_native/textures.cpp').read_text()
    helper = block(text, 'std::shared_ptr<TextureResource> FindTextureAtUse(')
    adopt = block(text, 'std::shared_ptr<TextureResource> AdoptTexture(')
    assert 'FindTextureAtUse(guest_address)' in adopt
    assert adopt.index('LockRecording()') < adopt.index('FindTextureAtUse(')
    code = r'''
#include <array>
#include "gpu_native/resource_registry.h"
using legodimensions::gpu_native::ResourceRegistry;
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
using u32=uint32_t;
#define CHECK(c) do {if(!(c)){std::fprintf(stderr,"failed: %s\n",#c);std::exit(23);}}while(0)
#define REXLOG_INFO(...) ((void)0)
bool LongProbeEnabled(){return false;}
template<class...T>void LongProbeEvent(T&&...){}
template<class T>int LongProbeHex(const T&){return 0;}
struct D3DTexture {struct {u32 common=3;}resource;struct {std::array<u32,6>dword{};}format;}header;
struct TextureResource {
 std::atomic<u32>header_sequence{0};std::atomic<bool>replaced{false};
 u32 guest_address=1;
 bool owns_guest_memory=false,surface=false,resolved_on_host=false,guest_backing_current=false;
 bool guest_uploaded=true,upload_source_key_valid=true;
 u32 adopted_header_type=3,descriptor_index=4,resolved_descriptor_index=5,sampled_descriptor_index=6,mirror_address=0;
 std::vector<u32> previous_surface_descriptors{7};
 std::mutex mutex;std::array<u32,6>guest_fetch{};
 u32 cpu_stamp=11,depth_alias_generation=12;
};
std::mutex g_textures_mutex;
ResourceRegistry<TextureResource>g_textures;
std::vector<std::shared_ptr<TextureResource>>retired;
std::vector<u32>descriptors;
u32 invalidations=0,header_reads=0,registry_lookups=0;
std::shared_ptr<TextureResource>replacement_on_read;
struct Memory {void SystemHeapFree(u32){CHECK(false);}}memory;
#define REX_KERNEL_MEMORY() (&memory)
struct HostDevice {
 static int LockRecording(){return 0;}
 static void UnregisterTexture(u32 s){descriptors.push_back(s);}
 static void RetireResource(std::shared_ptr<TextureResource>r){retired.push_back(r);}
};
template<class T>T* GuestAt(u32){++header_reads;if(replacement_on_read){g_textures[1]=replacement_on_read;replacement_on_read.reset();}return &header;}
std::shared_ptr<TextureResource>FindTexture(u32 a){++registry_lookups;return g_textures.Lookup(a,g_textures_mutex);}
void InvalidateFramebufferReferences(u32){++invalidations;}
void NoteTextureFootprintChange(u32){}
bool header_readable=true;bool ReadableUploadSpan(u32,u32){return header_readable;}
// Residency/alias bypass is exercised with real SDK faults separately.
bool CopyNativeGuestMetadata(Memory&,u32 address,void* output,u32 bytes){
 if(!header_readable)return false;std::memcpy(output,GuestAt<D3DTexture>(address),bytes);return true;
}
''' + block(text,'bool TexturePlacementHeaderMatches(')+'\n'+block(text, 'bool RefreshTextureResourceHeader(u32 guest_address,\n    const std::shared_ptr<TextureResource>& resource) {')+'\n'+block(text, 'std::shared_ptr<void> RefreshTextureHeaderOwner(')+'\n'+block(text, 'void RefreshTextureHeader(') + '\n' + helper + r'''
auto create(){
 auto r=std::make_shared<TextureResource>();r->guest_fetch=header.format.dword;
 g_textures[1]=r;return r;
}
int main(){
 header.format.dword={2,0xF4000052u,0x001FE0FFu,0xD10,0x200,0xF4100A00u};
 CHECK(!FindTextureAtUse(1));CHECK(header_reads==0);
 auto old=create();auto before=old->guest_fetch;
 CHECK(TexturePlacementHeaderMatches(*old));
 header_readable=false;CHECK(!TexturePlacementHeaderMatches(*old));header_readable=true;
 header.format.dword[1]^=0x1000;CHECK(!TexturePlacementHeaderMatches(*old));header.format.dword[1]^=0x1000;
 auto lookups_before=registry_lookups,reads_before=header_reads;
 for(int i=0;i<1000;++i)CHECK(FindTextureAtUse(1)==old);
 const char* setting=std::getenv("LEGO_NATIVE_REUSE_TEXTURE_LOOKUP");
 const bool legacy=setting && std::strcmp(setting,"0")==0;
 CHECK(registry_lookups-lookups_before==(legacy?3000:1000));
 CHECK(header_reads-reads_before==(legacy?2000:1000)); // Legacy also validates its returned owner.
 CHECK(old->cpu_stamp==11 && retired.empty() && descriptors.empty());
 // An inaccessible placement is not permission to reuse cached pixels.
 header_readable=false;CHECK(!FindTextureAtUse(1));CHECK(g_textures[1]==old&&retired.empty());
 header_readable=true;CHECK(FindTextureAtUse(1)==old);
 // Streaming relocates the backing allocation without calling SetTexture.
 header.format.dword[1]=0xF400C052u;header.format.dword[5]=0xF410CA00u;
 CHECK(FindTextureAtUse(1)==old && old->guest_fetch==header.format.dword);
 CHECK(old->cpu_stamp==0 && old->depth_alias_generation==0);
 CHECK(old->guest_uploaded && old->upload_source_key_valid && retired.empty());
 // Address-only host resolve relocation keeps GPU pixels authoritative.
 old->resolved_on_host=true;old->cpu_stamp=19;old->guest_backing_current=true;
 header.format.dword[1]=0xF420C052u;
 CHECK(FindTextureAtUse(1)==old && old->resolved_on_host && old->cpu_stamp==0);
 CHECK(!old->guest_backing_current);
 // The same bound placement header now describes another size/format.
 auto relocated=old->guest_fetch;
 header.format.dword[2]^=0x100;
 CHECK(!FindTextureAtUse(1));
 CHECK(retired.size()==1 && retired[0]==old && old->guest_fetch==relocated);
 CHECK((descriptors==std::vector<u32>{4,5,6,7}) && invalidations==1);
 auto replacement=create();CHECK(FindTextureAtUse(1)==replacement && replacement!=old);
 CHECK(replacement->guest_fetch==header.format.dword);
 // Owned resources and surfaces are not D3DTexture placement headers.
 auto count=header_reads;replacement->owns_guest_memory=true;
 header.format.dword=before;CHECK(FindTextureAtUse(1)==replacement && header_reads==count);
 replacement->owns_guest_memory=false;replacement->surface=true;
 CHECK(FindTextureAtUse(1)==replacement && header_reads==count);
 replacement->surface=false;header.format.dword=replacement->guest_fetch;
 header.resource.common=6;CHECK(!FindTextureAtUse(1));
 CHECK(retired.size()==2 && retired[1]==replacement);
 // A replacement published during a changed-header read must not be erased
 // or have its descriptors retired on behalf of the previous registry owner.
 header.resource.common=3;auto previous=create();auto newer=std::make_shared<TextureResource>();
 header.format.dword[2]^=0x100;newer->guest_fetch=header.format.dword;
 auto retired_before=retired.size(),descriptors_before=descriptors.size();
 replacement_on_read=newer;CHECK(FindTextureAtUse(1)==newer);
 CHECK(g_textures[1]==newer&&retired.size()==retired_before&&descriptors.size()==descriptors_before);
 std::puts("Bound-header relocation/replacement refreshed; old resources retained.");
}
'''
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    def compile_run(name, source,reuse='1'):
        cpp, exe = output/(name+'.cpp'), output/(name+'.exe')
        cpp.write_text(source)
        subprocess.run([args.compiler, '-std=c++20', '-UNDEBUG', '-I'+str(root/'rexlego/src'), str(cpp), '-o', str(exe)], check=True, timeout=45)
        return subprocess.run([str(exe)], capture_output=True, text=True, timeout=10,
            env=dict(os.environ,LEGO_NATIVE_REUSE_TEXTURE_LOOKUP=reuse))
    result = compile_run('current', code)
    assert result.returncode == 0, result.stderr
    legacy = compile_run('legacy', code,reuse='0')
    assert legacy.returncode == 0, legacy.stderr
    negative = compile_run('cached-without-refresh', code.replace(helper, helper.replace(
        'RefreshTextureResourceHeader(guest_address,existing)', 'true')))
    assert negative.returncode == 23, negative.stderr
    (output/'verification.json').write_text(json.dumps({
        'result':'passed', 'production_bodies':['FindTextureAtUse','RefreshTextureResourceHeader','RefreshTextureHeader'],
        'adopt_calls_use_helper_under_recording_lock':True,
        'unchanged_uses':1000,'optimized_registry_lookups':1000,'legacy_registry_lookups':3000,
        'header_reads_optimized':1000,'header_reads_legacy':2000,'legacy_rollback_passed':True,
        'unreadable_metadata_rejects_stale_record':True,
        'changed_header_cannot_retire_replaced_registry_owner':True,
        'negative_returncode':negative.returncode,
        'gpu_fence_completion_tested':False, 'lotr_visual_acceptance':False}, indent=2)+'\n')
    print(result.stdout.strip())


if __name__ == '__main__':
    main()
