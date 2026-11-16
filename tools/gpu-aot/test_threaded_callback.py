"""Verify deferred callback registration using the production hook body."""
import argparse
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--compiler', default='clang++')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
a.output.mkdir(parents=True, exist_ok=True)
source = (root / 'rexlego/src/gpu_native/hooks_device.cpp').read_text()
start = source.index('void InsertCallbackHook(')
end = source.index('{', start) + 1
depth = 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
fixture = r'''
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_set>
#include <vector>
using u32 = uint32_t;
struct D3DDevice {};
bool worker = false, threaded = true;
int drains = 0, submission = 0, completed = 0;
std::deque<std::function<void()>> tasks;
std::deque<std::pair<int, std::function<void()>>> callbacks;
std::vector<u32> invoked;
bool NativeWorkerShouldQueue() { return threaded && !worker; }
void NativeWorkerSubmit(std::function<void()> task) { tasks.push_back(std::move(task)); }
void GuestFunction(u32 data) { assert(!worker); invoked.push_back(data); }
struct Dispatcher { auto GetFunction(u32 address) { return address == 1 ? &GuestFunction : nullptr; } };
struct Kernel { Dispatcher dispatcher; auto function_dispatcher() { return &dispatcher; } } kernel;
namespace rex::system { auto kernel_state() { return &kernel; } }
namespace rex::ppc {
template<class T> void GuestToHostFunction(void (*function)(u32), u32 data) { function(data); }
}
#define REXLOG_ERROR(...) ((void)0)
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
struct HostDevice {
 static auto LockRecording() {
  if (!worker) ++drains;
  static std::recursive_mutex mutex;
  return std::unique_lock(mutex);
 }
 static bool EnqueueCompletionCallback(std::function<void()> callback) {
  callbacks.emplace_back(submission, std::move(callback)); return true;
 }
 static void PollCompletionCallbacks() {
  if (worker) return;
  while (!callbacks.empty() && callbacks.front().first <= completed) {
   auto callback = std::move(callbacks.front().second); callbacks.pop_front(); callback();
  }
 }
};
''' + source[start:end] + r'''
int main() {
 InsertCallbackHook(nullptr, 0, 0, 99);
 assert(tasks.empty() && drains == 0);
 NativeWorkerSubmit([] { submission = 1; });
 InsertCallbackHook(nullptr, 0, 1, 11);
 NativeWorkerSubmit([] { submission = 2; });
 InsertCallbackHook(nullptr, 0, 1, 22);
 assert(drains == 0 && callbacks.empty() && invoked.empty());
 worker = true;
 while (!tasks.empty()) { auto task = std::move(tasks.front()); tasks.pop_front(); task(); }
 assert(drains == 0 && invoked.empty() && callbacks.size() == 2);
 worker = false;
 HostDevice::PollCompletionCallbacks(); assert(invoked.empty());
 completed = 1; HostDevice::PollCompletionCallbacks(); assert((invoked == std::vector<u32>{11}));
 completed = 2; HostDevice::PollCompletionCallbacks(); assert((invoked == std::vector<u32>{11, 22}));
 HostDevice::PollCompletionCallbacks(); assert(invoked.size() == 2);
 threaded = false;
 InsertCallbackHook(nullptr, 0, 1, 33);
 assert((invoked == std::vector<u32>{11, 22, 33}));
 InsertCallbackHook(nullptr, 0, 2, 44); assert(callbacks.empty());
}
'''
cpp = a.output / 'threaded-callback.cpp'
exe = a.output / 'threaded-callback.exe'
cpp.write_text(fixture)
subprocess.run([a.compiler, '-std=c++20', str(cpp), '-o', str(exe)], check=True, timeout=60)
subprocess.run([str(exe.resolve())], check=True, timeout=30)
print('Passed callback queue order, fence attribution, guest execution, null and inline paths.')
