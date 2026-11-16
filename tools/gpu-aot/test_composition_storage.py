"""Run real composition storage transactions, reuse, rollback and copyback retry."""
import argparse,subprocess,sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
p.add_argument('--compiler',default='clang++');a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[2]
# Reuse the existing allocation/view/descriptor failure oracle's mock types,
# while extracting all state transitions below from current production source.
subprocess.run([sys.executable,str(Path(__file__).with_name('test_render_resize.py')),
    str(out/'base'),'--compiler',a.compiler],check=True)
prefix=(out/'base/test.cpp').read_text().split('int main(){')[0]
prefix=prefix.replace('#include <cassert>','#include <cassert>\n#include <algorithm>\n#include <map>\n#include <mutex>\n#include "gpu_native/resolve_region.h"')
prefix=prefix.replace('int failure=0,','int allocations=0;std::recursive_mutex recording_mutex;\nenum class ColorResolveDestination{kScaledRGBA8};\nint failure=0,')
prefix=prefix.replace(' u32 width=0,height=0,levels=0;',' int value=0;u32 width=0,height=0,levels=0;')
prefix=prefix.replace('struct TextureResource {','struct TextureResource {\n std::shared_ptr<SurfaceStorage> composition_storage;bool composition_active=false,authority_probe_signature_valid=false;\n u32 LogicalWidth()const{return logical_width?logical_width:width;}u32 LogicalHeight()const{return logical_height?logical_height:height;}')
prefix=prefix.replace('struct HostDevice {','struct HostDevice {\n static auto LockRecording(){return std::unique_lock(recording_mutex);}\n static bool ResolveHdrColor(plume::RenderTexture* s,u32,plume::RenderTexture* d,u32 w,u32 h,float,ColorResolveDestination,bool,const ResolveRegion* output,const ResolveRegion* input){\n  assert(s && d && s!=d && w==d->width && h==d->height);assert(input->width==s->width && input->height==s->height);assert(output->width==d->width && output->height==d->height);d->value=failure==7?888:s->value;return failure!=7;\n }')
prefix=prefix.replace('++copies;assert(s && d && s!=d);return failure!=6;',
    '++copies;assert(s && d && s!=d);d->value=failure==6?999:s->value;return failure!=6;')
prefix=prefix.replace(' if(failure==1)return{};',' ++allocations;if(failure==1)return{};')
production=(root/'rexlego/src/gpu_native/textures.cpp').read_text()
def body(signature):
    start=production.index(signature);end=production.index('{',start)+1;depth=1
    while depth:depth+=(production[end]=='{')-(production[end]=='}');end+=1
    return production[start:end]
extra=r'''
#undef assert
#define assert(x) ((x)?void(0):std::exit(23))
std::map<u32,std::shared_ptr<TextureResource>> registry;
std::vector<std::shared_ptr<TextureResource>> g_composition_surfaces;
bool g_composition_restore_pending=false;
auto FindTexture(u32 address){auto it=registry.find(address);return it==registry.end()?std::shared_ptr<TextureResource>{}:it->second;}
bool EndNativeCompositionTargets(bool preserve_writes=true);
'''
functions='\n'.join(body(signature) for signature in ['void SwapCompositionStorage(',
    'bool CopyCompositionStorage(','bool BeginCompositionSurface(','bool EndNativeCompositionTargets(',
    'bool RetryNativeCompositionTargets(','bool BeginNativeCompositionTargets('])
main=r'''
auto surface(u32 address,bool depth,u32 w=1280,u32 h=720){
 auto r=std::make_shared<TextureResource>();r->guest_address=address;r->surface=true;
 r->format=depth?plume::RenderFormat::D32_FLOAT_S8_UINT:plume::RenderFormat::R8G8B8A8_UNORM;
 r->host_width=w;r->host_height=h;r->texture->width=w;r->texture->height=h;
 r->descriptor_index=address;r->resolved_descriptor_index=address+1;r->sampled_descriptor_index=address+2;
 r->texture->value=depth?8:4;registry[address]=r;return r;
}
int main(){
 auto color=surface(10,false),depth=surface(20,true);
 // Initial color-copy failure must restore the original storage and contents.
 failure=7;auto* old_color=color->texture.get();assert(!BeginNativeCompositionTargets(10,20));
 assert(color->texture.get()==old_color && color->HostWidth()==1280 && color->texture->value==4);
 assert(!color->composition_active && g_composition_surfaces.empty());
 // Failure preparing the second target rolls back the first without copying
 // tentative native writes into the still-authoritative scene-sized storage.
 failure=6;assert(!BeginNativeCompositionTargets(10,20));
 assert(color->texture.get()==old_color && color->texture->value==4 && depth->texture->value==8);
 assert(color->HostWidth()==1280 && depth->HostWidth()==1280 && !g_composition_restore_pending);
 failure=0;retained.clear();assert(BeginNativeCompositionTargets(10,20));
 assert(color->texture->value==4 && depth->texture->value==8);
 color->texture->value=40;depth->texture->value=80;assert(EndNativeCompositionTargets());
 assert(color->texture.get()==old_color && color->texture->value==40 && depth->texture->value==80);
 const auto warm_allocations=allocations,warm_registrations=registrations;
 for(int frame=0;frame<100;++frame){
  assert(BeginNativeCompositionTargets(10,20));
  assert(color->HostWidth()==1920 && depth->HostHeight()==1080);
  color->texture->value=frame+1000;depth->texture->value=frame+2000;
  assert(EndNativeCompositionTargets());assert(color->HostWidth()==1280 && depth->HostHeight()==720);
  assert(color->texture->value==frame+1000 && depth->texture->value==frame+2000);
 }
 assert(allocations==warm_allocations && registrations==warm_registrations);
 assert(color->previous_surface_descriptors.size()<=6 && depth->previous_surface_descriptors.size()<=6);
 // Failed copyback never publishes partially updated scene-sized storage.
 assert(BeginNativeCompositionTargets(10,20));color->texture->value=345;depth->texture->value=678;
 failure=7;assert(!EndNativeCompositionTargets());assert(g_composition_restore_pending);
 assert(color->composition_active && color->HostWidth()==1920 && color->texture->value==345);
 failure=0;assert(RetryNativeCompositionTargets());assert(color->HostWidth()==1280 && color->texture->value==345);
 assert(!g_composition_restore_pending && !color->composition_active);
 // Later full-screen targets join the same phase. A failed extension must
 // preserve native UI writes already made to earlier targets.
 assert(BeginNativeCompositionTargets(10,20));color->texture->value=900;depth->texture->value=901;
 auto later_color=surface(30,false),later_depth=surface(40,true);
 failure=6;assert(!BeginNativeCompositionTargets(30,40));
 assert(g_composition_surfaces.size()==2 && color->texture->value==900 && depth->texture->value==901);
 assert(color->composition_active && later_color->HostWidth()==1280 && later_depth->HostWidth()==1280);
 failure=0;assert(BeginNativeCompositionTargets(30,0));assert(BeginNativeCompositionTargets(0,40));
 assert(g_composition_surfaces.size()==4);const auto stable_copies=copies;
 assert(BeginNativeCompositionTargets(30,40));assert(copies==stable_copies && g_composition_surfaces.size()==4);
 later_color->texture->value=902;later_depth->texture->value=903;
 assert(EndNativeCompositionTargets());assert(color->texture->value==900 && depth->texture->value==901);
 assert(later_color->texture->value==902 && later_depth->texture->value==903);
 // A new FSR mode changes the scene extent while the native alternate can be
 // reused. Preserve guest logical dimensions and new-mode contents.
 assert(ResizeNativeStorage(*color,{1280,720,640,360}));
 assert(ResizeNativeStorage(*depth,{1280,720,640,360}));
 color->texture->value=12;depth->texture->value=13;const auto mode_allocations=allocations;
 assert(BeginNativeCompositionTargets(10,20));assert(allocations==mode_allocations);
 assert(color->texture->value==12 && depth->texture->value==13);assert(EndNativeCompositionTargets());
 assert(color->HostWidth()==640 && depth->HostHeight()==360 && color->LogicalWidth()==1280);
 // Releasing a guest owner before Swap must not touch a replacement at the
 // same guest address; its old allocations still belong to recording fences.
 assert(BeginNativeCompositionTargets(10,20));auto replacement=surface(10,false);
 assert(EndNativeCompositionTargets());assert(replacement->HostWidth()==1280 && replacement->texture->value==4);
 registry.clear();g_composition_surfaces.clear();color=surface(10,false,1920,1080);depth=surface(20,true,1920,1080);
 const auto before=allocations;assert(BeginNativeCompositionTargets(10,20));
 assert(g_composition_surfaces.empty() && EndNativeCompositionTargets() && allocations==before);
}
'''
for name,source in [('positive',functions),('omit-copyback',functions.replace(
    'if(preserve_writes && !CopyCompositionStorage(*resource,true))','if(false)'))]:
    cpp=out/(name+'.cpp');exe=out/(name+'.exe');cpp.write_text(prefix+extra+source+main)
    subprocess.run([a.compiler,'-std=c++20','-UNDEBUG','-I'+str(root/'rexlego/src'),str(cpp),'-o',str(exe)],check=True)
    result=subprocess.run([str(exe)])
    assert result.returncode==(0 if name=='positive' else 23),(name,result.returncode)
print('PASS production composition allocation reuse, both rollback paths, copyback/retry, guest replacement and native-size no-op; omitted copyback rejected')
