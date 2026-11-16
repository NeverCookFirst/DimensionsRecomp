"""Run the production surface cache across shutdown and device recreation."""
import argparse
from pathlib import Path
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',type=Path)
    parser.add_argument('--compiler',default='clang++')
    parser.add_argument('--verify-regression',action='store_true')
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[2]
    text=(root/'rexlego/src/gpu_native/textures.cpp').read_text()
    first=text.index('namespace surface_pool {')
    last=text.index('}  // namespace surface_pool',first)+len('}  // namespace surface_pool')
    production=text[first:last]
    code=r'''
#include <atomic>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
using u32=uint32_t;using u64=uint64_t;
#undef assert
#define assert(c) do{if(!(c)){std::fprintf(stderr,"check failed: %s\n",#c);std::exit(23);}}while(0)
#define REXLOG_INFO(...) ((void)0)
int destroyed=0;
namespace plume{struct RenderTexture{int device;u64 bytes=1024;explicit RenderTexture(int n,u64 b=1024):device(n),bytes(b){}
 ~RenderTexture(){++destroyed;}};}
struct TextureResource{u32 width=1920,height=1080,format=1;
 u32 HostWidth()const{return width;}u32 HostHeight()const{return height;}};
u64 SurfaceStorageBytes(const plume::RenderTexture* t){return t?t->bytes:0;}
''' + production + r'''
int main(){
 TextureResource r;
 assert(!surface_pool::Take(r));
 surface_pool::Start();
 surface_pool::Give(r,std::make_unique<plume::RenderTexture>(1));
 auto reused=surface_pool::Take(r);assert(reused&&reused->device==1);
 surface_pool::Give(r,std::move(reused));
 TextureResource other=r;other.format=2;assert(!surface_pool::Take(other));
 // Shutdown drains pooled storage before the device dies.
 surface_pool::Stop();assert(destroyed==1&&surface_pool::g_free.empty());
 // A retained frame reference can be released later, during HostState teardown.
 surface_pool::Give(r,std::make_unique<plume::RenderTexture>(1));
 assert(destroyed==2&&surface_pool::g_free.empty());
 surface_pool::Start();assert(!surface_pool::Take(r));
 surface_pool::Give(r,std::make_unique<plume::RenderTexture>(2));
 reused=surface_pool::Take(r);assert(reused&&reused->device==2);reused.reset();
 for(unsigned i=0;i<20;++i)surface_pool::Give(r,std::make_unique<plume::RenderTexture>(2));
 assert(surface_pool::g_free.begin()->second.size()==16);
 surface_pool::Stop();assert(destroyed==23&&surface_pool::g_free.empty());
 // Thousands of distinct world shapes previously survived until exit.
 surface_pool::Start();
 for(unsigned i=0;i<1000;++i){r.width=1000+i;
  surface_pool::Give(r,std::make_unique<plume::RenderTexture>(3,8*1024*1024));
  assert(surface_pool::g_cached_bytes<=surface_pool::kMaxCachedBytes);
  assert(surface_pool::g_free.size()<=surface_pool::kMaxCachedKeys);
 }
 assert(surface_pool::g_evictions>900);
 r.width=1999;reused=surface_pool::Take(r);assert(reused&&reused->device==3);
 surface_pool::Give(r,std::move(reused));
 surface_pool::Give(r,std::make_unique<plume::RenderTexture>(3,surface_pool::kMaxCachedBytes+1));
 assert(surface_pool::g_cached_bytes<=surface_pool::kMaxCachedBytes);
 surface_pool::Stop();assert(surface_pool::g_cached_bytes==0&&surface_pool::g_usage.empty());
 surface_pool::Start();
 for(unsigned i=0;i<1000;++i){r.width=1000+i;
  surface_pool::Give(r,std::make_unique<plume::RenderTexture>(4));
  assert(surface_pool::g_free.size()<=surface_pool::kMaxCachedKeys);
 }
 assert(surface_pool::g_free.size()==surface_pool::kMaxCachedKeys);
 surface_pool::Stop();assert(surface_pool::g_cached_bytes==0);
}
'''
    args.output.mkdir(parents=True,exist_ok=True)
    def run(name,code):
        source=args.output.resolve()/(name+'.cpp');binary=args.output.resolve()/(name+'.exe')
        source.write_text(code)
        subprocess.run([args.compiler,'-std=c++20',str(source),'-o',str(binary)],check=True,timeout=45)
        return subprocess.run([str(binary)],capture_output=True,timeout=10)
    result=run('surface-pool',code)
    assert result.returncode==0,result.stderr.decode(errors='replace')
    if args.verify_regression:
        result=run('surface-pool-recycles-after-shutdown',code.replace('if (!g_active) return;',''))
        assert result.returncode==23,result.stderr.decode(errors='replace')
    print('Production surface cache reuse, world-shape byte/key budgets, oversized rejection and device lifetime PASS')


if __name__=='__main__':
    main()
