"""Exercise production framebuffer creation/invalidation/reset and owner lifetimes."""
import argparse
from pathlib import Path
import subprocess


def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--compiler', default='clang++')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    text = (root / 'rexlego/src/gpu_native/textures.cpp').read_text()
    key = text[text.index('struct FramebufferKey {'):text.index('struct SurfaceStorage {')]
    registry = text[text.index('std::mutex g_textures_mutex;'):text.index('std::atomic<u32> g_texture_lifecycle_logs')]
    bodies = '\n'.join(function(text, signature) for signature in [
        'void InvalidateFramebufferReferences(u32 guest_address)',
        'plume::RenderFramebuffer* ResolveFramebuffer(\n    const std::array<u32, 4>&',
        'void ResetTextureResources()'])
    harness = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
using u32 = uint32_t;
thread_local unsigned held_resource_locks = 0;
struct ResourceMutex {
  std::mutex value;
  void lock() { value.lock(); ++held_resource_locks; }
  void unlock() { --held_resource_locks; value.unlock(); }
};
namespace plume {
struct RenderTexture {};
struct RenderFramebuffer { static inline unsigned destroyed = 0; ~RenderFramebuffer() { ++destroyed; } };
struct RenderFramebufferDesc { const RenderTexture** colorAttachments{}; u32 colorAttachmentsCount{}; RenderTexture* depthAttachment{}; };
}
'''
    harness += key + r'''
struct TextureResource {
  u32 guest_address{}, mirror_address{};
  bool owns_guest_memory = false;
  bool depth_format = false;
  ResourceMutex mutex;
  std::unique_ptr<plume::RenderTexture> texture = std::make_unique<plume::RenderTexture>();
  int format = 0;
  std::unordered_map<FramebufferKey, std::shared_ptr<plume::RenderFramebuffer>, FramebufferKeyHash> framebuffers;
};
'''
    harness += registry + r'''
std::unordered_map<u32, std::weak_ptr<TextureResource>> g_depth_resolves, g_edram_colors;
unsigned g_depth_alias_generation = 0;
struct Memory { unsigned frees = 0; void SystemHeapFree(u32) { ++frees; } } guest_memory;
#define REX_KERNEL_MEMORY() (&guest_memory)
#define REXLOG_ERROR(...) ((void)0)
bool IsDepthFormat(int format) { return format != 0; }
std::shared_ptr<TextureResource> FindTexture(u32 address) {
  std::lock_guard lock(g_textures_mutex);
  auto it = g_textures.find(address); return it == g_textures.end() ? nullptr : it->second;
}
struct Device {
  bool fail = false;
  std::function<void()> before_create;
  std::shared_ptr<plume::RenderFramebuffer> createFramebuffer(const plume::RenderFramebufferDesc&) {
    if (before_create) before_create();
    return fail ? nullptr : std::make_shared<plume::RenderFramebuffer>();
  }
} device;
struct HostDevice {
  static inline std::recursive_mutex recording;
  static inline std::vector<std::shared_ptr<plume::RenderFramebuffer>> retired;
  static auto LockRecording() { return std::unique_lock(recording); }
  static auto Device() { return &device; }
  static void RetireResource(std::shared_ptr<plume::RenderFramebuffer> item) {
    assert(held_resource_locks == 0); // retirement occurs after each owner's mutex is released
    retired.push_back(std::move(item));
  }
};
'''
    harness += bodies + r'''
std::shared_ptr<TextureResource> Add(u32 address, bool depth = false) {
  auto r = std::make_shared<TextureResource>(); r->guest_address = address; r->format = depth;
  g_textures[address] = r; return r;
}
int main() {
  auto a = Add(1), b = Add(2), c = Add(3), d = Add(4, true), e = Add(5, true);
  // Thousands of sampled textures never become owners or incur invalidation locks.
  for (u32 i = 100; i < 10100; ++i) Add(i);
  const std::array<u32, 4> ab{1, 2, 0, 0}, ac{1, 3, 0, 0};
  auto first = ResolveFramebuffer(ab, 4); assert(first);
  assert(ResolveFramebuffer(ab, 4) == first);
  assert(ResolveFramebuffer(ac, 4)); assert(ResolveFramebuffer(ab, 5));
  assert(g_framebuffer_owners.size() == 2);
  InvalidateFramebufferReferences(2); // second color, cross-owner dependency
  assert(HostDevice::retired.size() == 2); assert(d->framebuffers.size() == 1);
  assert(e->framebuffers.empty()); assert(g_framebuffer_owners.size() == 1);
  assert(plume::RenderFramebuffer::destroyed == 0); // fence retirement retains old objects
  InvalidateFramebufferReferences(4); // depth dependency
  assert(d->framebuffers.empty()); assert(HostDevice::retired.size() == 3);
  assert(g_framebuffer_owners.empty());
  HostDevice::retired.clear(); assert(plume::RenderFramebuffer::destroyed == 3);
  assert(ResolveFramebuffer(ac, 4)); // emptied owner becomes indexed again
  // Same guest address can now name another resource generation. The retired
  // owner keeps its framebuffer until that owner retires; no new owner false hit.
  auto replacement = Add(4, true);
  InvalidateFramebufferReferences(1);
  assert(g_framebuffer_owners.empty()); assert(d->framebuffers.size() == 1);
  assert(ResolveFramebuffer(ab, 4));
  InvalidateFramebufferReferences(1); assert(replacement->framebuffers.empty());
  // Expired weak pointers are pruned and do not keep texture lifetimes alive.
  std::weak_ptr<TextureResource> expired;
  { auto dead = Add(20, true); expired = dead; g_framebuffer_owners[20] = dead; g_textures.erase(20); }
  assert(expired.expired()); InvalidateFramebufferReferences(123); assert(!g_framebuffer_owners.contains(20));
  // A failed creation must not register a phantom owner; attachment gaps remain rejected.
  device.fail = true; assert(!ResolveFramebuffer(ab, 5)); device.fail = false;
  assert(!g_framebuffer_owners.contains(5));
  assert(!ResolveFramebuffer(std::array<u32, 4>{1, 0, 3, 0}, 5));
  // Force invalidation to overlap the window before framebuffer creation/index
  // registration. It must wait for the recording lock, then see the new entry.
  std::promise<void> reached, release; auto released = release.get_future().share();
  device.before_create = [&] { reached.set_value(); released.wait(); };
  auto creator = std::async(std::launch::async, [&] { assert(ResolveFramebuffer(ab, 5)); });
  reached.get_future().wait();
  std::promise<void> invalidator_started;
  auto invalidator = std::async(std::launch::async, [&] { invalidator_started.set_value(); InvalidateFramebufferReferences(2); });
  invalidator_started.get_future().wait();
  assert(invalidator.wait_for(std::chrono::milliseconds(75)) == std::future_status::timeout);
  release.set_value(); creator.get(); invalidator.get(); device.before_create = {};
  assert(e->framebuffers.empty()); assert(!g_framebuffer_owners.contains(5));
  // Reset removes all weak generations and preserves the existing guest-free policy.
  assert(ResolveFramebuffer(ab, 4)); replacement->owns_guest_memory = true;
  replacement->mirror_address = 77; g_depth_resolves[9] = replacement; g_edram_colors[8] = a;
  g_depth_alias_generation = 123;
  ResetTextureResources(); assert(g_textures.empty() && g_framebuffer_owners.empty());
  assert(g_depth_resolves.empty() && g_edram_colors.empty() && g_depth_alias_generation == 0);
  assert(guest_memory.frees == 2);
  std::cout << "Production framebuffer-owner dependency/lifetime/concurrency regressions passed\n";
}
'''
    args.output.mkdir(parents=True, exist_ok=True)
    source = args.output.resolve() / 'test.cpp'
    binary = args.output.resolve() / 'test.exe'
    source.write_text(harness)
    subprocess.run([args.compiler, '-std=c++20', '-UNDEBUG', '-pthread', str(source), '-o', str(binary)], check=True, timeout=45)
    subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    main()
