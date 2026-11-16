"""Exercise production metadata reads while the recording gate is occupied."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser()
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='clang++')
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
source = (Path(__file__).resolve().parents[2] / 'rexlego/src/gpu_native/textures.cpp').read_text()
def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

harness = r'''
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>
using u32=unsigned;using u64=unsigned long long;
using namespace std::chrono_literals;
thread_local bool preparing=false,fsr=false;
bool CurrentDrawPreparation(){return preparing;}
bool NativeFsr3Enabled(){return fsr;}
std::recursive_mutex recording;
struct HostDevice{static auto LockRecording(){return std::unique_lock(recording);}};
struct TextureResource{
 bool surface=true;int format=1;u32 logical=1280,physical=1280;
 u32 LogicalWidth()const{return logical;}u32 LogicalHeight()const{return 720;}
 u32 HostWidth()const{return physical;}u32 HostHeight()const{return 720;}
};
struct NativeRenderExtent{u32 logical_width=0,logical_height=0,width=0,height=0;};
struct TextureResourceView{TextureResource* texture=nullptr;};
std::mutex registry;
std::unordered_map<u32,std::shared_ptr<TextureResource>> resources;
std::atomic<u64> g_textures_generation{0};
auto FindTexture(u32 address){std::lock_guard lock(registry);auto it=resources.find(address);return it==resources.end()?std::shared_ptr<TextureResource>{}:it->second;}
std::atomic<unsigned> adopted{0};
auto AdoptTexture(u32 address){++adopted;return FindTexture(address);}
auto TextureResourceViewOf(u32,const std::shared_ptr<TextureResource>& resource){return TextureResourceView{resource.get()};}
struct D3DSurface{u32 depth_info=7;};D3DSurface header;
template<class T>T* GuestAt(u32){return &header;}
bool IsDepthFormat(int format){return format==2;}
float ColorOutputScale(u32 value){return float(value);}
'''
tests = r'''
int main(){
 resources[1]=std::make_shared<TextureResource>();
 auto gate=HostDevice::LockRecording();
 auto reader=std::async(std::launch::async,[]{
   preparing=true;
   for(unsigned i=0;i<10000;++i){
     auto extent=NativeTextureExtent(1);assert(extent.width==1280&&extent.height==720);
     assert(SurfaceColorOutputScale(1)==7);
     assert(ResolveTextureResource(1).texture==FindTexture(1).get());
     assert(!ResolveTextureResource(0).texture&&NativeTextureExtent(0).width==0);
     assert(SurfaceColorOutputScale(0)==1);
   }
 });
 assert(reader.wait_for(5s)==std::future_status::ready);reader.get();assert(adopted==0);
 auto scaled=std::async(std::launch::async,[]{preparing=true;fsr=true;return NativeTextureExtent(1);});
 assert(scaled.wait_for(30ms)==std::future_status::timeout);
 gate.unlock();assert(scaled.get().width==1280);
 auto old=resources[1];resources[1]=std::make_shared<TextureResource>();
 resources[1]->physical=640;++g_textures_generation;
 preparing=true;assert(NativeTextureExtent(1).width==640);
 resources[2]=std::make_shared<TextureResource>();resources[2]->surface=false;
 assert(ResolveTextureResource(2).texture&&adopted==1);
 assert(SurfaceColorOutputScale(2)==1);
}
'''
cpp = a.output / 'preparation-metadata.cpp'
cpp.write_text(harness + '\n'.join(function(s) for s in (
    'float SurfaceColorOutputScale(', 'NativeRenderExtent NativeTextureExtent(',
    'TextureResourceView ResolveTextureResource(')) + tests)
exe = a.output / 'preparation-metadata.exe'
subprocess.run([a.compiler, '-std=c++20', '-O2', str(cpp), '-o', str(exe)], check=True, timeout=60)
subprocess.run([str(exe.resolve())], check=True, timeout=15)
print('PASS production metadata reads: occupied recording gate, fixed and scaled paths, replacement, nulls and adoption fallback')
