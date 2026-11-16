"""Run the production ordered pool-copy body with a delayed recorder barrier."""
import argparse
from pathlib import Path
import subprocess


def body(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='clang++')
p.add_argument('--previous-source', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
a.output.mkdir(parents=True, exist_ok=True)
production = body((root / 'rexlego/src/gpu_native/hooks_buffer.cpp').read_text(),
                  'void PoolCopyRecorded(')
fixture = r'''
#include "gpu_native/pool_copy.h"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
using namespace legodimensions::gpu_native;
using u32=uint32_t;using u8=uint8_t;
#define CHECK(c) do {if(!(c)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#c);std::exit(23);}}while(0)
#define REXLOG_ERROR(...) ((void)0)
struct Batch {bool active=false,waited=false;std::vector<PhysicalCopyRange>textures,buffers,shaders;}g_pool_batch;
struct Memory {u8 data[1024]{};u8* TranslatePhysical(u32 at){return data+at;}} memory;
#define REX_KERNEL_MEMORY() (&::memory)
bool gpu_owned=false,gpu_destination=false,supported=true,accessible=true,destination_accessible=true,revoke_during_readback=false;
bool on_recorder=false;
bool OnNativeRecorderThread(){return on_recorder;}
u32 waits=0,readbacks=0,invalidations=0;
std::thread::id coordinator;
std::future<void> recorder;
std::vector<int> events;
void BeginPoolCopyBatch(){g_pool_batch.active=true;}
void legodimensions::gpu_native::FlushPoolCopyBatch(){g_pool_batch={};events.push_back(1);}
bool BatchAccessible(PhysicalCopyRange,bool write){return accessible&&(!write||destination_accessible);}
bool PoolCopyMayTouchTextures(PhysicalCopyRange){return true;}
bool PoolCopyMayTouchBuffers(PhysicalCopyRange){return true;}
bool PoolCopyMayTouchShaders(PhysicalCopyRange){return true;}
bool PoolCopyGpuSourceIndexed(PhysicalCopyRange){return gpu_owned;}
bool PoolCopyGpuDestinationIndexed(PhysicalCopyRange dst){
 return gpu_destination&&CopyRangesOverlap(dst,{256,128})&&!(dst.address<=256&&dst.address+dst.length>=384);
}
void WaitAllPreparation(){++waits;events.push_back(2);}
bool MaterializePoolCopyTextureSource(PhysicalCopyRange src){
 if(!gpu_owned)return true;
 CHECK(on_recorder || std::this_thread::get_id()!=coordinator);++readbacks;events.push_back(4);
 if(!supported)return false;
 std::memset(memory.data+src.address,0xa5,src.length);gpu_owned=false;
 if(revoke_during_readback)destination_accessible=false;return true;
}
bool MaterializePoolCopyTextureDestination(PhysicalCopyRange dst){
 if(!PoolCopyGpuDestinationIndexed(dst))return true;
 CHECK(on_recorder || std::this_thread::get_id()!=coordinator);++readbacks;events.push_back(4);
 if(!supported)return false;
 std::memset(memory.data+256,0xb6,128);gpu_destination=false;return true;
}
bool MaterializePoolCopyTextureDependencies(PhysicalCopyRange src,PhysicalCopyRange dst){
 return MaterializePoolCopyTextureSource(src)&&MaterializePoolCopyTextureDestination(dst);
}
void NativeRecorderSubmit(std::function<void()> run,bool present,bool barrier){
 CHECK(!on_recorder&&!present&&barrier&&waits);events.push_back(3);
 // Work is delayed until WaitExecuted: copying before that wait uses stale
 // guest backing, while attempting a tape readback runs on the wrong thread.
 recorder=std::async(std::launch::deferred,[run=std::move(run)]{
  std::thread worker(run);worker.join();
 });
}
u32 NativeRecorderBarrier(){return 7;}
void NativeRecorderWaitExecuted(u32 serial){CHECK(serial==7);recorder.get();events.push_back(5);}
void InvalidateCpuPhysicalMemory(u32,u32){++invalidations;events.push_back(6);}
''' + production + r'''
int main(){
 coordinator=std::this_thread::get_id();
 const PhysicalCopyRange src{64,64},dst{256,64};
 std::memset(memory.data+64,0x11,64);gpu_owned=true;
 PoolCopyRecorded(256,64,64,src,dst);
 CHECK(readbacks==1&&invalidations==1&&!gpu_owned);
 for(u32 i=0;i<64;++i)CHECK(memory.data[256+i]==0xa5);
 CHECK(events==std::vector<int>({1,2,3,4,5,6}));
 CHECK(g_pool_batch.textures.size()==1&&g_pool_batch.buffers.size()==1&&g_pool_batch.shaders.size()==1);
 // No GPU source needs no recorder round trip; physical overlap is memmove.
 events.clear();const auto old_readbacks=readbacks;
 for(u32 i=0;i<128;++i)memory.data[64+i]=u8(i);
 PoolCopyRecorded(96,64,128,{64,128},{96,128});
 for(u32 i=0;i<128;++i)CHECK(memory.data[96+i]==u8(i));
 CHECK(readbacks==old_readbacks&&invalidations==2);
 // Unsupported GPU layouts never publish stale guest backing as valid pixels.
 gpu_owned=true;supported=false;memory.data[256]=0x7b;
 PoolCopyRecorded(256,64,64,src,dst);
 CHECK(readbacks==2&&invalidations==2&&memory.data[256]==0x7b&&gpu_owned);
 // A lost destination after the fence wait also cannot be written.
 supported=true;revoke_during_readback=true;
 PoolCopyRecorded(256,64,64,src,dst);
 CHECK(invalidations==2&&readbacks==3&&memory.data[256]==0x7b);
 // A partial CPU overwrite must retain GPU pixels outside the destination.
 destination_accessible=true;revoke_during_readback=false;gpu_owned=false;gpu_destination=true;
 std::memset(memory.data+64,0xa5,128);std::memset(memory.data+256,0x11,128);
 const auto before_partial=readbacks;
 PoolCopyRecorded(256,64,64,src,dst);
 CHECK(readbacks==before_partial+1&&invalidations==3&&!gpu_destination);
 for(u32 i=0;i<64;++i)CHECK(memory.data[256+i]==0xa5&&memory.data[320+i]==0xb6);
 // Complete replacement needs neither destination preservation nor a wait.
 gpu_destination=true;const auto before_full=readbacks;
 PoolCopyRecorded(256,64,128,{64,128},{256,128});
 CHECK(readbacks==before_full&&invalidations==4);
 for(u32 i=0;i<128;++i)CHECK(memory.data[256+i]==0xa5);
 // Temporal hooks already run on the recorder: materialize in place, never
 // enqueue and wait for this same thread. Preserve failure atomicity too.
 on_recorder=true;gpu_destination=false;gpu_owned=true;events.clear();
 PoolCopyRecorded(256,64,64,src,dst);
 CHECK(events==std::vector<int>({1,2,4,6})&&invalidations==5&&!gpu_owned);
 for(u32 i=0;i<64;++i)CHECK(memory.data[256+i]==0xa5);
 gpu_owned=true;supported=false;events.clear();memory.data[256]=0x7b;
 PoolCopyRecorded(256,64,64,src,dst);
 CHECK(events==std::vector<int>({1,2,4})&&invalidations==5&&memory.data[256]==0x7b);
 puts("PASS production ordered copy: coordinator barrier and recorder inline, exact bytes, overlap, failure retention");
}
'''


def run(name, source):
    cpp = a.output.resolve() / (name + '.cpp')
    exe = cpp.with_suffix('.exe')
    cpp.write_text(source)
    subprocess.run([a.compiler, '-std=c++20', '-O2', '-I' + str(root / 'rexlego/src'),
                    str(cpp), '-o', str(exe)], check=True, timeout=45)
    return subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)


r = run('ordered-pool-source', fixture)
if r.returncode:
    raise RuntimeError(r.stdout + r.stderr)
print(r.stdout.strip())
partial_mutant = production.replace('PoolCopyGpuDestinationIndexed(dst)', 'false')
r = run('lost-partial-destination', fixture.replace(production, partial_mutant))
if r.returncode != 23:
    raise RuntimeError('Omitted destination preservation escaped the untouched-pixel regression')
print('PASS negative control: omitting partial destination preservation loses untouched GPU pixels')
if a.previous_source:
    previous = body(a.previous_source.read_text(), 'void PoolCopyRecorded(')
    r = run('previous-source', fixture.replace(production, previous))
    if r.returncode != 23:
        raise RuntimeError('Previous implementation escaped the GPU source regression')
    print('PASS negative control: accepted previous source fails GPU-source visibility')
