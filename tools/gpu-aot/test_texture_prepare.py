"""Execute production sampling transaction, header refresh and binding loop.

Allocation/upload drivers are bounded mocks; placement changes are injected
inside upload, where guest streaming can publish without SetTexture.
"""
import argparse,json,os,subprocess
from pathlib import Path

def block(text,signature):
    start=text.index(signature);end=text.index('{',start)+1;depth=1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('output',type=Path)
    p.add_argument('--compiler',default='clang++');a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    root=Path(__file__).resolve().parents[2];source=(root/'rexlego/src/gpu_native/textures.cpp').read_text()
    subprocess.run([os.sys.executable,str(Path(__file__).with_name('test_texture_binding_failures.py')),
        str(out/'binding-boundary'),'--compiler',a.compiler],check=True,timeout=60)
    code=(out/'binding-boundary/texture-binding.cpp').read_text()
    code=code.replace('#include <iostream>','#include <iostream>\n#include <atomic>\n#include <mutex>\n#include <cstring>')
    code=code.replace('struct HostDevice {','''struct HostDevice {
 static inline u32 retired=0;
 static inline std::vector<std::shared_ptr<void>>retired_owners;
 static void UnregisterTexture(u32){}
 template<class T>static void RetireResource(std::shared_ptr<T>r){++retired;retired_owners.push_back(r);}
''',1)
    code=code.replace('struct TextureResource {','''struct TextureResource {
 std::atomic<u32>header_sequence{0};std::atomic<bool>replaced{false};
 bool owns_guest_memory=false,guest_backing_current=false,guest_uploaded=false,upload_source_key_valid=false;
 u32 adopted_header_type=3,resolved_descriptor_index=4,sampled_descriptor_index=5,mirror_address=0;
 std::vector<u32>previous_surface_descriptors{};
 std::array<u32,6>guest_fetch{};u32 cpu_stamp=0,depth_alias_generation=0;
''',1)
    code=code.replace('std::shared_ptr<TextureResource>FindTexture(u32 address){',
        'u32 registry_lookups=0;\nstd::shared_ptr<TextureResource>FindTexture(u32 address){\n ++registry_lookups;',1)
    resolver=block(code,'TextureResourceView ResolveTextureResource(')
    helpers='''
std::mutex g_textures_mutex;auto&g_textures=resources;
struct D3DTexture{struct{u32 common=3;}resource;struct{std::array<u32,6>dword{};}format;};
std::array<D3DTexture,100>headers;
template<class T>T* GuestAt(u32 address){assert(address<headers.size());return &headers[address];}
void NoteTextureFootprintChange(u32){}
void InvalidateFramebufferReferences(u32){}
struct Memory{void SystemHeapFree(u32){assert(false);}}memory;
#define REX_KERNEL_MEMORY() (&memory)
bool UnlockedTextureUploadEnabled(){return false;}
bool CurrentDrawPreparation(){return false;}
bool NativeFsr3Enabled(){return false;}
struct NativeRecordingGate{static int RegistryDepth(){return 0;}};
bool RegistryFreeTextureUpload(const TextureResource&){return false;}
int publication_mode=0,publications_left=0;u32 fail_on_upload=0,adoptions=0;
'''+block(source,'bool RefreshTextureResourceHeader(u32 guest_address,\n    const std::shared_ptr<TextureResource>& resource) {')+'\n'+block(source,'std::shared_ptr<void> RefreshTextureHeaderOwner(')+'\n'+block(source,'void RefreshTextureHeader(')+'\n'+block(source,'std::shared_ptr<TextureResource> FindTextureAtUse(')+'''
std::shared_ptr<TextureResource>AdoptTexture(u32 address){
 if(!address)return {};
 auto resource=FindTextureAtUse(address);
 if(!resource && address && address<headers.size() && headers[address].resource.common==3 && headers[address].format.dword[0]){
  resource=std::make_shared<TextureResource>();resource->guest_fetch=headers[address].format.dword;
  resource->descriptor_index=29+adoptions++;resources[address]=resource;
 }
 return resource;
}
TextureResourceView TextureResourceViewOf(u32,const std::shared_ptr<TextureResource>& resource){
 if(!resource)return {};
 return {resource->texture,resource->descriptor_index,resource->d3d_type,false,false,
         resource->sampling_fetch,resource->has_sampling_fetch};
}
'''+'''
// Direct resolve has a real SDK/GPU fixture. This boundary projects prepared metadata.
TextureResourceView ResolveTextureResource(u32 address){return TextureResourceViewOf(address,AdoptTexture(address));}
'''
    code=code.replace(resolver,helpers)
    upload=block(code,'bool UploadTextureResource(')
    storage=upload.replace('bool UploadTextureResource(u32 guest_address,plume::RenderCommandList* commands,bool require_content_hash)',
        'bool UploadTextureResourceStorage(u32 guest_address,const std::shared_ptr<TextureResource>& resource,plume::RenderCommandList* commands,bool require_content_hash)')
    storage=storage.replace(' const auto resource=FindTexture(guest_address);',' if(fail_on_upload && upload_calls==fail_on_upload)return false;',1)
    needle=' if(resource&&resource->cpu_upload_allowed){';assert needle in storage
    storage=storage.replace(needle,needle+'''
  resource->guest_uploaded=true;
  if(publications_left){
   if(publications_left>0)--publications_left;
   if(publication_mode==1)headers[guest_address].format.dword[2]^=0x100;
   if(publication_mode==2){headers[guest_address].format.dword[1]+=0x1000;headers[guest_address].format.dword[5]+=0x1000;}
   if(publication_mode==3)headers[guest_address].resource.common=6;
  }
''',1)
    code=code.replace(upload,storage+'\n'+block(source,'bool UploadTextureResource('))
    code=code.replace(block(code,'TexturePreparationResult PrepareTextureResource('),block(source,'TexturePreparationResult PrepareTextureResource('))
    code=code.replace('int main(){','int boundary_main(){',1)
    closing=code.rfind('}');code=code[:closing]+'return 0;\n'+code[closing:]
    code+='''
#undef assert
#define assert(c) do{if(!(c)){std::cerr<<"failed line "<<__LINE__<<": "<<#c<<"\\n";std::exit(23);}}while(0)
auto Setup(){
 Reset();headers={};HostDevice::retired=0;HostDevice::retired_owners.clear();adoptions=0;
 publication_mode=publications_left=0;fail_on_upload=0;registry_lookups=0;
 headers[1].format.dword={2,0xF4000052u,0x001FE0FFu,0xD10,0x200,0xF4100A00u};
 auto original=Add(1,0);original->guest_fetch=headers[1].format.dword;return original;
}
void Draw(bool indexed){if(indexed)DrawIndexedVerticesHook(&device,0,0,0,3);else DrawVerticesHook(&device,0,0,3);}
void Ready(){assert(successful_draws==1&&query_failures==0);auto selected=resources.at(1);
 assert(selected->guest_uploaded||selected->surface||selected->resolved_on_host);
 assert(selected->guest_fetch==headers[1].format.dword);
 assert(last_shared.texture_2d[0]==selected->descriptor_index);}
int main(){
 const char* setting=std::getenv("LEGO_NATIVE_REUSE_TEXTURE_LOOKUP");
 bool legacy=setting && std::strcmp(setting,"0")==0;
 for(bool indexed:{false,true}){
  Setup();Draw(indexed);Ready();assert(upload_calls==1&&registry_lookups==(legacy?3:1));
  for(int mode:{1,2}){
   auto previous=Setup();publication_mode=mode;publications_left=1;vertex_mask=1;
   Draw(indexed);Ready();assert(upload_calls==2&&cpu_upload_calls==2);
   assert((require_hashes==std::vector<bool>{true,true}));
   assert((mode==1)==(resources.at(1)!=previous));assert(HostDevice::retired==(mode==1?1:0));
  }
  for(int mode:{1,2}){
   Setup();publication_mode=mode;publications_left=-1;Draw(indexed);
   assert(successful_draws==0&&query_failures==1&&upload_calls==3&&HostDevice::sampler_calls==0);
   assert(last_shared.texture_2d[0]==0);
   publication_mode=publications_left=0;upload_calls=cpu_upload_calls=query_failures=0;Draw(indexed);Ready();assert(upload_calls==1);
  }
  Setup();publication_mode=1;publications_left=1;fail_on_upload=2;Draw(indexed);
  assert(successful_draws==0&&query_failures==1&&upload_calls==2&&HostDevice::sampler_calls==0);
  Setup();publication_mode=3;publications_left=1;Draw(indexed);
  assert(successful_draws==0&&query_failures==1&&upload_calls==1&&HostDevice::sampler_calls==0);
  Setup()->uploaded_descriptor=59;Draw(indexed);Ready();assert(last_shared.texture_2d[0]==59&&upload_calls==1);
  Setup()->lose_storage_on_upload=true;Draw(indexed);assert(query_failures==1&&successful_draws==0&&HostDevice::sampler_calls==0);
  Setup()->uploaded_descriptor=~u32{0};Draw(indexed);assert(query_failures==1&&successful_draws==0);
  for(bool surface:{false,true}){auto host=Setup();host->surface=surface;host->resolved_on_host=!surface;
   host->cpu_upload_allowed=false;Draw(indexed);assert(successful_draws==1&&cpu_upload_calls==0);}
 }
 auto original=Setup();TextureResourceView result;result.texture=reinterpret_cast<void*>(77);result.descriptor_index=88;
 assert(PrepareTextureResource(99,&command_list,false,result)==TexturePreparationResult::kUnavailable);
 assert(!result.texture&&result.descriptor_index==~u32{0}&&upload_calls==0);
 assert(PrepareTextureResource(1,nullptr,false,result)==TexturePreparationResult::kUploadFailed&&!result.texture);
 std::cout<<"PASS production preparation/binding: layout+relocation retry, bounded churn, failures/recovery, animation hashes, final SRV and per-call owner reuse\\n";
}
'''
    def run(name,body,reuse='1'):
        cpp=out/(name+'.cpp');exe=out/(name+'.exe');cpp.write_text(body)
        subprocess.run([a.compiler,'-std=c++20','-D_CRT_SECURE_NO_WARNINGS','-UNDEBUG',str(cpp),'-o',str(exe)],check=True,timeout=45)
        return subprocess.run([str(exe)],capture_output=True,text=True,timeout=10,
            env=dict(os.environ,LEGO_NATIVE_REUSE_TEXTURE_LOOKUP=reuse))
    positives=[]
    for setting in ('1','0'):
        r=run('prepare-'+setting,code,setting);assert r.returncode==0,r.stderr
        positives.append(dict(reuse=setting,exit=r.returncode,stdout=r.stdout))
    mutants={
        'omit-header-validation':('if (!RefreshTextureResourceHeader(guest_address,resource)) {','if (false) {'),
        'omit-relocation-retry':('std::memcmp(&fetch_before,&resource->guest_fetch,sizeof(fetch_before))','0'),
        'omit-replacement-upload':('attempt<3','attempt<1'),
        'stale-preupload-view':('const auto view=TextureResourceViewOf(guest_address,resource);','const auto view=initial;')}
    negatives=[]
    for name,(old,new) in mutants.items():
        assert code.count(old)==1,(name,code.count(old));r=run(name,code.replace(old,new))
        assert r.returncode==23,(name,r.stderr);negatives.append(dict(name=name,exit=r.returncode,stderr=r.stderr))
    (out/'verification.json').write_text(json.dumps(dict(game_launched=False,production_transaction_and_binding=True,
        production_header_refresh=True,allocation_and_upload_boundary_mocked=True,positive=positives,negative=negatives,
        scope='actual transaction/header refresh/binding and upload entry guards; mocked GPU allocation/conversion/publication, no game FPS/fence proof'),indent=2))
    print(positives[0]['stdout'])

if __name__=='__main__':main()
