#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace native_heap_diagnostics {
enum class Operation : uint32_t { kAllocated = 1, kFreeBegin = 2, kFreeComplete = 3 };
struct Event {
  uint64_t sequence = 0, key = 0;
  uint32_t pointer = 0, heap = 0, caller = 0, frame = 0, thread = 0, bytes = 0;
  Operation operation{};
};
struct History { std::array<Event,8> events{}; uint32_t count = 0; };

// Diagnostics only. This records observations without interpreting allocator
// flags, changing guest memory, bypassing a free, or allocating in a hot hook.
// A bounded bucket can evict old observations; absence proves nothing.
class Journal {
  struct Bucket {
    std::atomic_flag busy = ATOMIC_FLAG_INIT;
    std::array<Event,8> events{};
    uint32_t next = 0;
  };
  std::array<Bucket,8192> buckets_{};
  std::atomic<uint64_t> sequence_{0};
  struct Lock {
    Bucket& bucket;
    explicit Lock(Bucket& value) : bucket(value) {
      while (bucket.busy.test_and_set(std::memory_order_acquire)) {}
    }
    ~Lock() { bucket.busy.clear(std::memory_order_release); }
  };
  static uint32_t Index(uint64_t key) {
    key ^= key >> 33; key *= 0xff51afd7ed558ccdull;
    key ^= key >> 33;
    return uint32_t(key) & 8191;
  }
 public:
  static uint64_t Key(uint32_t pointer) {
    if (pointer >= 0xA0000000u) {
      const uint32_t view = pointer < 0xC0000000u ? 0xA0000000u :
                            pointer < 0xE0000000u ? 0xC0000000u : 0xE0000000u;
      const uint64_t physical = uint64_t(pointer - view) + (view == 0xE0000000u ? 4096 : 0);
      if (physical < 0x20000000u) return (1ull << 32) | physical;
    }
    return pointer;
  }
  uint64_t Record(Operation operation, uint32_t pointer, uint32_t heap,
                  uint32_t caller, uint32_t frame, uint32_t thread, uint32_t bytes = 0) {
    if (!pointer) return 0;
    const uint64_t key = Key(pointer);
    auto& bucket = buckets_[Index(key)];
    Lock lock(bucket);
    const uint64_t sequence = sequence_.fetch_add(1,std::memory_order_relaxed) + 1;
    if (!sequence) return 0;
    bucket.events[bucket.next++ & 7] = {sequence,key,pointer,heap,caller,frame,thread,bytes,operation};
    return sequence;
  }
  History Read(uint32_t pointer) {
    History result;
    if (!pointer) return result;
    const uint64_t key = Key(pointer);
    auto& bucket = buckets_[Index(key)];
    Lock lock(bucket);
    // Walk oldest-to-newest without sorting or allocating under the lock.
    for (uint32_t n=0; n<8; ++n) {
      const auto& event = bucket.events[(bucket.next+n)&7];
      if (event.sequence && event.key == key) result.events[result.count++] = event;
    }
    return result;
  }
};
} // namespace native_heap_diagnostics
