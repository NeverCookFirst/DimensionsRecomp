"""Check actual resource-wait hook exclusions and completion callback ordering."""
import argparse
import json
import os
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('output',type=Path)
p.add_argument('--compiler', default='clang++')
a=p.parse_args()
root=Path(__file__).resolve().parents[2];a.output.mkdir(parents=True,exist_ok=True)
def body(path,sig):
 text=(root/path).read_text();start=text.index(sig);end=text.index('{',start)+1;depth=1
 while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
texture=body('rexlego/src/gpu_native/textures.cpp','bool IsCpuUploadedTexture(')
hooks='\n'.join(body('rexlego/src/gpu_native/hooks_device.cpp',x) for x in [
 'void BlockUntilNotBusyHook(', 'void BlockUntilIdleHook(', 'void BlockOnFenceHook(', 'void InsertCallbackHook('])
h=a.output/'cpu-resource-wait.cpp';h.write_text(r'''
#include <cassert>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <functional>
#include <vector>
#include <unordered_set>
using u32=unsigned;
#define REXLOG_ERROR(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
#define REXLOG_INFO(...) ((void)0)
struct D3DDevice{};
enum class SyncReason { kOther, kIdle, kFence, kResource, kCallback };
struct HostDevice {
 static inline std::recursive_mutex lock;
 static inline u32 syncs=0,skips=0,callbacks=0;
 static inline bool ready=true;
 static inline bool completed=false;
 static inline std::vector<std::function<void()>> pending;
 static auto LockRecording(){return std::unique_lock(lock);}
 static bool Synchronize(SyncReason = SyncReason::kOther){++syncs;return ready;}
 static void RecordCpuResourceWaitSkip(){++skips;}
 static bool EnqueueCompletionCallback(std::function<void()> fn){if(!ready)return false;pending.push_back(fn);return true;}
 static void PollCompletionCallbacks(){if(!completed)return;auto tasks=std::move(pending);pending.clear();for(auto& fn:tasks)fn();}
};
struct Texture {std::mutex mutex;bool surface=false,resolved_on_host=false,owns_guest_memory=false;unsigned depth_alias_generation=0;};
std::shared_ptr<Texture> texture=std::make_shared<Texture>();
auto FindTexture(u32 address){return address==3?texture:nullptr;}
bool IsNativeBuffer(u32 address){return address==1;}
bool IsNativeShader(u32 address){return address==2;}
void Callback(u32 data){assert(HostDevice::syncs==0&&data==77);++HostDevice::callbacks;}
struct Dispatcher {auto GetFunction(u32 addr){return addr==9?&Callback:nullptr;}};
struct Kernel {Dispatcher d;auto function_dispatcher(){return &d;}};
namespace rex {namespace system {auto kernel_state(){static Kernel k;return &k;}}
 namespace ppc {template<class T> void GuestToHostFunction(void(*fn)(u32),u32 data){fn(data);}}}
'''+texture+'\n'+hooks+r'''
int main(){
 const bool enabled=std::getenv("LEGO_NATIVE_ASYNC_CPU_RESOURCES")!=nullptr;
 auto check=[&](u32 resource,bool safe){HostDevice::syncs=HostDevice::skips=0;
  BlockUntilNotBusyHook(resource);assert(HostDevice::skips==u32(enabled&&safe));
  assert(HostDevice::syncs==u32(!enabled||!safe));};
 check(0,false);check(100,false);check(1,true);check(2,true);
 for(u32 bits=0;bits<16;++bits){texture->surface=bits&1;texture->resolved_on_host=bits&2;texture->owns_guest_memory=bits&4;texture->depth_alias_generation=bits&8;
  check(3,bits==0);}
 HostDevice::syncs=0;BlockUntilIdleHook(nullptr);assert(HostDevice::syncs==1);
 HostDevice::syncs=0;BlockOnFenceHook(1);assert(HostDevice::syncs==1);
 HostDevice::syncs=0;InsertCallbackHook(nullptr,0,9,77);assert(HostDevice::syncs==0&&HostDevice::callbacks==0);
 HostDevice::completed=true;HostDevice::PollCompletionCallbacks();assert(HostDevice::callbacks==1);
 HostDevice::ready=false;HostDevice::syncs=0;InsertCallbackHook(nullptr,0,9,77);
 assert(HostDevice::syncs==0&&HostDevice::callbacks==1);
}
''')
exe=a.output/'cpu-resource-wait.exe'
subprocess.run([a.compiler,'-std=c++20',str(h),'-o',str(exe)],check=True)
for enabled in (False,True):
 env=os.environ.copy();env.pop('LEGO_NATIVE_ASYNC_CPU_RESOURCES',None)
 if enabled:env['LEGO_NATIVE_ASYNC_CPU_RESOURCES']='1'
 subprocess.run([str(exe.resolve())],check=True,env=env)
(a.output/'verification.json').write_text(json.dumps({'actual_native_hook_bodies':True,
 'opt_in_and_default_tested':True,'checks':['known CPU buffers/shaders','borrowed CPU textures',
 'all surface/resolved/owned/depth-alias texture exclusions','unknown resource fallback','explicit idle and fence waits',
 'GPU completion before guest callback','unavailable device does not call guest'],'passed':True,
 'real_GPU_gameplay_not_proven':True},indent=2))
print('Passed actual wait hooks: conservative default, CPU-only opt-in, exclusions and deferred callback ordering.')
