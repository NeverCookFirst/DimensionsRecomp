#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace legodimensions::gpu_native {

template<class Resource>
class ResourceRegistry : private std::unordered_map<uint32_t, std::shared_ptr<Resource>> {
  using Base = std::unordered_map<uint32_t, std::shared_ptr<Resource>>;
  struct Entry {
    uint64_t registry = 0, generation = 0;
    uint32_t address = 0;
    std::weak_ptr<Resource> owner;
  };

 public:
  using typename Base::mapped_type;
  using Base::begin;
  using Base::end;
  using Base::find;
  using Base::contains;
  using Base::size;
  using Base::empty;
  ResourceRegistry() : identity_(next_identity_.fetch_add(1, std::memory_order_relaxed)) {
    for (auto& bucket : buckets_) bucket.store(1, std::memory_order_relaxed);
  }
  uint64_t Generation() const { return generation_.load(std::memory_order_acquire); }
  // Moves only when an entry may have been added; erasures leave it unchanged.
  uint64_t Insertions() const { return insertions_.load(std::memory_order_acquire); }
  // Keys added since the last take, under the registry mutex; false when more were added than recorded.
  bool TakeInsertedKeys(std::vector<uint32_t>& out) {
    out.swap(inserted_keys_);
    inserted_keys_.clear();
    return !std::exchange(inserted_overflow_, false);
  }
  // Bucket versions move only with registrations near one address, unlike the whole-registry generation.
  static uint32_t Bucket(uint32_t address) { return ((address >> 4) * 0x9E3779B9u) >> 20; }
  uint64_t BucketVersion(uint32_t address) const {
    return buckets_[Bucket(address)].load(std::memory_order_acquire);
  }

  mapped_type Lookup(uint32_t address, std::mutex& mutex) {
    if (!address) return {};
    static const bool enabled = [] {
      const char* value = std::getenv("LEGO_NATIVE_RESOURCE_LOOKUP_CACHE");
      return !value || value[0] != '0';
    }();
    static thread_local std::array<Entry, 128> entries;
    const uint32_t hash = (address ^ (address >> 12) ^ (address >> 23)) * 0x9E3779B9u;
    auto& entry = entries[hash >> 25];
    // Read before the find: a writer between them bumps the bucket past this value.
    const auto version = BucketVersion(address);
    if (enabled && entry.registry == identity_ &&
        entry.generation == version && entry.address == address) {
      if (auto owner = entry.owner.lock()) return owner;
    }
    std::lock_guard lock(mutex);
    const auto found = Base::find(address);
    if (found == Base::end()) return {};
    if (enabled) entry = {identity_, version, address, found->second};
    return found->second;
  }

  template<class Key, class... Args>
  auto emplace(Key&& key, Args&&... args) {
    insertions_.fetch_add(1, std::memory_order_release);
    RecordInsert(static_cast<uint32_t>(key));
    Advance(static_cast<uint32_t>(key));
    return Base::emplace(std::forward<Key>(key), std::forward<Args>(args)...);
  }

  auto erase(typename Base::iterator it) {
    Advance(it->first);
    return Base::erase(it);
  }
  auto erase(typename Base::const_iterator it) {
    Advance(it->first);
    return Base::erase(it);
  }
  auto erase(uint32_t key) {
    Advance(key);
    return Base::erase(key);
  }

  mapped_type& operator[](uint32_t address) {
    insertions_.fetch_add(1, std::memory_order_release);
    RecordInsert(address);
    Advance(address);
    return Base::operator[](address);
  }

  void clear() {
    for (auto& bucket : buckets_) bucket.fetch_add(1, std::memory_order_release);
    Advance(0);
    Base::clear();
  }

 private:
  // Registry writers hold the same mutex used by lookup misses.
  void Advance(uint32_t address) {
    buckets_[Bucket(address)].fetch_add(1, std::memory_order_release);
    const auto current = generation_.load(std::memory_order_relaxed);
    if (current) generation_.store(current + 1, std::memory_order_release);
  }

  inline static std::atomic<uint64_t> next_identity_{1};
  const uint64_t identity_;
  std::atomic<uint64_t> generation_{1};
  std::atomic<uint64_t> insertions_{1};
  // Bounded, so a registry nobody takes from does not grow.
  void RecordInsert(uint32_t key) {
    if (inserted_keys_.size() < 8192) inserted_keys_.push_back(key);
    else inserted_overflow_ = true;
  }
  std::vector<uint32_t> inserted_keys_;
  bool inserted_overflow_ = false;
  std::array<std::atomic<uint64_t>, 4096> buckets_{};
};

} // namespace legodimensions::gpu_native
