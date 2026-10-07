"""Exercise production submission, ring reuse and retirement with 3/12 slots."""
import argparse
import ast
from pathlib import Path
import subprocess

from test_frame_ring_transaction import function


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--compiler", default="clang++")
    parser.add_argument("--negative-control", choices=("slot-count", "retirement"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = (root / "rexlego/src/gpu_native/device.cpp").read_text()
    # The shared boundary includes a disabled optional timestamp pointer;
    # test_gpu_timestamps separately executes the enabled production helper.
    # Share the existing fake-device boundary, while executing new production
    # bodies unchanged. These fakes do not measure GPU or presentation speed.
    tree = ast.parse((Path(__file__).with_name("test_frame_ring_transaction.py")).read_text())
    harness = next(ast.literal_eval(node.value) for node in ast.walk(tree)
                   if isinstance(node, ast.Assign) and any(
                       isinstance(target, ast.Name) and target.id == "harness"
                       for target in node.targets))
    harness = harness.replace("#include <algorithm>", "#include <algorithm>\n#include <cstdlib>\n#include <cstring>\n#include <mutex>\n#include <stdexcept>")
    harness = harness.replace('#include "gpu_native/descriptor_retirement.h"', '#include "gpu_native/descriptor_retirement.h"\n#include "gpu_native/completion_queue.h"\nusing legodimensions::gpu_native::CompletionQueue;')
    harness = harness.replace("int begins=0;void begin(){++begins;}",
                              "int begins=0,ends=0;void begin(){++begins;}void end(){++ends;}")
    harness = harness.replace(" int waits=0;", " int waits=0,submits=0;\n void executeCommandLists(RenderCommandList*,RenderCommandFence* base){++submits;++static_cast<D3D12CommandFence*>(base)->fenceValue;} ")
    start = harness.index("struct State {")
    state = harness[start:]
    state = state.replace(",3>", ",12>")
    state = state.replace("u64 completed_submission=0;", "u64 completed_submission=0,last_submission=0;")
    state = state.replace("u64 completed_submission=0,last_submission=0;", "u64 completed_submission=0,last_submission=0;CompletionQueue completion_callbacks;u64 callbacks_enqueued=0,callbacks_executed=0;")
    state = state.replace(" State(){for(u32 i=0;i<3;++i){", " State(u32 slots=3):command_slot_count(slots),retired_descriptors(8,slots){for(u32 i=0;i<slots;++i){")
    state = state.replace("  framebuffers.push_back(std::make_unique<plume::RenderFramebuffer>());", "  if(i<3)framebuffers.push_back(std::make_unique<plume::RenderFramebuffer>());")
    state = state.replace("  render_semaphores.push_back(std::make_unique<plume::RenderCommandSemaphore>());", "  if(i<3)render_semaphores.push_back(std::make_unique<plume::RenderCommandSemaphore>());")
    harness = harness[:start] + state
    harness += r'''
std::recursive_mutex recording_mutex;
std::mutex g_mutex;
std::unique_ptr<State> g_state;
constexpr u32 kFirstTextureSlot=0;
struct HostDevice {
 static auto LockRecording(){return std::unique_lock(recording_mutex);}
 static bool SubmitRecordedWork();
 static void RetireResource(std::shared_ptr<void>);
 static void UnregisterTexture(u32);
 static bool EnqueueCompletionCallback(std::function<void()>);
 static void PollCompletionCallbacks();
};
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void Environment(const char* value){
#ifdef _WIN32
 _putenv_s("LEGO_NATIVE_COMMAND_SLOTS",value?value:"");
#else
 if(value)setenv("LEGO_NATIVE_COMMAND_SLOTS",value,1);else unsetenv("LEGO_NATIVE_COMMAND_SLOTS");
#endif
}
'''
    bodies = "\n".join(function(source, signature) for signature in (
        "u32 NativeCommandSlotCount(", "bool CreateFrameRing(",
        "void RefreshCompletedSubmissionsLocked(", "void MarkSubmissionLocked(",
        "bool WaitForSubmissionLocked(", "bool WaitForSubmittedFramesLocked(",
        "bool RebuildSwapChain(", "plume::RenderCommandList* BeginFrameCommandsLocked(",
        "bool HostDevice::SubmitRecordedWork(", "void HostDevice::RetireResource(",
        "void HostDevice::UnregisterTexture(",
        "bool HostDevice::EnqueueCompletionCallback(", "void HostDevice::PollCompletionCallbacks(",
    ))
    if args.negative_control == "slot-count":
        bodies = bodies.replace("state.command_slot_count", "State::kFramesInFlight")
    tests = r'''
int main(){try{
 Environment(nullptr);Check(NativeCommandSlotCount()==3,"default slots changed");
 Environment("12");Check(NativeCommandSlotCount()==12,"12-slot opt-in rejected");
 for(const char* value:{"3","0","1","32","13","-12","12junk","12 ","012","999999999999999999999999",""}){
  Environment(value);Check(NativeCommandSlotCount()==3,"invalid/default selector changed");
 }
 Environment(nullptr);
 // Slot11 retirement is checked independently: old three-ticket traversal
 // must fail this oracle without executing an out-of-bounds access.
 {
  DescriptorRetirement retirement(8,12);
  Check(!retirement.Retire(3,1u<<11),"descriptor retained through slot11");
  bool freed=false;for(u32 i=0;i<11;++i)retirement.CompleteFrame(i,[&](u32){freed=true;});
  Check(!freed,"descriptor released before slot11");
  retirement.CompleteFrame(11,[&](u32 i){Check(i==3,"wrong descriptor");freed=true;});
  Check(freed,"descriptor never released after slot11");
 }
 for(u32 slots:{3u,12u}){
  g_state=std::make_unique<State>(slots);auto& state=*g_state;
  state.framebuffers.clear();state.render_semaphores.clear();state.swap_chain->resize_pending=false;
  allocation={0,0};Check(CreateFrameRing(state),"creation failed");
  Check(allocation.attempts==int(6+3*slots),"all selected command slots allocated");
  Check(state.framebuffers.size()==3&&state.render_semaphores.size()==3,"swap-chain image count changed");
  for(u32 i=0;i<slots;++i){
   auto* commands=BeginFrameCommandsLocked(state);Check(commands==state.command_lists[i].get(),"ring slot mismatch");
   Check(state.queue->waits==0,"unused slot waited");
   state.draw_upload_page=7;state.draw_upload_offset=1024;
   Check(HostDevice::SubmitRecordedWork(),"submission failed");
   Check(commands->ends==1&&state.last_submission==i+1&&state.frame_submitted[i],"submission order changed");
  }
  Check(state.frame_slot==0&&state.queue->submits==int(slots),"ring did not wrap exactly");
  const auto last=state.last_submission;Check(HostDevice::SubmitRecordedWork()&&state.last_submission==last,"empty submit created work");
  auto resource=std::make_shared<int>(17);std::weak_ptr<int> retained=resource;
  HostDevice::RetireResource(std::move(resource));HostDevice::UnregisterTexture(3);
  int callbacks=0;Check(HostDevice::EnqueueCompletionCallback([&]{++callbacks;}),"callback enqueue failed");
  HostDevice::PollCompletionCallbacks();Check(callbacks==0,"callback ran before final submission");
  for(u32 i=0;i<slots;++i){
   Check(!retained.expired()&&state.texture_slots[3],"resource or descriptor retired before final GPU fence");
   Check(BeginFrameCommandsLocked(state)==state.command_lists[i].get(),"reuse slot mismatch");
   Check(state.queue->waits==int(i+1)&&state.completed_submission==i+1,"fence completion order changed");
   Check(!state.frame_submitted[i]&&state.draw_upload_page==0&&state.draw_upload_offset==0,"reuse before fence/reset");
   HostDevice::PollCompletionCallbacks();Check(callbacks==(i+1==slots?1:0),"callback fence ordering changed");
   state.command_list_open=false;state.frame_slot=(i+1)%slots;
  }
  Check(retained.expired()&&!state.texture_slots[3],"resource/descriptor retained after all fences");
  g_state.reset();Check(Tracked::live==0,"ring resources leaked");
  // Every selected-slot allocation failure leaves prior ring publication intact.
  for(int failure=1;failure<=int(6+3*slots);++failure){
   State state(slots);auto* old=state.command_lists[slots-1].get();
   allocation={0,failure};Check(!CreateFrameRing(state),"allocation failure accepted");
   Check(state.command_lists[slots-1].get()==old&&state.framebuffers.size()==3,"partial ring published");
   allocation={0,0};Check(CreateFrameRing(state),"allocation failure was not retryable");
  }
  Check(Tracked::live==0,"failure/retry resources leaked");
 }
 for(u32 invalid:{0u,33u,UINT32_MAX}){bool rejected=false;try{DescriptorRetirement d(8,invalid);}catch(const std::invalid_argument&){rejected=true;}Check(rejected,"invalid retirement slot count accepted");}
 DescriptorRetirement maxslots(8,32);Check(!maxslots.Retire(3,1u<<31),"maximum mask bit lost");
 bool freed=false;maxslots.CompleteFrame(31,[&](u32){freed=true;});Check(freed,"maximum ticket not completed");
 std::cout<<"PASS: production 3/12-slot submission/reuse, exact fence retirement and allocation retries.\n";
 return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 17;}}
'''
    args.output.mkdir(parents=True, exist_ok=True)
    header = (root / "rexlego/src/gpu_native/descriptor_retirement.h").read_text()
    if args.negative_control == "retirement":
        header = header.replace("frame < tickets_.size()", "frame < std::min(tickets_.size(),std::size_t{3})")
    (args.output / "gpu_native").mkdir(exist_ok=True)
    (args.output / "gpu_native/descriptor_retirement.h").write_text(header)
    cpp = args.output.resolve() / "command-slots.cpp"
    exe = args.output.resolve() / "command-slots.exe"
    cpp.write_text(harness + bodies + tests)
    subprocess.run([args.compiler, "-std=c++20", "-UNDEBUG", "-I" + str(args.output.resolve()),
                    "-I" + str(root / "rexlego/src"), str(cpp), "-o", str(exe)], check=True, timeout=45)
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
    print(result.stdout, end="")
    print(result.stderr, end="")
    if args.negative_control:
        oracle = "all selected command slots allocated" if args.negative_control == "slot-count" else "descriptor retained through slot11"
        if result.returncode != 17 or oracle not in result.stderr:
            raise RuntimeError("Negative control missed its required production oracle")
        print(args.negative_control + " negative control rejected as expected")
    else:
        result.check_returncode()


if __name__ == "__main__":
    main()
