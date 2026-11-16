#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace legodimensions::gpu_native {
struct GeometryPublication {
  std::atomic<bool> recorded{false}, ready{false};
};
// A deferred tape can be abandoned. Destruction alone proves nothing unless
// the copy reached a real command list; ordinary resource retirement then
// keeps this object through every live GPU fence, just like the buffer itself.
struct GeometryFenceCompletion {
  explicit GeometryFenceCompletion(std::shared_ptr<GeometryPublication> state)
      : state(std::move(state)) {}
  ~GeometryFenceCompletion() {
    if (state->recorded.load(std::memory_order_acquire))
      state->ready.store(true,std::memory_order_release);
  }
  std::shared_ptr<GeometryPublication> state;
};
struct GeometryContentKey {
  uint64_t content_hash=0, conversion_hash=0;
  uint32_t length=0, kind=0, format=0, stride=0, phase=0;
  bool operator==(const GeometryContentKey&) const = default;
};
struct GeometryContentKeyHash {
  size_t operator()(const GeometryContentKey& key) const {
    uint64_t hash=key.content_hash;
    for(uint64_t value:{key.conversion_hash,uint64_t(key.length),uint64_t(key.kind),
        uint64_t(key.format),uint64_t(key.stride),uint64_t(key.phase)})
      hash^=value+0x9E3779B97F4A7C15ull+(hash<<6)+(hash>>2);
    return size_t(hash);
  }
};

// The cache owns only bounded CPU snapshots and weak GPU references. Hashes
// select candidates; every raw byte and every conversion field must match.
// Callers must first validate a current guest-memory stamp. Only immutable
// uploads with ready publication may be consumed by another preparation job.
template<class Buffer>
class GeometryContentCache {
 public:
  static constexpr uint64_t kMaxBytes=32ull*1024*1024;
  static constexpr uint32_t kMaxEntries=4096,kMaxEntryBytes=4u*1024*1024;
  struct Counters {uint64_t hits=0,misses=0,pending=0,bytes=0,entries=0;};
  std::shared_ptr<Buffer> Find(const GeometryContentKey& key,
      std::span<const uint8_t> raw,std::span<const uint32_t> fields) {
    std::lock_guard lock(mutex_);
    if(raw.size()!=key.length || !Eligible(raw.size(),fields.size())) {++stats_.misses;return {};}
    auto [it,end]=index_.equal_range(key);
    while(it!=end) {
      auto current=it++;
      auto entry=current->second;
      auto buffer=entry->buffer.lock();
      if(!buffer) {Erase(entry);continue;}
      if(entry->fields.size()!=fields.size() ||
          !std::equal(fields.begin(),fields.end(),entry->fields.begin()) ||
          std::memcmp(raw.data(),entry->raw.data(),raw.size())!=0) continue;
      if(!entry->publication->ready.load(std::memory_order_acquire)) {++stats_.pending;continue;}
      entries_.splice(entries_.begin(),entries_,entry);
      ++stats_.hits;
      return buffer;
    }
    ++stats_.misses;
    return {};
  }
  void Remember(const GeometryContentKey& key,std::vector<uint8_t> raw,
      std::span<const uint32_t> fields,const std::shared_ptr<Buffer>& buffer,
      std::shared_ptr<GeometryPublication> publication) {
    if(!buffer || !publication || raw.size()!=key.length || !Eligible(raw.size(),fields.size())) return;
    const uint64_t charge=raw.size()+fields.size_bytes();
    std::lock_guard lock(mutex_);
    while(!entries_.empty() && (stats_.bytes>kMaxBytes-charge || stats_.entries>=kMaxEntries))
      Erase(std::prev(entries_.end()));
    entries_.push_front({key,std::move(raw),{fields.begin(),fields.end()},buffer,std::move(publication)});
    index_.emplace(key,entries_.begin());
    stats_.bytes+=charge;++stats_.entries;
  }
  static bool Eligible(size_t bytes,size_t fields) {
    return bytes && bytes<=kMaxEntryBytes && fields<=256;
  }
  void Reset() {std::lock_guard lock(mutex_);index_.clear();entries_.clear();stats_={};}
  Counters Stats() const {std::lock_guard lock(mutex_);return stats_;}
 private:
  struct Entry {
    GeometryContentKey key;
    std::vector<uint8_t> raw;
    std::vector<uint32_t> fields;
    std::weak_ptr<Buffer> buffer;
    std::shared_ptr<GeometryPublication> publication;
  };
  using Iterator=typename std::list<Entry>::iterator;
  void Erase(Iterator entry) {
    auto [it,end]=index_.equal_range(entry->key);
    for(;it!=end;++it) if(it->second==entry) {index_.erase(it);break;}
    stats_.bytes-=entry->raw.size()+entry->fields.size()*sizeof(uint32_t);
    --stats_.entries;entries_.erase(entry);
  }
  mutable std::mutex mutex_;
  Counters stats_;
  std::list<Entry> entries_;
  std::unordered_multimap<GeometryContentKey,Iterator,GeometryContentKeyHash> index_;
};
}  // namespace legodimensions::gpu_native
