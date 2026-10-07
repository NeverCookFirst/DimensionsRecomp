"""Exercise production draw snapshots, recording identity, and lifecycle locks."""
import argparse
import ast
from pathlib import Path
import subprocess


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--compiler', default='clang++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    production = (root / 'rexlego/src/gpu_native/device.cpp').read_text()
    header = (root / 'rexlego/src/gpu_native/device.h').read_text()
    # Reuse the established fake driver, while compiling actual call-site
    # bodies below; this fixture does not reimplement snapshot/serial logic.
    existing = ast.parse((root / 'tools/gpu-aot/test_frame_ring_transaction.py').read_text())
    harness = next(ast.literal_eval(n.value) for n in ast.walk(existing)
                   if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'harness' for t in n.targets))
    harness = harness.replace('#include <vector>', '#include <vector>\n#include <cstdlib>\n#include <future>\n#include <mutex>\n#include <thread>\n#include <latch>')
    harness = harness.replace('namespace plume {', '''
namespace plume {
enum class RenderCommandListType {DIRECT};
enum class RenderFormat {B8G8R8A8_UNORM};
struct RenderSwapChainDesc {RenderSwapChainDesc(void*,RenderFormat,u32){}};
struct Queue;struct Swap;
struct RenderPipelineLayout {};struct RenderDescriptorSet {};struct RenderBuffer {};
''', 1)
    harness = harness.replace('struct Device {', '''struct Device {
 std::unique_ptr<Queue> createCommandQueue(RenderCommandListType);
 int getDescription(){return 0;}
''', 1)
    harness = harness.replace(' int waits=0;', ''' int waits=0,submissions=0;
 std::unique_ptr<Swap> createSwapChain(RenderSwapChainDesc);
 void executeCommandLists(RenderCommandList* commands,RenderCommandFence*) {assert(commands);++submissions;}
''', 1)
    harness = harness.replace('void begin(){++begins;}', 'void begin(){++begins;}void end(){}', 1)
    harness = harness.replace('// Disabled optional timestamp boundary;', '''
namespace plume {
using RenderDevice=Device;
struct RenderInterface {auto createDevice(){return std::make_unique<Device>();}};
std::unique_ptr<Queue> Device::createCommandQueue(RenderCommandListType){return std::make_unique<Queue>();}
std::unique_ptr<Swap> Queue::createSwapChain(RenderSwapChainDesc){return std::make_unique<Swap>();}
}
namespace rex::ui {struct Window {void* GetNativeWindowHandle(){return this;}};}
using HWND=void*;using HRESULT=int;
struct ID3D12Debug {void EnableDebugLayer(){}void Release(){}};
#define IID_PPV_ARGS(x) x
#define SUCCEEDED(x) ((x)>=0)
int D3D12GetDebugInterface(ID3D12Debug**){return -1;}
const char* NameOf(Backend){return "fake";}void InitializeLogoCapture(){}
constexpr const char* kNativeGpuBuildFingerprint="fixture";
bool CpuMemoryWatchEnabled(){return false;}
// Disabled optional timestamp boundary;''', 1)
    harness = harness.replace('struct GpuSubmissionTimestamps {', '''struct GpuSubmissionTimestamps {
 static std::unique_ptr<GpuSubmissionTimestamps> Create(auto*,auto*,u32,const char*,const char*){return {};}
 int Frequency(){return 1;}void Shutdown(u32){}
''', 1)
    harness = harness.replace('GpuSubmissionTimestamps* gpu_timestamps=nullptr;', 'std::unique_ptr<GpuSubmissionTimestamps> gpu_timestamps;', 1)
    harness = harness.replace('struct State {', '''
enum class SyncReason {kOther,kIdle,kFence,kResource,kCallback,kQueryBegin,kQueryRelease,kCount};
struct State {
 std::unique_ptr<plume::RenderInterface> render_interface;
 std::unique_ptr<plume::D3D12CommandFence> idle_fence;
 std::unique_ptr<plume::RenderPipelineLayout> pipeline_layout;
 std::unique_ptr<plume::RenderDescriptorSet> texture_descriptors,sampler_descriptors;
 std::unique_ptr<plume::RenderBuffer> null_vertex_buffer;
 struct CallbackQueue {size_t size(){return 0;}} completion_callbacks;
 u64 last_submission=0,sync_calls=0;double sync_ms=0;
 std::array<u64,static_cast<u32>(SyncReason::kCount)> sync_reason_calls{};
 std::array<double,static_cast<u32>(SyncReason::kCount)> sync_reason_ms{};
''', 1)
    harness += '\n' + function(header, 'struct DrawDeviceView {') + ';\n'
    harness += r'''
struct CountingMutex {
 std::mutex mutex;u64 acquisitions=0;
 void lock(){mutex.lock();++acquisitions;}void unlock(){mutex.unlock();}
};
CountingMutex g_mutex;
std::unique_ptr<State> g_state;
struct HostDevice {
 static std::unique_lock<std::recursive_mutex> LockRecording();
 static DrawDeviceView CurrentDrawDeviceView();
 static bool Create(rex::ui::Window*,Backend);
 static void Shutdown();
 static bool Synchronize(SyncReason);
 static bool SubmitRecordedWork();
};
void ResetDrawResources(){}void ResetQueryResources(){}void ResetShaderResources(){}
void ResetBufferResources(){}void ResetTextureResources(){}void ResetDrawBindings(){}
void ResetVertexDeclarations(){}void ShutdownCpuMemoryWatch(){}
auto CreateInterface(Backend backend){return backend==Backend::kD3D12?
 std::make_unique<plume::RenderInterface>():std::unique_ptr<plume::RenderInterface>{};}
bool CreatePipelineLayout(State& state){
 state.pipeline_layout=std::make_unique<plume::RenderPipelineLayout>();
 state.texture_descriptors=std::make_unique<plume::RenderDescriptorSet>();
 state.sampler_descriptors=std::make_unique<plume::RenderDescriptorSet>();return true;
}
bool CreatePresentPipeline(State&){return true;}
bool CreateNullVertexBuffer(State& state){state.null_vertex_buffer=std::make_unique<plume::RenderBuffer>();return true;}
bool CreateNullTextures(State&){return true;}
'''
    bodies = '\n'.join(function(production, signature) for signature in (
        'bool CreateFrameRing(', 'void RefreshCompletedSubmissionsLocked(',
        'void MarkSubmissionLocked(', 'bool WaitForSubmissionLocked(',
        'bool WaitForSubmittedFramesLocked(', 'bool RebuildSwapChain(',
        'plume::RenderCommandList* BeginFrameCommandsLocked(',
        'std::unique_lock<std::recursive_mutex> HostDevice::LockRecording(',
        'DrawDeviceView HostDevice::CurrentDrawDeviceView(',
        'bool HostDevice::Create(', 'void HostDevice::Shutdown(',
        'bool HostDevice::Synchronize(', 'bool HostDevice::SubmitRecordedWork('))
    # Actual acquire-failure close/submit transition, including frame advance.
    present = function(production, 'bool HostDevice::PresentTexture(')
    start = present.index('  if (!acquired || image_index >= state.framebuffers.size()) {')
    close = function(present[start:], '  if (!acquired || image_index >= state.framebuffers.size()) {')
    bodies += '\nbool AcquireFailed(State& state){bool acquired=false;u32 image_index=0,slot=state.frame_slot;auto* commands=state.command_lists[slot].get();\n' + close + '\nreturn true;}\n'
    tests = r'''
int main(){
 auto recording=HostDevice::LockRecording();
 assert(!HostDevice::CurrentDrawDeviceView().commands);
 rex::ui::Window window;
 assert(!HostDevice::Create(nullptr,Backend::kD3D12));assert(g_recording_serial==0);
 assert(!HostDevice::Create(&window,Backend::kVulkan));assert(g_recording_serial==0);
 assert(HostDevice::Create(&window,Backend::kD3D12));
 auto closed=HostDevice::CurrentDrawDeviceView();
 assert(closed.device&&closed.pipeline_layout&&closed.texture_descriptors&&closed.sampler_descriptors&&closed.null_vertex_buffer);
 assert(!closed.commands&&!closed.recording_serial);
 auto& state=*g_state;
 auto* commands=BeginFrameCommandsLocked(state);
 const auto locks_before=g_mutex.acquisitions;
 auto first=HostDevice::CurrentDrawDeviceView();
 assert(g_mutex.acquisitions==locks_before+1);
 assert(commands&&first.commands==commands&&first.recording_serial==1);
 assert(first.device==state.device.get()&&first.pipeline_layout==state.pipeline_layout.get());
 assert(first.texture_descriptors==state.texture_descriptors.get()&&first.sampler_descriptors==state.sampler_descriptors.get());
 assert(first.null_vertex_buffer==state.null_vertex_buffer.get());
 for(int i=0;i<100;++i){assert(BeginFrameCommandsLocked(state)==commands);assert(HostDevice::CurrentDrawDeviceView().recording_serial==1);}
 // Helper/present counters are not recording identities.
 ++state.present_number;assert(HostDevice::CurrentDrawDeviceView().recording_serial==1);
 assert(HostDevice::Synchronize(SyncReason::kFence));
 assert(!HostDevice::CurrentDrawDeviceView().commands&&!HostDevice::CurrentDrawDeviceView().recording_serial);
 assert(BeginFrameCommandsLocked(state)==commands);assert(HostDevice::CurrentDrawDeviceView().recording_serial==2);
 assert(HostDevice::SubmitRecordedWork());assert(!HostDevice::CurrentDrawDeviceView().recording_serial);
 assert(BeginFrameCommandsLocked(state));assert(HostDevice::CurrentDrawDeviceView().recording_serial==3);
 assert(!AcquireFailed(state));assert(!HostDevice::CurrentDrawDeviceView().recording_serial);
 assert(BeginFrameCommandsLocked(state));assert(HostDevice::CurrentDrawDeviceView().recording_serial==4);
 // Failed wait cannot begin, reclaim or advance the recording sequence.
 assert(HostDevice::SubmitRecordedWork());
 state.frame_submitted[state.frame_slot]=true;
 state.frame_fences[state.frame_slot]->native.completed=UINT64_MAX;
 const auto serial=g_recording_serial;auto* previous=state.command_lists[state.frame_slot].get();const auto previous_begins=previous->begins;
 assert(!BeginFrameCommandsLocked(state));assert(g_recording_serial==serial&&!state.command_list_open&&previous->begins==previous_begins);
 state.frame_fences[state.frame_slot]->native.completed=0;
 assert(BeginFrameCommandsLocked(state));assert(g_recording_serial==serial+1);
 // Production Create fast path and Shutdown must both respect the retained
 // recording guard, including the snapshot's raw pointer lifetime.
 std::latch create_started(1);
 auto create=std::async(std::launch::async,[&]{create_started.count_down();return HostDevice::Create(&window,Backend::kD3D12);});
 create_started.wait();assert(create.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);
 recording.unlock();assert(create.wait_for(std::chrono::seconds(3))==std::future_status::ready&&create.get());
 recording=HostDevice::LockRecording();
 auto live=HostDevice::CurrentDrawDeviceView();std::latch shutdown_started(1);
 auto shutdown=std::async(std::launch::async,[&]{shutdown_started.count_down();HostDevice::Shutdown();});
 shutdown_started.wait();assert(shutdown.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout);
 assert(HostDevice::CurrentDrawDeviceView().device==live.device&&g_state);
 recording.unlock();assert(shutdown.wait_for(std::chrono::seconds(3))==std::future_status::ready);shutdown.get();
 recording=HostDevice::LockRecording();assert(!HostDevice::CurrentDrawDeviceView().device);
 const auto before_recreate=g_recording_serial;
 assert(HostDevice::Create(&window,Backend::kD3D12));assert(BeginFrameCommandsLocked(*g_state));
 assert(HostDevice::CurrentDrawDeviceView().recording_serial==before_recreate+1);
 HostDevice::Shutdown();
 // Exhausted serial refuses the first recording before allocator/resource mutation.
 assert(HostDevice::Create(&window,Backend::kD3D12));g_recording_serial=UINT64_MAX;
 auto& overflow=*g_state;const auto begins=overflow.command_lists[overflow.frame_slot]->begins;
 overflow.draw_upload_offset=512;
 assert(!BeginFrameCommandsLocked(overflow));
 assert(overflow.command_lists[overflow.frame_slot]->begins==begins&&overflow.draw_upload_offset==512&&!overflow.command_list_open);
 HostDevice::Shutdown();assert(Tracked::live==0);
 std::cout<<"PASS: production snapshot identity, same-open/rebegin/submit/sync/acquire failure/recreation/overflow, retained recording excludes Create/Shutdown.\n";
}
'''
    args.output.mkdir(parents=True, exist_ok=True)
    source = args.output.resolve() / 'draw-device-view.cpp'
    binary = args.output.resolve() / 'draw-device-view'
    source.write_text(harness + bodies + tests)
    subprocess.run([args.compiler, '-std=c++20', '-UNDEBUG', '-pthread',
                    '-I' + str(root / 'rexlego/src'), str(source), '-o', str(binary)],
                   check=True, timeout=45)
    subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == '__main__':
    main()
