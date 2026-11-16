#include "gpu_native/native_recording.h"
#include "gpu_native/native_preparation.h"
#include "gpu_native/renderer_optimizations.h"
#include <plume_d3d12.h>

#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>
#include <immintrin.h>

namespace legodimensions::gpu_native {
namespace {
using namespace plume;

// Payloads own every transient array and contain no guest-memory pointers.
class CommandTape {
  struct alignas(16) Block { std::byte bytes[16]; Block() noexcept {} };
  struct Header {
    void (*invoke)(RenderCommandList*, const std::byte*);
    uint32_t blocks, first, second;
    bool preparation;
  };
  static constexpr size_t Align(size_t n) { return (n + 15) & ~size_t(15); }
  std::vector<Block> storage_;
  std::vector<std::function<void(RenderCommandList*)>> external_;
  uint64_t upload_bytes_ = 0;
 public:
  template<class Call, class A = std::byte, class B = std::byte>
  void Push(Call call, std::span<const A> first = {}, std::span<const B> second = {}, bool preparation = false) {
    static_assert(std::is_trivially_copyable_v<Call> && alignof(Call) <= 16);
    static_assert(std::is_trivially_copyable_v<A> && std::is_trivially_copyable_v<B> && alignof(A) <= 16 && alignof(B) <= 16);
    constexpr size_t call_at = Align(sizeof(Header));
    constexpr size_t first_at = call_at + Align(sizeof(Call));
    const size_t second_at = first_at + Align(first.size_bytes());
    const size_t blocks = (second_at + Align(second.size_bytes())) / 16;
    const size_t start = storage_.size();
    storage_.resize(start + blocks);
    auto* data = reinterpret_cast<std::byte*>(storage_.data() + start);
    const Header header{[](RenderCommandList* list, const std::byte* data) {
      const auto& h = *reinterpret_cast<const Header*>(data);
      const auto& invoke = *reinterpret_cast<const Call*>(data + call_at);
      const auto* a = reinterpret_cast<const A*>(data + first_at);
      const auto* b = reinterpret_cast<const B*>(data + first_at + Align(size_t(h.first) * sizeof(A)));
      invoke(list, std::span<const A>(a, h.first), std::span<const B>(b, h.second));
    }, uint32_t(blocks), uint32_t(first.size()), uint32_t(second.size()), preparation};
    std::memcpy(data, &header, sizeof(header));
    std::memcpy(data + call_at, &call, sizeof(call));
    if (!first.empty()) std::memcpy(data + first_at, first.data(), first.size_bytes());
    if (!second.empty()) std::memcpy(data + second_at, second.data(), second.size_bytes());
    if (preparation) upload_bytes_ += first.size_bytes();
  }
  void External(std::function<void(RenderCommandList*)> command) {
    const size_t index = external_.size();
    external_.push_back(std::move(command));
    Push([this, index](RenderCommandList* list, auto, auto) { external_[index](list); });
  }
  void Play(RenderCommandList* list) const {
    for (size_t at = 0; at < storage_.size();) {
      const auto* data = reinterpret_cast<const std::byte*>(storage_.data() + at);
      const auto& header = *reinterpret_cast<const Header*>(data);
      if (!header.preparation) header.invoke(list, data);
      at += header.blocks;
    }
  }
  void Prepare() const {
    for (size_t at = 0; at < storage_.size();) {
      const auto* data = reinterpret_cast<const std::byte*>(storage_.data() + at);
      const auto& header = *reinterpret_cast<const Header*>(data);
      if (header.preparation) header.invoke(nullptr, data);
      at += header.blocks;
    }
    _mm_sfence();
  }
  bool HasPreparation() const { return upload_bytes_ != 0; }
  bool Empty() const { return storage_.empty(); }
  size_t Blocks() const { return storage_.size(); }
  // Appends a copy of blocks [begin, end), which hold no external or preparation commands.
  void Repeat(size_t begin, size_t end) {
    const size_t start = storage_.size();
    storage_.resize(start + (end - begin));
    std::memcpy(storage_.data() + start, storage_.data() + begin, (end - begin) * sizeof(Block));
  }
  // Drops blocks from `begin` on, which hold no external or preparation commands.
  void Truncate(size_t begin) { storage_.resize(begin); }
  size_t Bytes() const { return storage_.size() * sizeof(Block); }
  void Clear() { storage_.clear(); external_.clear(); upload_bytes_ = 0; }
};

class RecordingQueue {
  struct Work { RenderCommandList* list; std::unique_ptr<CommandTape> tape; std::future<void> prepared; };
  std::mutex mutex_;
  std::condition_variable ready_, progress_;
  std::deque<Work> work_;
  std::vector<std::unique_ptr<CommandTape>> pool_;
  std::thread worker_;
  size_t pending_ = 0, bytes_ = 0;
  bool stop_ = false;
  std::exception_ptr failure_;
  void Run() {
    SetThreadDescription(GetCurrentThread(), L"Native GPU recording");
    for (;;) {
      Work work;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] { return stop_ || !work_.empty(); });
        if (work_.empty()) return;
        work = std::move(work_.front()); work_.pop_front();
      }
      std::exception_ptr failure;
      try {
        if (work.prepared.valid()) work.prepared.get();
        work.tape->Play(work.list);
      } catch (...) { failure = std::current_exception(); }
      const auto bytes = work.tape->Bytes();
      work.tape->Clear();
      {
        std::lock_guard lock(mutex_);
        if (failure && !failure_) failure_ = failure;
        bytes_ -= bytes; --pending_;
        if (pool_.size() < 128) pool_.push_back(std::move(work.tape));
      }
      progress_.notify_all();
    }
  }
 public:
  RecordingQueue() { worker_ = std::thread([this] { Run(); }); }
  ~RecordingQueue() {
    { std::lock_guard lock(mutex_); stop_ = true; }
    ready_.notify_one();
    worker_.join();
  }
  std::unique_ptr<CommandTape> Take() {
    std::lock_guard lock(mutex_);
    if (pool_.empty()) return std::make_unique<CommandTape>();
    auto tape = std::move(pool_.back()); pool_.pop_back(); return tape;
  }
  void Post(RenderCommandList* list, std::unique_ptr<CommandTape> tape) {
    std::unique_lock lock(mutex_);
    progress_.wait(lock, [&] { return pending_ < 128 && bytes_ < (64u << 20); });
    if (failure_) std::rethrow_exception(failure_);
    bytes_ += tape->Bytes(); ++pending_;
    work_.push_back({list, std::move(tape), {}});
    auto& queued = work_.back();
    try {
      if (queued.tape->HasPreparation())
        queued.prepared = PostNativePreparation([owned = queued.tape.get()] { owned->Prepare(); }, queued.tape->Bytes());
    } catch (...) {
      bytes_ -= queued.tape->Bytes(); --pending_;
      work_.pop_back();
      throw;
    }
    ready_.notify_one();
  }
  void Wait() {
    std::unique_lock lock(mutex_);
    progress_.wait(lock, [&] { return pending_ == 0; });
    if (failure_) std::rethrow_exception(failure_);
  }
};

std::shared_ptr<RecordingQueue> RecordingQueueInstance() {
  static std::mutex mutex;
  static std::weak_ptr<RecordingQueue> instance;
  std::lock_guard lock(mutex);
  auto queue = instance.lock();
  if (!queue) { queue = std::make_shared<RecordingQueue>(); instance = queue; }
  return queue;
}

// State already applied to a list is not applied again; raw list access and external commands forget it.
struct ReplayState {
  RenderCommandList* list = nullptr;
  uint64_t raw_epoch = 0;
  bool pipeline_set = false, layout_set = false, framebuffer_set = false;
  bool viewport_set = false, scissor_set = false, index_set = false, bias_set = false;
  bool blend_set = false;
  std::array<float, 4> blend{};
  const RenderPipeline* pipeline = nullptr;
  const RenderPipelineLayout* layout = nullptr;
  const RenderFramebuffer* framebuffer = nullptr;
  RenderViewport viewport{};
  RenderRect scissor{};
  bool index_bound = false;
  RenderIndexBufferView index{};
  float bias[3]{};
  std::array<bool, 8> sets_set{}, roots_set{};
  std::array<RenderDescriptorSet*, 8> sets{};
  std::array<RenderBufferReference, 8> roots{};
  struct Vertex { bool set = false, slot_set = false; RenderVertexBufferView view{}; RenderInputSlot slot{}; };
  std::array<Vertex, 16> vertices{};
};
std::atomic<uint64_t> g_raw_epoch{1};
ReplayState& ReplayStateFor(RenderCommandList* list) {
  static thread_local ReplayState state;
  const auto epoch = g_raw_epoch.load(std::memory_order_acquire);
  if (state.list != list || state.raw_epoch != epoch) { state = {}; state.list = list; state.raw_epoch = epoch; }
  return state;
}
void ResetReplayState(RenderCommandList* list) { auto& state = ReplayStateFor(list); state = {}; state.list = list; state.raw_epoch = g_raw_epoch.load(std::memory_order_acquire); }
void InvalidateReplayState() { g_raw_epoch.fetch_add(1, std::memory_order_acq_rel); }
template<class T> bool SameBytes(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }

class PreparedCommandList final : public NativeDrawCommands {
  static constexpr size_t kNoTail = ~size_t{0};
  std::shared_ptr<RecordingQueue> queue_;
  std::unique_ptr<RenderCommandList> list_;
  std::unique_ptr<CommandTape> tape_;
  unsigned draws_ = 0;
  // Shape for merged draws: state settings, then draws only.
  bool shape_ok_ = true, frozen_ = false;
  size_t tail_begin_ = kNoTail, tail_end_ = 0, frozen_begin_ = 0, frozen_end_ = 0;
  uint64_t tail_commands_ = 0, frozen_commands_ = 0;
  // The last draw recorded and the frozen tail's last draw.
  struct Draw { bool indexed; uint32_t n, instances, first; int32_t vertex; uint32_t instance; };
  Draw last_draw_{}, frozen_draw_{};
  template<class Call, class A = std::byte, class B = std::byte>
  void Record(Call call, std::span<const A> first = {}, std::span<const B> second = {}) {
    tape_->Push(call, first, second);
    if (tape_->Bytes() >= 256 * 1024) Flush();
  }
  // Any command that is neither a pure state setting nor a draw.
  template<class Call, class A = std::byte, class B = std::byte>
  void Push(Call call, std::span<const A> first = {}, std::span<const B> second = {}) {
    shape_ok_ = false;
    Record(call, first, second);
  }
  // Sets state the replay keeps until changed; setting it again to the same value changes nothing.
  template<class Call, class A = std::byte, class B = std::byte>
  void PushState(Call call, std::span<const A> first = {}, std::span<const B> second = {}) {
    if (tail_begin_ != kNoTail) shape_ok_ = false;
    Record(call, first, second);
  }
  template<class Call, class A = std::byte, class B = std::byte>
  void PushDraw(Call call, std::span<const A> first = {}, std::span<const B> second = {}) {
    const size_t begin = tape_->Blocks();
    Record(call, first, second);
    if (tail_begin_ == kNoTail) tail_begin_ = begin;
    tail_end_ = tape_->Blocks();
    ++tail_commands_;
  }
 public:
  PreparedCommandList() : tape_(std::make_unique<CommandTape>()) {}
  explicit PreparedCommandList(std::unique_ptr<RenderCommandList> list)
      : queue_(RecordingQueueInstance()), list_(std::move(list)), tape_(queue_->Take()) {}
  void Replay(RenderCommandList* commands) override {
    if (RendererOptimizationEnabled(kSkipEmptyCommandTapes) && tape_->Empty()) return;
    if (tape_->HasPreparation()) tape_->Prepare();
    tape_->Play(commands);
  }
  void Reset() override {
    tape_->Clear(); draws_ = 0;
    shape_ok_ = true; frozen_ = false; tail_begin_ = kNoTail; tail_end_ = 0; tail_commands_ = 0;
  }
  bool Empty() override { return tape_->Empty(); }
  bool FreezeDrawTail() override {
    if (frozen_) return true;
    if (!shape_ok_ || tail_begin_ == kNoTail || tail_end_ != tape_->Blocks()) return false;
    frozen_ = true;
    frozen_begin_ = tail_begin_; frozen_end_ = tail_end_; frozen_commands_ = tail_commands_;
    frozen_draw_ = last_draw_;
    return true;
  }
  void RepeatDrawTail() override {
    if (!frozen_) throw std::logic_error("Draw tail repeated while not frozen");
    tape_->Repeat(frozen_begin_, frozen_end_);
  }
  bool InstanceDrawTail(const RenderPipeline* pipeline, RenderBufferReference constants, uint32_t instances) override {
    const Draw draw = frozen_draw_;
    if (!frozen_ || !pipeline || !instances || tape_->Blocks() != frozen_end_ || frozen_commands_ != 1 ||
        draw.instances != 1 || draw.instance != 0) return false;
    tape_->Truncate(frozen_begin_);
    if (draws_) --draws_;
    // The list no longer ends in a repeatable tail: nothing freezes, repeats or instances it again.
    frozen_ = false; tail_begin_ = kNoTail; tail_end_ = 0; tail_commands_ = 0;
    setPipeline(pipeline);
    setGraphicsRootDescriptor(constants, 4);
    if (draw.indexed) drawIndexedInstanced(draw.n, instances, draw.first, draw.vertex, 0);
    else drawInstanced(draw.n, instances, draw.first, 0);
    shape_ok_ = false;
    return true;
  }
  ~PreparedCommandList() override { try { Immediate(); } catch (...) {} }
  void Flush() override {
    if (!queue_ || !tape_ || tape_->Empty()) return;
    shape_ok_ = frozen_ = false;
    queue_->Post(list_.get(), std::move(tape_));
    tape_ = queue_->Take();
    draws_ = 0;
  }
  RenderCommandList* Immediate() override {
    if (!queue_) throw std::logic_error("Immediate command access during draw preparation");
    Flush(); queue_->Wait(); InvalidateReplayState(); return list_.get();
  }
  void External(std::function<void(RenderCommandList*)> command) override {
    shape_ok_ = false;
    tape_->External([command = std::move(command)](RenderCommandList* list) { command(list); ResetReplayState(list); });
  }
  void ResourceCommand(std::function<void(RenderCommandList*)> command) override {
    shape_ok_ = false;
    tape_->External([command = std::move(command)](RenderCommandList* list) {
      if (auto* deferred = dynamic_cast<NativeDeferredCommands*>(list))
        deferred->ResourceCommand(command);
      else command(list);
    });
  }
  void SetBlendFactor(const std::array<float, 4>& value) override {
    PushState([value](auto* commands, auto, auto) {
      auto& state = ReplayStateFor(commands);
      if (state.blend_set && SameBytes(state.blend, value)) return;
      static_cast<D3D12CommandList*>(commands)->d3d->OMSetBlendFactor(value.data());
      state.blend = value;
      state.blend_set = true;
    });
  }
  void Upload(void* destination, const void* source, uint32_t bytes) override {
    if (!queue_) { std::memcpy(destination, source, bytes); return; }
    shape_ok_ = false;
    tape_->Push([destination](auto*, auto data, auto) { std::memcpy(destination,data.data(),data.size_bytes()); },
        std::span(static_cast<const std::byte*>(source),bytes), std::span<const std::byte>{}, true);
    if (tape_->Bytes() >= 256 * 1024) Flush();
  }
  void begin() override { Push([](auto* c, auto, auto) { c->begin(); ResetReplayState(c); }); }
  void end() override { Push([](auto* c, auto, auto) { c->end(); ResetReplayState(c); }); Immediate(); }
  void barriers(RenderBarrierStages stages, const RenderBufferBarrier* buffers, uint32_t nb, const RenderTextureBarrier* textures, uint32_t nt) override {
    const auto call = [stages](auto* c, auto b, auto t) { c->barriers(stages, b.data(), uint32_t(b.size()), t.data(), uint32_t(t.size())); };
    // A texture already in the requested layout gets no barrier unless it allows unordered access, so
    // texture-only transitions of such textures set state like any other setting.
    bool layouts_only = nb == 0;
    for (uint32_t i = 0; layouts_only && i < nt; ++i)
      layouts_only = !(static_cast<const D3D12Texture*>(textures[i].texture)->desc.flags & RenderTextureFlag::UNORDERED_ACCESS);
    if (!layouts_only) { Push(call, std::span(buffers, nb), std::span(textures, nt)); return; }
    PushState(call, std::span(buffers, nb), std::span(textures, nt));
  }
  void dispatch(uint32_t x, uint32_t y, uint32_t z) override { Push([=](auto* c, auto, auto) { c->dispatch(x,y,z); }); Flush(); }
  void drawInstanced(uint32_t n, uint32_t instances, uint32_t first, uint32_t instance) override {
    last_draw_ = {false, n, instances, first, 0, instance};
    PushDraw([=](auto* c, auto, auto) { c->drawInstanced(n,instances,first,instance); }); if (++draws_ == 32) Flush();
  }
  void drawIndexedInstanced(uint32_t n, uint32_t instances, uint32_t first, int32_t vertex, uint32_t instance) override {
    last_draw_ = {true, n, instances, first, vertex, instance};
    PushDraw([=](auto* c, auto, auto) { c->drawIndexedInstanced(n,instances,first,vertex,instance); }); if (++draws_ == 32) Flush();
  }
  void setPipeline(const RenderPipeline* p) override {
    PushState([=](auto* c, auto, auto) {
      auto& s = ReplayStateFor(c);
      if (s.pipeline_set && s.pipeline == p) return;
      s.pipeline_set = true; s.pipeline = p; c->setPipeline(p);
    });
  }
  void setComputePipelineLayout(const RenderPipelineLayout* p) override { Push([=](auto* c, auto, auto) { c->setComputePipelineLayout(p); }); }
  void setGraphicsPipelineLayout(const RenderPipelineLayout* p) override {
    PushState([=](auto* c, auto, auto) {
      auto& s = ReplayStateFor(c);
      if (s.layout_set && s.layout == p) return;
      s.layout_set = true; s.layout = p; c->setGraphicsPipelineLayout(p);
    });
  }
  void setRaytracingPipelineLayout(const RenderPipelineLayout* p) override { Push([=](auto* c, auto, auto) { c->setRaytracingPipelineLayout(p); }); }
  void setComputeDescriptorSet(RenderDescriptorSet* p, uint32_t i) override { Push([=](auto* c, auto, auto) { c->setComputeDescriptorSet(p,i); }); }
  void setGraphicsDescriptorSet(RenderDescriptorSet* p, uint32_t i) override {
    PushState([=](auto* c, auto, auto) {
      auto& s = ReplayStateFor(c);
      if (i < s.sets.size()) {
        if (s.sets_set[i] && s.sets[i] == p) return;
        s.sets_set[i] = true; s.sets[i] = p;
      }
      c->setGraphicsDescriptorSet(p,i);
    });
  }
  void setRaytracingDescriptorSet(RenderDescriptorSet* p, uint32_t i) override { Push([=](auto* c, auto, auto) { c->setRaytracingDescriptorSet(p,i); }); }
  void setGraphicsRootDescriptor(RenderBufferReference b, uint32_t i) override {
    PushState([=](auto* c, auto, auto) {
      auto& s = ReplayStateFor(c);
      if (i < s.roots.size()) {
        if (s.roots_set[i] && SameBytes(s.roots[i], b)) return;
        s.roots_set[i] = true; s.roots[i] = b;
      }
      c->setGraphicsRootDescriptor(b,i);
    });
  }
  void setIndexBuffer(const RenderIndexBufferView* p) override {
    const bool bound = p != nullptr; const auto v = bound ? *p : RenderIndexBufferView{};
    PushState([=](auto* c, auto, auto) {
      auto& s = ReplayStateFor(c);
      if (s.index_set && s.index_bound == bound && (!bound || SameBytes(s.index, v))) return;
      s.index_set = true; s.index_bound = bound; s.index = v;
      c->setIndexBuffer(bound ? &v : nullptr);
    });
  }
  void setVertexBuffers(uint32_t first, const RenderVertexBufferView* v, uint32_t n, const RenderInputSlot* s) override {
    PushState([first](auto* c, auto v, auto s) {
      auto& state = ReplayStateFor(c);
      const auto differs = [&](size_t i) {
        const auto& slot = state.vertices[first + i];
        return !slot.set || !SameBytes(slot.view, v[i]) || slot.slot_set != !s.empty() ||
            (!s.empty() && !SameBytes(slot.slot, s[i]));
      };
      if (first + v.size() > state.vertices.size()) {
        for (size_t i = 0; first + i < state.vertices.size() && i < v.size(); ++i) {
          auto& slot = state.vertices[first + i];
          slot.set = true; slot.view = v[i]; slot.slot_set = !s.empty();
          if (!s.empty()) slot.slot = s[i];
        }
        c->setVertexBuffers(first,v.data(),uint32_t(v.size()),s.empty() ? nullptr : s.data());
        return;
      }
      // Unchanged slots keep what the list already has; send the span from the first to the last change.
      size_t low = 0, high = v.size();
      while (low < high && !differs(low)) ++low;
      if (low == high) return;
      while (high > low && !differs(high - 1)) --high;
      for (size_t i = low; i < high; ++i) {
        auto& slot = state.vertices[first + i];
        slot.set = true; slot.view = v[i]; slot.slot_set = !s.empty();
        if (!s.empty()) slot.slot = s[i];
      }
      c->setVertexBuffers(uint32_t(first + low),v.data() + low,uint32_t(high - low),
          s.empty() ? nullptr : s.data() + low);
    }, std::span(v,n), std::span(s,s ? n : 0));
  }
  void setViewports(const RenderViewport* v, uint32_t n) override {
    PushState([](auto* c, auto v, auto) {
      auto& s = ReplayStateFor(c);
      if (v.size() == 1 && s.viewport_set && SameBytes(s.viewport, v[0])) return;
      s.viewport_set = v.size() == 1; if (s.viewport_set) s.viewport = v[0];
      c->setViewports(v.data(),uint32_t(v.size()));
    },std::span(v,n));
  }
  void setScissors(const RenderRect* v, uint32_t n) override {
    PushState([](auto* c, auto v, auto) {
      auto& s = ReplayStateFor(c);
      if (v.size() == 1 && s.scissor_set && SameBytes(s.scissor, v[0])) return;
      s.scissor_set = v.size() == 1; if (s.scissor_set) s.scissor = v[0];
      c->setScissors(v.data(),uint32_t(v.size()));
    },std::span(v,n));
  }
  void setFramebuffer(const RenderFramebuffer* p) override {
    PushState([=](auto* c, auto, auto) {
      auto& s = ReplayStateFor(c);
      if (s.framebuffer_set && s.framebuffer == p) return;
      s.framebuffer_set = true; s.framebuffer = p; c->setFramebuffer(p);
    });
  }
  void setDepthBias(float a, float b, float d) override {
    PushState([=](auto* c, auto, auto) {
      auto& s = ReplayStateFor(c);
      if (s.bias_set && s.bias[0] == a && s.bias[1] == b && s.bias[2] == d) return;
      s.bias_set = true; s.bias[0] = a; s.bias[1] = b; s.bias[2] = d; c->setDepthBias(a,b,d);
    });
  }
  void clearColor(uint32_t i, RenderColor color, const RenderRect* r, uint32_t n) override { Push([=](auto* c, auto r, auto) { c->clearColor(i,color,r.data(),uint32_t(r.size())); },std::span(r,n)); }
  void clearDepthStencil(bool depth, bool stencil, float z, uint32_t value, const RenderRect* r, uint32_t n) override { Push([=](auto* c, auto r, auto) { c->clearDepthStencil(depth,stencil,z,value,r.data(),uint32_t(r.size())); },std::span(r,n)); }
  void copyBufferRegion(RenderBufferReference dst, RenderBufferReference src, uint64_t n) override { Push([=](auto* c, auto, auto) { c->copyBufferRegion(dst,src,n); }); }
  void copyTextureRegion(const RenderTextureCopyLocation& dst, const RenderTextureCopyLocation& src, uint32_t x, uint32_t y, uint32_t z, const RenderBox* box) override {
    const bool bounded = box != nullptr; const auto bounds = bounded ? *box : RenderBox{};
    Push([=](auto* c, auto, auto) { c->copyTextureRegion(dst,src,x,y,z,bounded ? &bounds : nullptr); });
  }
  void copyBuffer(const RenderBuffer* dst, const RenderBuffer* src) override { Push([=](auto* c, auto, auto) { c->copyBuffer(dst,src); }); }
  void copyTexture(const RenderTexture* dst, const RenderTexture* src) override { Push([=](auto* c, auto, auto) { c->copyTexture(dst,src); }); }
  void resolveTexture(const RenderTexture* dst, const RenderTexture* src) override { Push([=](auto* c, auto, auto) { c->resolveTexture(dst,src); }); }
  void resolveTextureRegion(const RenderTexture* dst, uint32_t x, uint32_t y, const RenderTexture* src, const RenderRect* rect, RenderResolveMode mode) override {
    const bool bounded = rect != nullptr; const auto r = bounded ? *rect : RenderRect{};
    Push([=](auto* c, auto, auto) { c->resolveTextureRegion(dst,x,y,src,bounded ? &r : nullptr,mode); });
  }
  void discardTexture(const RenderTexture* p) override { Push([=](auto* c, auto, auto) { c->discardTexture(p); }); }
  void resetQueryPool(const RenderQueryPool* p, uint32_t i, uint32_t n) override { Push([=](auto* c, auto, auto) { c->resetQueryPool(p,i,n); }); }
  void writeTimestamp(const RenderQueryPool* p, uint32_t i) override { Push([=](auto* c, auto, auto) { c->writeTimestamp(p,i); }); }
  void setComputePushConstants(uint32_t r,const void* d,uint32_t o,uint32_t n) override { Immediate()->setComputePushConstants(r,d,o,n); }
  void setGraphicsPushConstants(uint32_t r,const void* d,uint32_t o,uint32_t n) override {
    if (!n) { Immediate()->setGraphicsPushConstants(r,d,o,n); return; }
    PushState([=](auto* c, auto data, auto) { c->setGraphicsPushConstants(r,data.data(),o,n); },
        std::span(static_cast<const uint32_t*>(d),(n + 3) / 4));
  }
  void setRaytracingPushConstants(uint32_t r,const void* d,uint32_t o,uint32_t n) override { Immediate()->setRaytracingPushConstants(r,d,o,n); }
  void traceRays(uint32_t x,uint32_t y,uint32_t z,RenderBufferReference b,const RenderShaderBindingGroupsInfo& i) override { Immediate()->traceRays(x,y,z,b,i); }
  void buildBottomLevelAS(const RenderAccelerationStructure* a,RenderBufferReference b,const RenderBottomLevelASBuildInfo& i) override { Immediate()->buildBottomLevelAS(a,b,i); }
  void buildTopLevelAS(const RenderAccelerationStructure* a,RenderBufferReference b,RenderBufferReference c,const RenderTopLevelASBuildInfo& i) override { Immediate()->buildTopLevelAS(a,b,c,i); }
};
} // namespace

std::unique_ptr<NativeDrawCommands> CreateNativeDrawCommands() {
  return std::make_unique<PreparedCommandList>();
}

bool NativeFullDrawPreparationEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("LEGO_NATIVE_FULL_DRAW_PREPARATION");
    return value && value[0] == '1' && NativePrepareEnabled();
  }();
  return enabled;
}

bool NativePrepareEnabled() {
  return NativePreparationWorkerCount() != 0;
}

NativePreparationQueue& PreparationQueueInstance() {
  static NativePreparationQueue queue(NativePreparationWorkerCount());
  return queue;
}
void StopNativePreparation() { PreparationQueueInstance().Shutdown(); }
std::future<void> PostNativePreparation(std::function<void()> work, size_t bytes) {
  return PreparationQueueInstance().Post(std::move(work), bytes);
}
void PostNativePreparationDetached(std::function<void()> work, size_t bytes) {
  PreparationQueueInstance().PostDetached(std::move(work), bytes);
}

unsigned NativePreparationWorkerCount() {
  static const unsigned count = [] {
    const char* value = std::getenv("LEGO_NATIVE_PREPARE_THREADS");
    return value ? unsigned(std::clamp(std::atoi(value), 0, 8)) : 0u;
  }();
  return count;
}

std::unique_ptr<plume::RenderCommandList> PrepareNativeCommandList(std::unique_ptr<plume::RenderCommandList> commands) {
  if (!commands || !NativePrepareEnabled() || NativeFullDrawPreparationEnabled()) return commands;
  return std::make_unique<PreparedCommandList>(std::move(commands));
}

void SetNativeBlendFactor(plume::RenderCommandList* commands, const std::array<float, 4>& value) {
  if (RendererOptimizationEnabled(kRetainedBlendState)) {
    if (auto* deferred = dynamic_cast<NativeDeferredCommands*>(commands)) {
      deferred->SetBlendFactor(value);
      return;
    }
  }
  RecordNativeCommand(commands, [value](auto* list) {
    static_cast<plume::D3D12CommandList*>(list)->d3d->OMSetBlendFactor(value.data());
    auto& state = ReplayStateFor(list);
    state.blend = value;
    state.blend_set = true;
  });
}

void ResetNativeReplayState(plume::RenderCommandList* commands) { ResetReplayState(commands); }

} // namespace legodimensions::gpu_native
