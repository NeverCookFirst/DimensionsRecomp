"""Run production Plume command constructors/factories with failing fake COM APIs."""
import argparse
from pathlib import Path
import subprocess


def function(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--source", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    path = args.source or root / "thirdparty/plume/plume_d3d12.cpp"
    if not path.is_file():
        parser.error("Initialize public thirdparty/plume and run prepare_plume.py before this fixture")
    production = path.read_text()
    bodies = "\n".join(function(production, signature) for signature in (
        "D3D12CommandList::D3D12CommandList(", "D3D12CommandList::~D3D12CommandList(",
        "D3D12CommandFence::D3D12CommandFence(", "D3D12CommandFence::~D3D12CommandFence(",
        "D3D12CommandSemaphore::D3D12CommandSemaphore(", "D3D12CommandSemaphore::~D3D12CommandSemaphore(",
        "D3D12CommandQueue::D3D12CommandQueue(", "D3D12CommandQueue::~D3D12CommandQueue(",
        "std::unique_ptr<RenderCommandList> D3D12CommandQueue::createCommandList(",
        "std::unique_ptr<RenderCommandQueue> D3D12Device::createCommandQueue(",
        "std::unique_ptr<RenderCommandFence> D3D12Device::createCommandFence(",
        "std::unique_ptr<RenderCommandSemaphore> D3D12Device::createCommandSemaphore(",
    ))
    harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
using HRESULT=long;using HANDLE=std::uintptr_t;using UINT64=uint64_t;
#define FAILED(value) ((value)<0)
#define IID_PPV_ARGS(value) (value)
constexpr int FALSE=0,D3D12_FENCE_FLAG_NONE=0;
constexpr int D3D12_COMMAND_QUEUE_PRIORITY_NORMAL=0,D3D12_COMMAND_QUEUE_FLAG_NONE=0;
using D3D12_COMMAND_LIST_TYPE=int;
constexpr int D3D12_COMMAND_LIST_TYPE_DIRECT=0,D3D12_COMMAND_LIST_TYPE_COMPUTE=1,D3D12_COMMAND_LIST_TYPE_COPY=2;
struct D3D12_COMMAND_QUEUE_DESC {int Priority=0,Flags=0,NodeMask=0,Type=0;};
enum class Failure {None,Allocator,List,Fence,Event,Queue};
Failure failure=Failure::None;
struct Native {
 static inline int live=0;
 Native(){++live;}virtual ~Native(){--live;}
 void Release(){delete this;}
};
struct ID3D12CommandAllocator:Native {};
struct ID3D12GraphicsCommandList:Native {
 bool closed=false;
 template<class T>HRESULT QueryInterface(T** out){*out=nullptr;return -1;}
 HRESULT Close(){closed=true;return 0;}
};
struct ID3D12Fence:Native {UINT64 initial;explicit ID3D12Fence(UINT64 value):initial(value){}};
struct ID3D12CommandQueue:Native {int type;explicit ID3D12CommandQueue(int value):type(value){}};
int live_events=0;
HANDLE CreateEvent(void*,int manual,int initial,void*){
 assert(!manual&&!initial);if(failure==Failure::Event)return 0;
 ++live_events;return 1;
}
void CloseHandle(HANDLE event){assert(event==1&&live_events>0);--live_events;}
struct FakeDevice {
 HRESULT CreateCommandAllocator(int,ID3D12CommandAllocator** out){
  if(failure==Failure::Allocator)return -1;*out=new ID3D12CommandAllocator;return 0;
 }
 HRESULT CreateCommandList(int,int,ID3D12CommandAllocator* allocator,void*,ID3D12GraphicsCommandList** out){
  assert(allocator);if(failure==Failure::List)return -1;*out=new ID3D12GraphicsCommandList;return 0;
 }
 HRESULT CreateFence(UINT64 initial,int flags,ID3D12Fence** out){
  assert(flags==0);if(failure==Failure::Fence)return -1;*out=new ID3D12Fence(initial);return 0;
 }
 HRESULT CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* desc,ID3D12CommandQueue** out){
  if(failure==Failure::Queue)return -1;*out=new ID3D12CommandQueue(desc->Type);return 0;
 }
};
namespace plume {
enum class RenderCommandListType {DIRECT,COMPUTE,COPY,UNKNOWN};
struct RenderCommandList {virtual ~RenderCommandList()=default;};
struct RenderCommandFence {virtual ~RenderCommandFence()=default;};
struct RenderCommandSemaphore {virtual ~RenderCommandSemaphore()=default;};
struct RenderCommandQueue {virtual ~RenderCommandQueue()=default;};
struct D3D12Device;struct D3D12CommandQueue;
struct D3D12CommandList:RenderCommandList {
 ID3D12GraphicsCommandList* d3d=nullptr;
 ID3D12GraphicsCommandList* d3dV1=nullptr;
 ID3D12GraphicsCommandList* d3dV4=nullptr;
 ID3D12CommandAllocator* commandAllocator=nullptr;D3D12CommandQueue* queue=nullptr;
 D3D12CommandList(D3D12CommandQueue*);~D3D12CommandList()override;
};
struct D3D12CommandFence:RenderCommandFence {
 ID3D12Fence* d3d=nullptr;D3D12Device* device=nullptr;HANDLE fenceEvent=0;UINT64 fenceValue=0;
 D3D12CommandFence(D3D12Device*);~D3D12CommandFence()override;
};
struct D3D12CommandSemaphore:RenderCommandSemaphore {
 ID3D12Fence* d3d=nullptr;D3D12Device* device=nullptr;UINT64 semaphoreValue=0;
 D3D12CommandSemaphore(D3D12Device*);~D3D12CommandSemaphore()override;
};
struct D3D12CommandQueue:RenderCommandQueue {
 ID3D12CommandQueue* d3d=nullptr;D3D12Device* device=nullptr;RenderCommandListType type=RenderCommandListType::UNKNOWN;
 D3D12CommandQueue(D3D12Device*,RenderCommandListType);~D3D12CommandQueue()override;
 std::unique_ptr<RenderCommandList> createCommandList();
};
struct D3D12Device {
 FakeDevice* d3d;
 std::unique_ptr<RenderCommandQueue> createCommandQueue(RenderCommandListType);
 std::unique_ptr<RenderCommandFence> createCommandFence();
 std::unique_ptr<RenderCommandSemaphore> createCommandSemaphore();
};
'''
    tests = r'''
}
int main(){
 using namespace plume;FakeDevice native;D3D12Device device{&native};
 for(auto type:{RenderCommandListType::DIRECT,RenderCommandListType::COMPUTE,RenderCommandListType::COPY}){
  failure=Failure::Queue;assert(!device.createCommandQueue(type));assert(Native::live==0);
  failure=Failure::None;auto queue=device.createCommandQueue(type);assert(queue&&Native::live==1);
  auto* typed=static_cast<D3D12CommandQueue*>(queue.get());assert(typed->d3d&&typed->type==type);
  for(auto mode:{Failure::Allocator,Failure::List}){
   failure=mode;assert(!typed->createCommandList());
   assert(Native::live==1); // A failed list releases its partially created allocator.
  }
  failure=Failure::None;auto commands=typed->createCommandList();assert(commands&&Native::live==3);
  auto* list=static_cast<D3D12CommandList*>(commands.get());
  assert(list->d3d&&list->commandAllocator&&list->d3d->closed);
  commands.reset();assert(Native::live==1);queue.reset();assert(Native::live==0);
 }
 for(auto mode:{Failure::Fence,Failure::Event}){
  failure=mode;assert(!device.createCommandFence());
  assert(Native::live==0&&live_events==0); // Failed event creation releases the COM fence.
 }
 failure=Failure::None;auto fence=device.createCommandFence();assert(fence);
 auto* typed_fence=static_cast<D3D12CommandFence*>(fence.get());
 assert(typed_fence->d3d&&typed_fence->d3d->initial==0&&typed_fence->fenceValue==1);
 assert(typed_fence->fenceEvent&&live_events==1&&Native::live==1);
 fence.reset();assert(Native::live==0&&live_events==0);
 failure=Failure::Fence;assert(!device.createCommandSemaphore());assert(Native::live==0);
 failure=Failure::None;auto semaphore=device.createCommandSemaphore();assert(semaphore);
 auto* typed_semaphore=static_cast<D3D12CommandSemaphore*>(semaphore.get());
 assert(typed_semaphore->d3d&&typed_semaphore->d3d->initial==1&&typed_semaphore->semaphoreValue==1);
 semaphore.reset();assert(Native::live==0&&live_events==0);
 std::cout<<"PASS: Plume command factories propagate allocator/list/fence/event/queue failures and clean partial resources.\n";
}
'''
    args.output.mkdir(parents=True, exist_ok=True)
    source = args.output.resolve() / "plume-command-creation.cpp"
    exe = args.output.resolve() / "plume-command-creation.exe"
    source.write_text(harness + bodies + tests)
    subprocess.run([args.compiler, "-std=c++20", "-UNDEBUG", str(source), "-o", str(exe)],
                   check=True, timeout=45)
    subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    main()
