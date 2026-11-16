"""Verify production scene storage group publication with allocation failures.

Executes extracted production preparation/commit/group functions. Device,
texture/view creation, descriptor heap and fences are injected boundaries.
No game, window or graphics device is started.
"""
from pathlib import Path
import argparse, hashlib, json, subprocess, sys


def body(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--compiler', default='clang++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    src = root / 'rexlego/src/gpu_native/textures.cpp'
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, str(Path(__file__).with_name('test_render_resize.py')), str(out / 'base'),
                    '--compiler', args.compiler], check=True)
    prefix = (out / 'base/test.cpp').read_text().split('int main(){')[0]
    prefix = '''#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <mutex>
#include <stdexcept>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d failed: %s\\n",__LINE__,#x); std::exit(23); } } while (0)
''' + prefix.replace('assert(', 'CHECK(')
    prefix = prefix.replace('int failure=0,',
        'int fail_kind=0,fail_at=0,allocations=0,resource_registrations=0;std::set<u32> slots;\nint failure=0,')
    prefix = prefix.replace('struct TextureResource {', '''struct TextureResource {
 bool composition_active=false;
 u32 LogicalWidth()const{return logical_width?logical_width:width;}
 u32 LogicalHeight()const{return logical_height?logical_height:height;}
''')
    prefix = prefix.replace('struct RenderTextureView {virtual', 'struct RenderTextureView {void* owner=nullptr;virtual')
    prefix = prefix.replace('return std::make_unique<D3D12TextureView>(d);',
        'auto result=std::make_unique<D3D12TextureView>(d);result->owner=this;return result;')
    prefix = prefix.replace('struct HostDevice {', '''std::recursive_mutex recording_mutex;
struct HostDevice {
 static auto LockRecording(){return std::unique_lock(recording_mutex);}
''')
    prefix = prefix.replace('++registrations;if(failure==4 || (failure==5 && registrations==2))return ~0u;',
        '++registrations;++resource_registrations;if(failure==7&&resource_registrations==2)throw std::runtime_error("Descriptor exception");if(failure==4 || (failure==5 && resource_registrations==2))return ~0u;')
    prefix = prefix.replace('return next_descriptor++;', 'slots.insert(next_descriptor);return next_descriptor++;')
    prefix = prefix.replace('unregistered.push_back(d);', 'CHECK(slots.erase(d)==1);unregistered.push_back(d);')
    prefix = prefix.replace(' if(failure==1)return{};', '''
 ++allocations;resource_registrations=0;failure=allocations==fail_at?fail_kind:0;
 if(failure==6)throw std::runtime_error("Texture exception");
 if(failure==1)return{};''')
    production = src.read_text()
    group = body(production, 'bool ReconfigureNativeSceneSurfaces(NativeRenderExtent extent)')
    globals = '''
std::map<u32,std::shared_ptr<TextureResource>> g_textures;
std::mutex g_textures_mutex;
'''
    cases = r'''
std::shared_ptr<TextureResource> surface(u32 address,bool depth,u32 w=1280,u32 h=720){
 auto r=std::make_shared<TextureResource>();r->guest_address=address;r->surface=true;
 r->format=depth?plume::RenderFormat::D32_FLOAT_S8_UINT:plume::RenderFormat::R8G8B8A8_UNORM;
 r->host_width=w;r->host_height=h;r->texture->width=w;r->texture->height=h;
 r->view->owner=r->texture.get();r->resolved_view->owner=r->texture.get();
 r->descriptor_index=address;r->resolved_descriptor_index=address+1;r->sampled_descriptor_index=address+2;
 for(auto d:{address,address+1,address+2})slots.insert(d);
 g_textures[address]=r;return r;
}
void setup(){
 g_textures.clear();retained.clear();slots.clear();unregistered.clear();
 allocations=registrations=invalidations=copies=failure=fail_kind=fail_at=0;next_descriptor=1000;
 surface(10,false);surface(20,true);surface(30,false,960,540);
 auto atlas=surface(40,true,256,256);atlas->width=atlas->height=256;
 auto cpu=surface(50,false);cpu->surface=false;
}
struct State {
 TextureResource* owner;plume::RenderTexture* texture;plume::RenderTextureView* view;
 plume::RenderTextureView* resolved;plume::RenderTexture* sampled;
 u32 width,height,logical_width,logical_height,descriptor,resolved_descriptor,sampled_descriptor;
};
std::vector<State> snapshot(){
 std::vector<State> result;for(const auto&[key,r]:g_textures)result.push_back({r.get(),r->texture.get(),r->view.get(),r->resolved_view.get(),
 r->sampled_texture.get(),r->HostWidth(),r->HostHeight(),r->logical_width,r->logical_height,r->descriptor_index,r->resolved_descriptor_index,r->sampled_descriptor_index});return result;
}
void unchanged(const std::vector<State>& before){
 for(const auto&s:before){auto&r=*s.owner;
  CHECK(r.texture.get()==s.texture&&r.view.get()==s.view&&r.resolved_view.get()==s.resolved&&r.sampled_texture.get()==s.sampled);
  CHECK(r.HostWidth()==s.width&&r.HostHeight()==s.height&&r.logical_width==s.logical_width&&r.logical_height==s.logical_height);
  CHECK(r.descriptor_index==s.descriptor&&r.resolved_descriptor_index==s.resolved_descriptor&&r.sampled_descriptor_index==s.sampled_descriptor);
  CHECK(r.sampled_valid&&r.guest_uploaded&&r.resolved_on_host&&r.guest_backing_current);
  CHECK(r.writeback_coverage.Matches(1280,720)&&r.writeback_coverage.Contains(639,359)&&r.previous_surface_descriptors.empty());
 }
 CHECK(invalidations==0&&retained.empty());
}
int main(){unsigned scenarios=0;
 const NativeRenderExtent extent{1280,720,3840,2160};
 for(int kind=1;kind<=7;++kind){if(kind==0)continue;for(int target=1;target<=3;++target){
  setup();auto before=snapshot();const auto old_slots=slots;fail_kind=kind;fail_at=target;bool ok=false,threw=false;
  try{ok=ReconfigureNativeSceneSurfaces(extent);}catch(const std::runtime_error&){threw=true;}
  CHECK(!ok&&threw==(kind==6||kind==7));unchanged(before);CHECK(slots==old_slots&&copies==0);++scenarios;
 }}
 setup();auto before=snapshot();const auto old_slots=slots;CHECK(ReconfigureNativeSceneSurfaces(extent));
 CHECK(allocations==3&&registrations==6&&invalidations==3&&retained.size()==3&&copies==0);
 for(unsigned i=0;i<3;++i){auto&r=*before[i].owner;
  CHECK(r.HostWidth()==3840&&r.HostHeight()==2160&&r.LogicalWidth()==1280&&r.LogicalHeight()==720);
  CHECK(r.texture.get()!=before[i].texture&&r.view->owner==r.texture.get()&&r.resolved_view->owner==r.texture.get());
  CHECK(slots.contains(r.descriptor_index)&&slots.contains(r.resolved_descriptor_index));
  CHECK(r.sampled_descriptor_index==~0u&&!r.sampled_texture&&!r.sampled_valid&&!r.guest_backing_current);
  CHECK(!r.guest_uploaded&&!r.resolved_on_host&&!r.writeback_coverage.Matches(1280,720));
  CHECK(r.previous_surface_descriptors==std::vector<u32>({before[i].descriptor,before[i].resolved_descriptor,before[i].sampled_descriptor}));
  bool found=false;for(auto&retired:retained)if(retired->texture.get()==before[i].texture){found=true;CHECK(retired->view.get()==before[i].view&&retired->sampled_texture.get()==before[i].sampled);}
  CHECK(found);
 }
 for(unsigned i=3;i<5;++i){CHECK(before[i].owner->texture.get()==before[i].texture&&before[i].owner->HostWidth()==before[i].width);}
 for(auto slot:old_slots)CHECK(slots.contains(slot));++scenarios;
 const auto prepared_count=allocations;CHECK(ReconfigureNativeSceneSurfaces(extent));CHECK(allocations==prepared_count&&invalidations==3);++scenarios;
 setup();before=snapshot();const auto original_slots=slots;g_textures.at(20)->composition_active=true;
 CHECK(!ReconfigureNativeSceneSurfaces(extent));unchanged(before);CHECK(slots==original_slots);++scenarios;
 setup();before=snapshot();CHECK(!ReconfigureNativeSceneSurfaces({1280,720,0,2160}));unchanged(before);CHECK(allocations==0);++scenarios;
 CHECK(!ReconfigureNativeSceneSurfaces({640,360,3840,2160}));unchanged(before);CHECK(allocations==0);++scenarios;
 setup();g_textures.clear();CHECK(ReconfigureNativeSceneSurfaces(extent));CHECK(allocations==0);++scenarios;
 setup();before=snapshot();fail_kind=5;fail_at=3;CHECK(!ReconfigureNativeSceneSurfaces(extent));unchanged(before);
 fail_kind=fail_at=0;CHECK(ReconfigureNativeSceneSurfaces(extent));CHECK(retained.size()==3&&invalidations==3);++scenarios;
 std::printf("PASS %u grouped scene resize scenarios: prepare-all publication, texture/view/descriptor failures and exceptions, abort cleanup, fenced old storage, retry, 4K metadata, unchanged extent, composition guard and non-scene exclusions\n",scenarios);
}
'''

    def run(label, pre=prefix, fn=group):
        fixture = out / (label + '.cpp')
        fixture.write_text(pre + globals + fn + cases)
        exe = out / (label + '.exe')
        result = subprocess.run([args.compiler, '-std=c++23', '-O2', '-UNDEBUG', '-I' + str(root / 'rexlego/src'),
                                 str(fixture), '-o', str(exe)], capture_output=True, text=True, timeout=60)
        (out / (label + '-compile.log')).write_text(result.stdout + result.stderr)
        result.check_returncode()
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20)
        (out / (label + '.log')).write_text(result.stdout + result.stderr)
        return result

    positive = run('production')
    positive.check_returncode()
    print(positive.stdout, end='', flush=True)
    start = group.index('  std::vector<PreparedNativeStorage> prepared;')
    sequential = group[:start] + '''  bool complete=true;
  for(const auto& resource:surfaces) complete=ResizeNativeStorage(*resource,extent)&&complete;
  return complete;
}'''
    negative = []
    result = run('publish-each-surface', fn=sequential)
    assert result.returncode == 23, (result.returncode, result.stdout, result.stderr)
    negative.append(dict(name='publish-each-surface', exit=result.returncode, error=result.stderr))
    for label, old, new in [
        ('leak-aborted-descriptors', 'if (descriptor != ~0u) HostDevice::UnregisterTexture(descriptor);', '(void)descriptor;'),
        ('release-published-descriptor', 'std::exchange(replacement.descriptor,~0u)', 'replacement.descriptor'),
    ]:
        assert prefix.count(old) == 1
        result = run(label, pre=prefix.replace(old, new))
        assert result.returncode == 23, (label, result.returncode, result.stdout, result.stderr)
        negative.append(dict(name=label, exit=result.returncode, error=result.stderr))
    files = [src, src.with_suffix('.h'), Path(__file__).resolve(), Path(__file__).with_name('test_render_resize.py')]
    (out / 'verification.json').write_text(json.dumps(dict(passed=True, game_launched=False, output=positive.stdout,
        negative=negative, sources={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in files},
        scope=__doc__, limits=[
            'Real texture/view creation, descriptor/fence implementations, full FSR mode/context atomicity and actual frame/window acknowledgement require separate verification.',
            'No guarantee is claimed for process-wide allocator exhaustion while committing framebuffer invalidation or fence-retirement bookkeeping.',
            '3840x2160 checks refer to resource metadata in this CPU fixture; no 4K gameplay frame is drawn.',
        ]), indent=2))


if __name__ == '__main__':
    main()
