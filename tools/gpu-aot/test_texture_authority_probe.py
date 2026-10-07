"""Compare actual texture view/probe bodies before and after diagnostic caching."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def block(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--compiler', default='clang++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = root / 'rexlego/src/gpu_native/textures.cpp'
    probe_source = root / 'rexlego/src/gpu_native/long_probe.h'
    view_source = root / 'rexlego/src/gpu_native/textures.h'
    text, probes = source.read_text(), probe_source.read_text()
    resolver = block(text, 'TextureResourceView ResolveTextureResource(')
    # Reconstruct the original production body by removing only the diagnostic
    # cache guard/assignments. View selection and the global dedup call stay real.
    cached = block(resolver, '    if (!resource->authority_probe_signature_valid ||')
    legacy_probe = cached[cached.index('      if (LongProbeOnce'):cached.rfind('}')]
    legacy_probe = '\n'.join(line[2:] for line in legacy_probe.rstrip().splitlines())
    legacy = resolver.replace(cached, legacy_probe)
    assert 'authority_probe_signature' not in legacy
    declarations = '\n'.join(block(text, name) + ';' for name in
                             ('struct FramebufferKey {', 'struct FramebufferKeyHash {',
                              'struct TextureResource {'))
    declarations += '\n' + block(view_source.read_text(), 'struct TextureResourceView {') + ';'
    once = block(probes, 'inline bool LongProbeOnce(')
    event = block(probes, 'template<class... T>\ninline void LongProbeEvent(')
    harness = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
using u32=uint32_t; using u64=uint64_t;
namespace plume {
enum class RenderFormat {UNKNOWN, R32_FLOAT, R32G32B32A32_FLOAT, COLOR, DEPTH};
struct RenderTexture {}; struct RenderTextureView {}; struct RenderFramebuffer {};
}
namespace rex::graphics::xenos { struct xe_gpu_texture_fetch_t {u32 swizzle=0xB48;}; }
struct TextureUploadSourceKey {}; struct CpuMemoryStamp {};
'''
    harness += declarations + r'''
bool IsDepthFormat(plume::RenderFormat format) {return format==plume::RenderFormat::DEPTH;}
using Event=std::tuple<std::string,bool,std::string>;
'''
    for namespace, body in [('original', legacy), ('optimized', resolver)]:
        harness += '\nnamespace ' + namespace + r''' {
bool enabled=false;
u64 lookups=0;
std::vector<Event> events;
std::unordered_map<u32,std::shared_ptr<TextureResource>> resources;
bool LongProbeEnabled() {return enabled;}
void WriteLongProbeEvent(std::string_view kind,bool anomaly,std::string_view payload) {
  events.emplace_back(kind,anomaly,payload);
}
'''
        harness += once + '\n' + event + r'''
bool CountedProbeOnce(u64 key) {++lookups;return LongProbeOnce(key);}
auto AdoptTexture(u32 address) {
  const auto found=resources.find(address);
  return found==resources.end() ? nullptr : found->second;
}
'''
        harness += body.replace('LongProbeOnce(', 'CountedProbeOnce(') + '\n}\n'
    harness += r'''
constexpr u32 address=0xAC585174;
std::shared_ptr<TextureResource> MakeResource(u32 guest) {
  auto resource=std::make_shared<TextureResource>();
  resource->guest_address=guest;resource->width=960;resource->height=3840;
  resource->host_width=1920;resource->host_height=7680;resource->format=plume::RenderFormat::COLOR;
  resource->descriptor_index=377;resource->d3d_type=3;
  resource->texture=std::make_unique<plume::RenderTexture>();
  resource->view=std::make_unique<plume::RenderTextureView>();
  resource->resolved_view=std::make_unique<plume::RenderTextureView>();
  resource->sampled_texture=std::make_unique<plume::RenderTexture>();
  resource->sampled_view=std::make_unique<plume::RenderTextureView>();
  return resource;
}
void Create(u32 guest=address) {
  original::resources[guest]=MakeResource(guest);
  optimized::resources[guest]=MakeResource(guest);
}
template<class Change> void ChangeBoth(Change change,u32 guest=address) {
  change(*original::resources.at(guest));change(*optimized::resources.at(guest));
}
int TextureRole(const TextureResourceView& v,const TextureResource* r) {
  if (!v.texture)return 0;
  if (v.texture==r->texture.get())return 1;
  assert(v.texture==r->sampled_texture.get());return 2;
}
int ViewRole(const TextureResourceView& v,const TextureResource* r) {
  if (!v.view)return 0;
  if (v.view==r->view.get())return 1;
  if (v.view==r->resolved_view.get())return 2;
  assert(v.view==r->sampled_view.get());return 3;
}
void ResolveBoth(u32 guest=address) {
  const auto before=original::ResolveTextureResource(guest);
  const auto after=optimized::ResolveTextureResource(guest);
  const auto a=original::AdoptTexture(guest),b=optimized::AdoptTexture(guest);
  assert(TextureRole(before,a.get())==TextureRole(after,b.get()));
  assert(ViewRole(before,a.get())==ViewRole(after,b.get()));
  assert(std::tie(before.format,before.descriptor_index,before.width,before.height,
                  before.d3d_type,before.surface,before.depth)==
         std::tie(after.format,after.descriptor_index,after.width,after.height,
                  after.d3d_type,after.surface,after.depth));
  assert(original::events==optimized::events);
}
bool LookupBound(u64 expected) {
  if(optimized::lookups==expected)return true;
  std::cerr<<"unchanged authority repeated global lookup: "<<optimized::lookups
           <<" != "<<expected<<'\n';return false;
}
int main() {
  ResolveBoth();assert(original::events.empty());Create();
  for(int i=0;i<20;++i)ResolveBoth();
  assert(original::lookups==0 && optimized::lookups==0);
  assert(!optimized::resources.at(address)->authority_probe_signature_valid);
  original::enabled=optimized::enabled=true;
  for(int i=0;i<10000;++i)ResolveBoth();
  assert(original::events.size()==1);
  if(!LookupBound(1))return 17;
  assert(original::lookups==10000);
  // Non-signature metadata changes must preserve the old event suppression,
  // while every current view still carries fresh dimensions and surface state.
  ChangeBoth([](auto& r){r.width=512;r.host_width=1024;r.surface=true;r.guest_fetch.swizzle=7;});
  ResolveBoth();assert(original::events.size()==1);assert(LookupBound(1));
  ChangeBoth([](auto& r){r.descriptor_index=378;});
  for(int i=0;i<500;++i)ResolveBoth();
  assert(original::events.size()==2);assert(LookupBound(2));
  ChangeBoth([](auto& r){r.resolved_on_host=true;r.resolved_descriptor_index=379;});
  ResolveBoth();assert(original::events.size()==3);assert(LookupBound(3));
  assert(optimized::ResolveTextureResource(address).view==optimized::resources.at(address)->resolved_view.get());
  // Returning to a globally seen raw signature performs one check, emits none.
  ChangeBoth([](auto& r){r.resolved_on_host=false;r.resolved_descriptor_index=~u32{0};r.descriptor_index=377;});
  ResolveBoth();assert(original::events.size()==3);assert(LookupBound(4));
  for(int i=0;i<100;++i)ResolveBoth();assert(LookupBound(4));
  ChangeBoth([](auto& r){r.resolved_on_host=true;});
  ResolveBoth();assert(original::events.size()==4);assert(LookupBound(5));
  // The sampled CPU depth view and later GPU-authoritative mirror both bypass
  // the ordinary authority diagnostic, exactly as before this optimization.
  ChangeBoth([](auto& r){r.resolved_on_host=false;r.guest_uploaded=true;r.sampled_valid=true;
      r.sampled_descriptor_index=380;r.format=plume::RenderFormat::DEPTH;});
  for(int i=0;i<100;++i)ResolveBoth();assert(LookupBound(5));
  auto sampled=optimized::ResolveTextureResource(address);
  assert(sampled.format==plume::RenderFormat::R32_FLOAT && !sampled.depth && !sampled.surface);
  assert(sampled.width==512 && sampled.descriptor_index==380);
  ChangeBoth([](auto& r){r.resolved_on_host=true;r.guest_uploaded=false;});
  ResolveBoth();assert(LookupBound(5));
  ChangeBoth([](auto& r){r.sampled_valid=false;r.resolved_on_host=false;});
  ResolveBoth();assert(original::events.size()==4);assert(LookupBound(6));
  // Resource replacement/reset boundaries discard private state. Old views
  // remain attached to retained old resources; global dedup outlives the maps.
  const auto retained_a=original::resources.at(address),retained_b=optimized::resources.at(address);
  const auto retained_view=optimized::ResolveTextureResource(address);
  original::resources.clear();optimized::resources.clear();ResolveBoth();Create();ResolveBoth();
  assert(retained_view.texture==retained_b->texture.get());
  assert(original::events.size()==4);assert(LookupBound(7));
  Create(address+4);ResolveBoth(address+4);assert(original::events.size()==5);assert(LookupBound(8));
  ChangeBoth([](auto& r){r.descriptor_index=381;});ResolveBoth();
  assert(original::events.size()==6);assert(LookupBound(9));
  // Disabled tracing neither updates the cache nor performs any global check.
  original::enabled=optimized::enabled=false;
  ChangeBoth([](auto& r){r.descriptor_index=382;});ResolveBoth();assert(LookupBound(9));
  original::enabled=optimized::enabled=true;ResolveBoth();
  assert(original::events.size()==7);assert(LookupBound(10));
  std::cout<<"Actual texture authority views/events preserved; "<<original::lookups
           <<" original global checks reduced to "<<optimized::lookups<<'\n';
}
'''
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    fixture, binary = output / 'test.cpp', output / 'test.exe'
    fixture.write_text(harness)
    command = [args.compiler, '-std=c++20', '-UNDEBUG', '-Wall', '-Wextra',
               '-Werror', str(fixture), '-o', str(binary)]
    subprocess.run(command, check=True, timeout=45)
    subprocess.run([str(binary)], check=True, timeout=10)
    # A before-change control runs the same actual-body/event/view checks and
    # must fail specifically at the unchanged-signature lookup count boundary.
    old_fixture, old_binary = output / 'legacy.cpp', output / 'legacy.exe'
    optimized_start = harness.index('namespace optimized {')
    before_control = harness[:optimized_start] + harness[optimized_start:].replace(
        resolver.replace('LongProbeOnce(', 'CountedProbeOnce('),
        legacy.replace('LongProbeOnce(', 'CountedProbeOnce('), 1)
    assert before_control != harness
    old_fixture.write_text(before_control)
    subprocess.run(command[:-3] + [str(old_fixture), '-o', str(old_binary)], check=True, timeout=45)
    negative = subprocess.run([str(old_binary)], capture_output=True, text=True, timeout=10)
    assert negative.returncode == 17, negative.stderr
    assert 'unchanged authority repeated global lookup: 10000 != 1' in negative.stderr
    (output / 'verification.json').write_text(json.dumps({
        'production_source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
        'probe_source_sha256': hashlib.sha256(probe_source.read_bytes()).hexdigest(),
        'view_source_sha256': hashlib.sha256(view_source.read_bytes()).hexdigest(),
        'actual_bodies': ['TextureResource declaration', 'TextureResourceView declaration',
                          'ResolveTextureResource', 'LongProbeOnce', 'LongProbeEvent'],
        'coverage': ['10000 unchanged bindings', 'trace disabled', 'raw/host authority',
                     'descriptor changes', 'globally seen rollback', 'CPU depth sampled view',
                     'GPU sampled view', 'resource-map reset/replacement boundary',
                     'retained old view', 'new guest address'],
        'boundary_mocks': ['AdoptTexture resource map', 'trace writer', 'Plume object identities'],
        'reset_lifetime_scope': 'Resource replacement boundary simulation; no GPU fence claim.',
        'legacy_negative': {'returncode': negative.returncode, 'stderr': negative.stderr.strip()},
        'result': 'passed'}, indent=2) + '\n')


if __name__ == '__main__':
    main()
