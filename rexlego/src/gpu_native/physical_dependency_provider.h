#pragma once

#include "gpu_native/pool_copy.h"
#include <rex/system/memory_data_provider.h>
#include <rex/system/xmemory.h>
#include <algorithm>
#include <bitset>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace legodimensions::gpu_native {

// Pending physical writes in producer submission order. This owns protection
// and callback lifetime only; it does not infer byte coverage or GPU versions.
// The client supplies immutable producer owners and must deliver every covered
// byte in ticket order before Settle returns true. Submission must not publish
// a ticket until earlier native CPU snapshots can read their preceding version.
// Ordinary guest accesses remain protected while Settle waits outside the SDK
// critical region. Native internal payload reads require their own ordered
// dependency snapshot path; recursively faulting this provider is fatal.
class PhysicalDependencyProvider {
 public:
  struct Api {
    rex::memory::RegisterPhysicalDataProvider add{};
    rex::memory::UnregisterPhysicalDataProvider remove{};
  };
  struct Ticket {
    uint64_t sequence{};
    std::vector<PhysicalCopyRange> ranges;
    std::shared_ptr<void> producer;
  };
  using Pending = std::shared_ptr<const Ticket>;
  using Settle = std::function<bool(std::span<const Pending>)>;
  // Stop must run outside renderer locks and the SDK critical region. In
  // direct-native mode there is no legacy GPU producer. If another client uses
  // the legacy slot, its producer must stop submitting writes before shutdown;
  // the final SDK provide pass may still deliver its already-pending writes.
  enum class StopProof { kLegacyProducerQuiesced };

  PhysicalDependencyProvider(rex::memory::Memory& memory, Api api, Settle settle)
      : memory_(memory), api_(api), settle_(std::move(settle)) {
    if (api_.add && api_.remove && settle_)
      registration_ = api_.add(&memory_, Callback, this);
  }
  ~PhysicalDependencyProvider() {
    // Never hide a GPU wait or leave a callback pointing at destroyed storage.
    if (registration_) std::terminate();
  }
  PhysicalDependencyProvider(const PhysicalDependencyProvider&) = delete;
  PhysicalDependencyProvider& operator=(const PhysicalDependencyProvider&) = delete;
  bool registered() const {
    auto critical = rex::thread::global_critical_region::AcquireDirect();
    return registration_ != nullptr;
  }
  bool accepting() const {
    auto critical = rex::thread::global_critical_region::AcquireDirect();
    return registration_ && accepting_;
  }

  Pending Track(uint64_t sequence, std::span<const PhysicalCopyRange> ranges,
                std::shared_ptr<void> producer) {
    if (!sequence || ranges.empty() || !producer) return {};
    auto ticket = std::make_shared<Ticket>(Ticket{sequence, {ranges.begin(), ranges.end()}, std::move(producer)});
    auto critical = rex::thread::global_critical_region::AcquireDirect();
    if (!registration_ || !accepting_ || sequence <= last_sequence_ || pending_.size() >= 4096) return {};
    for (const auto range : ticket->ranges)
      if (!range.length || range.address >= kLimit || range.length > kLimit - range.address ||
          !memory_.GetPhysicalHeap()->IsRangeCommittedReadable(range.address, range.length)) return {};
    // Allocate/publish the complete record before arming any page. Query only
    // inspects this list under the same SDK lock, with no locks or allocations.
    pending_.push_back(ticket);
    last_sequence_ = sequence;
    for (const auto range : ticket->ranges) {
      for (uint32_t p = range.address >> 12, end = (range.address + range.length - 1) >> 12; p <= end; ++p)
        tracked_pages_.set(p);
      memory_.EnablePhysicalMemoryDataProviders(range.address, range.length);
    }
    return ticket;
  }

  size_t pending_count() const {
    auto critical = rex::thread::global_critical_region::AcquireDirect();
    return pending_.size();
  }

  // Producer/resource destructors may acquire renderer locks. Drain their
  // retained tickets on a control thread outside the SDK critical region and
  // provider callbacks, never as a side effect of a Query/Provide invocation.
  bool Reap() {
    if (active_ == this) return false;
    std::vector<Pending> retired;
    {
      auto critical = rex::thread::global_critical_region::AcquireDirect();
      retired.swap(retired_);
    }
    return true;
  }

  bool Stop(StopProof) {
    if (active_ == this) return false;
    std::vector<PhysicalCopyRange> retired;
    {
      auto critical = rex::thread::global_critical_region::AcquireDirect();
      progress_.wait(critical, [&] { return !stopping_; });
      if (!registration_) return true;
      stopping_ = true;
      accepting_ = false;
      progress_.wait(critical, [&] { return !servicing_; });
      if (!pending_.empty() && !Deliver(critical, pending_)) {
        stopping_ = false;
        progress_.notify_all();
        return false;
      }
      // Remember deferred page bits, including tickets delivered by a fault.
      // Coalesce them so shutdown does not call the SDK once per page.
      for (uint32_t page = 0; page < kPages;) {
        if (!tracked_pages_.test(page)) { ++page; continue; }
        const auto first = page++;
        while (page < kPages && tracked_pages_.test(page)) ++page;
        retired.push_back({first << 12, (page - first) << 12});
      }
    }
    // This also preserves any legacy provider's existing pending dependency.
    // No new legacy writes may be submitted after the caller's StopProof.
    for (const auto range : retired) memory_.ProvidePhysicalMemory(range.address, range.length);
    {
      auto critical = rex::thread::global_critical_region::AcquireDirect();
      for (const auto range : retired) memory_.DisablePhysicalMemoryDataProviders(range.address, range.length);
    }
    // Unregister waits for callbacks, and therefore must be outside that lock.
    const bool removed = api_.remove(&memory_, registration_);
    Settle retired_settle;
    {
      auto critical = rex::thread::global_critical_region::AcquireDirect();
      if (removed) { registration_ = nullptr; retired_settle = std::move(settle_); }
      stopping_ = false;
      progress_.notify_all();
    }
    if (removed) Reap();
    return removed;
  }

 private:
  static constexpr uint32_t kLimit = 0x20000000, kPages = kLimit >> 12;
  static inline thread_local PhysicalDependencyProvider* active_ = nullptr;
  struct Active {
    PhysicalDependencyProvider* previous;
    explicit Active(PhysicalDependencyProvider* value) : previous(std::exchange(active_, value)) {}
    ~Active() { active_ = previous; }
  };
  bool Owns(uint32_t address) const {
    if (address >= kLimit) return false;
    const uint32_t page = address & ~4095u;
    for (const auto& ticket : pending_)
      for (const auto range : ticket->ranges)
        if (range.address < uint64_t(page) + 4096 && uint64_t(range.address) + range.length > page) return true;
    return false;
  }
  bool Deliver(std::unique_lock<std::recursive_mutex>& critical, std::vector<Pending> batch) {
    servicing_ = true;
    bool delivered = false;
    {
      Active active(this);
      critical.unlock();
      try { delivered = settle_(batch); } catch (...) { delivered = false; }
      critical.lock();
    }
    if (delivered) {
      retired_.insert(retired_.end(), batch.begin(), batch.end());
      std::erase_if(pending_, [&](const auto& ticket) {
        return std::find(batch.begin(), batch.end(), ticket) != batch.end();
      });
    }
    servicing_ = false;
    progress_.notify_all();
    // Do not disable protection here. The legacy slot cannot be queried, and
    // could have acquired different bytes on this page during the wait. The
    // SDK's ordinary fault retry calls both slots, then clears unowned page
    // bits. Stop performs explicit cleanup only after producer quiescence.
    return delivered;
  }
  std::vector<Pending> DependencyBatch(uint32_t page) const {
    // A producer can touch several pages. Publishing it must first settle
    // every older producer that overlaps any of its bytes, including through
    // a chain of intermediate producers. Otherwise a later fault on another
    // page can publish an old write over the version already made visible.
    // Scan backwards so each selected producer extends the dependency union
    // for all earlier producers. Disjoint newer/older writes remain pending.
    std::map<uint32_t, uint32_t> dependencies; // disjoint [start,end) intervals
    std::vector<uint8_t> included(pending_.size(), 0);
    for (size_t i = pending_.size(); i-- > 0;) {
      const auto& ticket = pending_[i];
      bool selected = std::any_of(ticket->ranges.begin(), ticket->ranges.end(), [&](auto range) {
        return range.address < uint64_t(page) + 4096 && uint64_t(range.address) + range.length > page;
      });
      if (!selected) {
        for (const auto range : ticket->ranges) {
          auto after = dependencies.lower_bound(range.address + range.length);
          if (after != dependencies.begin() && std::prev(after)->second > range.address) { selected = true; break; }
        }
      }
      if (!selected) continue;
      included[i] = 1;
      for (const auto range : ticket->ranges) {
        uint32_t first = range.address, end = first + range.length;
        auto it = dependencies.lower_bound(first);
        if (it != dependencies.begin() && std::prev(it)->second >= first) --it;
        while (it != dependencies.end() && it->first <= end) {
          first = std::min(first, it->first); end = std::max(end, it->second);
          it = dependencies.erase(it);
        }
        dependencies.emplace(first, end);
      }
    }
    std::vector<Pending> batch;
    for (size_t i = 0; i < pending_.size(); ++i) if (included[i]) batch.push_back(pending_[i]);
    return batch;
  }
  bool Provide(std::unique_lock<std::recursive_mutex>& critical, uint32_t address) {
    if (active_ == this) return false;
    while (Owns(address)) {
      if (servicing_) { progress_.wait(critical, [&] { return !servicing_; }); continue; }
      const uint32_t page = address & ~4095u;
      if (!Deliver(critical, DependencyBatch(page))) return false;
    }
    return true;
  }
  static bool Callback(std::unique_lock<std::recursive_mutex>& critical, void* context,
                       uint32_t address, rex::memory::PhysicalProviderOperation operation) {
    auto& self = *static_cast<PhysicalDependencyProvider*>(context);
    if (operation == rex::memory::PhysicalProviderOperation::kQuery) return self.Owns(address);
    return self.Provide(critical, address);
  }
  rex::memory::Memory& memory_;
  Api api_;
  Settle settle_;
  void* registration_{};
  bool accepting_ = true, servicing_ = false, stopping_ = false;
  uint64_t last_sequence_{};
  std::vector<Pending> pending_;
  std::vector<Pending> retired_;
  std::bitset<kPages> tracked_pages_;
  std::condition_variable_any progress_;
};

}  // namespace legodimensions::gpu_native
