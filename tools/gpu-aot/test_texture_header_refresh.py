"""Exercise the production refresh body with real before/after TU23 headers.

The bounded harness stubs GPU allocation, not the cache mutation under test.
GPU fence completion still requires the separate descriptor-retirement test
and the in-game AFK reproduction.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('log', type=Path)
parser.add_argument('output', type=Path)
parser.add_argument('--compiler', default='clang++')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
text = (root / 'rexlego/src/gpu_native/textures.cpp').read_text()
begin = text.index('void RefreshTextureHeader(u32 guest_address) {')
end = text.index('\nTextureResourceView ResolveTextureResource', begin)
body = text[begin:end]
rows = re.findall(r'header changed guest=([0-9A-F]+).*?cached=([0-9A-F,]+) current=([0-9A-F,]+)',
                  args.log.read_bytes().decode('utf-8', errors='replace'))
assert rows, 'Need actual runtime headers'
args.output.mkdir(parents=True, exist_ok=True)
harness = r'''
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>
bool LongProbeEnabled() { return false; }
template<class... T> void LongProbeEvent(T&&...) {}
template<class T> std::string LongProbeHex(const T&) { return {}; }
using u32=uint32_t;
struct D3DTexture { struct { u32 common=3; } resource; struct { std::array<u32,6> dword; } format; } header;
struct TextureResource {
  bool owns_guest_memory=false,surface=false,resolved_on_host=false,guest_uploaded=true,upload_source_key_valid=true;
  u32 adopted_header_type=3,descriptor_index=4,resolved_descriptor_index=5,sampled_descriptor_index=6,mirror_address=0;
  std::vector<u32> previous_surface_descriptors{7};
  std::mutex mutex; std::array<u32,6> guest_fetch;
  u32 cpu_stamp=0,depth_alias_generation=0;
};
std::mutex g_textures_mutex;
std::unordered_map<u32,std::shared_ptr<TextureResource>> g_textures;
std::vector<std::shared_ptr<TextureResource>> retired;
std::vector<u32> descriptors;
u32 framebuffer_invalidations=0;
struct Memory { void SystemHeapFree(u32) { assert(false); } } memory;
#define REX_KERNEL_MEMORY() (&memory)
#define REXLOG_INFO(...) ((void)0)
struct HostDevice {
  static int LockRecording() { return 0; }
  static void UnregisterTexture(u32 slot) { descriptors.push_back(slot); }
  static void RetireResource(std::shared_ptr<TextureResource> value) { retired.push_back(value); }
};
template<class T> T* GuestAt(u32) { return &header; }
std::shared_ptr<TextureResource> FindTexture(u32 a) { auto i=g_textures.find(a); return i==g_textures.end()?nullptr:i->second; }
void InvalidateFramebufferReferences(u32) { ++framebuffer_invalidations; }
'''
harness += body + r'''
void Check(std::array<u32,6> before,std::array<u32,6> after,bool replace,bool host=false,bool owned=false,u32 type=3) {
  g_textures.clear(); retired.clear(); descriptors.clear(); framebuffer_invalidations=0;
  auto old=std::make_shared<TextureResource>(); old->guest_fetch=before;
  old->cpu_stamp=11; old->depth_alias_generation=12;
  old->resolved_on_host=host; old->owns_guest_memory=owned;
  header.format.dword=after; header.resource.common=type; g_textures.emplace(1,old);
  RefreshTextureHeader(1);
  if(owned || (before==after && type==3)) {
    assert(FindTexture(1)==old && old->guest_fetch==before && old->upload_source_key_valid);
    assert(old->cpu_stamp==11 && old->depth_alias_generation==12);
    assert(retired.empty() && descriptors.empty()); return;
  }
  if(replace) {
    assert(!FindTexture(1)); assert(retired.size()==1 && retired[0]==old);
    assert((descriptors==std::vector<u32>{4,5,6,7}) && framebuffer_invalidations==1);
  } else {
    assert(FindTexture(1)==old && old->guest_fetch==after);
    assert(!old->upload_source_key_valid && old->guest_uploaded==host);
    assert(old->cpu_stamp==0 && old->depth_alias_generation==0);
    assert(retired.empty() && descriptors.empty());
  }
}
int main() {
'''
relocations = replacements = 0
for guest, cached, current in rows:
    old, new = [[int(w, 16) for w in value.split(',')] for value in (cached, current)]
    assert len(old) == len(new) == 6
    # Observed shape change is independently visible in the runtime words.
    replace = any(old[i] != new[i] for i in (0, 2, 3, 4))
    assert (old[1] & 0xFFF) == (new[1] & 0xFFF)
    assert (old[5] & 0xFFF) == (new[5] & 0xFFF)
    replacements += replace
    relocations += not replace
    a, b = ['{' + ','.join(hex(w) + 'u' for w in value) + '}' for value in (old, new)]
    for host in ('false', 'true'):
        harness += f'Check({a},{b},{str(replace).lower()},{host}); // guest {guest}\n'
    harness += f'Check({a},{b},false,false,true);\nCheck({a},{a},false);\n'
# Address-only-looking transitions may add/remove the complete mip allocation.
harness += r'''
Check({2,0xF4000052u,0x001FE0FFu,0xD10,0x200,0xA00},
      {2,0xF4000052u,0x001FE0FFu,0xD10,0x200,0xF4100A00u},true);
Check({2,0xF4000052u,0x001FE0FFu,0xD10,0x200,0xF4100A00u},
      {2,0xF4000052u,0x001FE0FFu,0xD10,0x200,0xA00},true);
Check({2,3,4,5,6,7},{2,3,4,5,6,7},true,false,false,6);
}
'''
source = args.output / 'refresh-harness.cpp'
source.write_text(harness, encoding='utf-8')
exe = args.output / 'refresh-harness.exe'
subprocess.run([args.compiler, '-std=c++20', '-DNOMINMAX', str(source), '-o', str(exe)], check=True)
subprocess.run([str(exe.resolve())], check=True)
report = {'runtime_header_pairs': len(rows), 'relocations': relocations,
          'shape_replacements': replacements, 'fixture_calls': len(rows) * 4 + 3,
          'production_body_extracted': True, 'gpu_fences_and_gameplay_verified': False}
(args.output / 'verification.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps(report))
