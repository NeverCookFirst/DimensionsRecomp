#include "gpu_native/draw_preparation.h"
#include "gpu_native/tonemap_dof.h"
#include <cassert>
#include <deque>
#include <random>
#include <vector>

using namespace legodimensions::gpu_native;
struct Slice {
  void* buffer = nullptr;
  void* mapped = nullptr;
  uint64_t offset = 0;
  explicit operator bool() const { return buffer && mapped; }
};
using Banks = PreparedConstantBanks<Slice>;
struct Arena {
  std::vector<uint8_t> memory = std::vector<uint8_t>(16 * 1024 * 1024);
  uint32_t used = 0, calls = 0;
  bool fail = false;
  Slice Allocate(uint32_t size) {
    ++calls;
    if (fail) return {};
    assert(used + size <= memory.size());
    Slice result{this, memory.data() + used, used};
    used += size;
    return result;
  }
};
void Swap(void* destination, const void* source, size_t size) {
  auto* dst = static_cast<uint8_t*>(destination);
  const auto* src = static_cast<const uint8_t*>(source);
  for (size_t n = 0; n < size; n += 4)
    for (size_t b = 0; b < 4; ++b) dst[n + b] = src[n + 3 - b];
}
Banks::Packet Oracle(const Banks::Packet& guest, bool dof) {
  Banks::Packet result;
  Swap(result.data(), guest.data(), 8192);
  std::memcpy(result.data() + 8192, guest.data() + 8192, 624);
  ApplyDisabledTonemapDof(0x3A47E5DDE66B42C6ull, dof, result.data() + 4096, 4096);
  return result;
}
void Prepare(Banks& banks, const Banks::Packet& guest, bool dof) {
  banks.UpdateFloatBank(0, {guest.data(), 4096}, 0, Swap);
  banks.UpdateFloatBank(1, {guest.data() + 4096, 4096}, dof, Swap);
  ApplyDisabledTonemapDof(0x3A47E5DDE66B42C6ull, dof, banks.Bytes().data() + 4096, 4096);
  banks.UpdateShared({guest.data() + 8192, 624});
}
void Verify(const Banks::Packet& expected, const std::array<Slice, 3>& slices) {
  for (size_t i = 0; i < slices.size(); ++i) {
    assert(slices[i] && slices[i].offset % 256 == 0);
    assert(std::memcmp(slices[i].mapped, expected.data() + Banks::offsets[i], Banks::sizes[i]) == 0);
  }
}
int main() {
  std::array<uint8_t, 4130> left{}, right{};
  for (unsigned shift = 0; shift < 32; ++shift) {
    for (unsigned offset = 0; offset < 4096; ++offset) {
      right[shift + offset] = uint8_t(1u << (offset & 7));
      assert(!EqualConstantBytesSimd(left.data() + shift, right.data() + shift, 4096));
      right[shift + offset] = 0;
    }
    for (unsigned length = 0; length < 256; ++length) {
      assert(EqualConstantBytesSimd(left.data() + shift, right.data() + shift, length));
      if (length) {
        right[shift + length - 1] = 0x80;
        assert(!EqualConstantBytesSimd(left.data() + shift, right.data() + shift, length));
        right[shift + length - 1] = 0;
      }
    }
  }
  Banks banks;
  Banks::Packet guest{};
  Arena arena;
  std::array<Slice, 3> slices{};
  auto allocate = [&](uint32_t n) { return arena.Allocate(n); };
  assert(!banks.ResolveUploads(1, allocate, slices));
  Prepare(banks, guest, false);
  assert(banks.ResolveUploads(1, allocate, slices));
  assert(arena.used == 8960 && banks.PreparedBanks() == 3);
  const auto original = slices;
  Prepare(banks, guest, false);
  assert(banks.ResolveUploads(1, allocate, slices));
  assert(arena.calls == 1 && banks.PreparedBanks() == 3);
  guest[8193] = 17;
  Prepare(banks, guest, false);
  assert(banks.ResolveUploads(1, allocate, slices));
  assert(arena.used == 9728 && slices[0].mapped == original[0].mapped);
  Verify(Banks::Packet{}, original);
  const auto published = slices;
  guest[0] = 99;
  Prepare(banks, guest, false);
  arena.fail = true;
  assert(!banks.ResolveUploads(1, allocate, slices));
  for (size_t i = 0; i < 3; ++i) assert(slices[i].mapped == published[i].mapped);
  arena.fail = false;
  assert(banks.ResolveUploads(1, allocate, slices));
  Verify(Oracle(guest, false), slices);
  for (bool disabled : {true, true, false, false, true, false}) {
    guest[4096 + 192] = 0x3f;
    guest[4096 + 193] = 0x80;
    Prepare(banks, guest, disabled);
    assert(banks.Bytes() == Oracle(guest, disabled));
    assert(banks.ResolveUploads(1, allocate, slices));
    Verify(Oracle(guest, disabled), slices);
  }
  arena.used = 0;
  std::fill(arena.memory.begin(), arena.memory.end(), 0xCD);
  assert(banks.ResolveUploads(2, allocate, slices));
  Verify(Oracle(guest, false), slices);
  assert(arena.used == 8960);
  assert(banks.ResolveUploads(0, allocate, slices));
  assert(banks.ResolveUploads(0, allocate, slices));
  assert(arena.used == 3 * 8960);
  banks.Reset();
  assert(!banks.ResolveUploads(2, allocate, slices));
  Prepare(banks, guest, false);
  assert(banks.ResolveUploads(2, allocate, slices));
  Verify(Oracle(guest, false), slices);
  std::mt19937 random(0x83FB6630);
  std::deque<std::pair<Banks::Packet, std::array<Slice, 3>>> history;
  uint64_t serial = 3;
  bool disabled = false;
  for (unsigned draw = 0; draw < 30000; ++draw) {
    if (draw % 256 == 0) { ++serial; arena.used = 0; history.clear(); }
    if (draw % 4) guest[random() % guest.size()] ^= uint8_t(random());
    if (draw % 37 == 0) disabled = !disabled;
    Prepare(banks, guest, disabled);
    const auto expected = Oracle(guest, disabled);
    assert(banks.Bytes() == expected);
    assert(banks.ResolveUploads(serial, allocate, slices));
    Verify(expected, slices);
    for (const auto& saved : history) Verify(saved.first, saved.second);
    history.emplace_back(expected, slices);
    if (history.size() > 8) history.pop_front();
  }
  PreparedValue<uint64_t, int> prepared;
  banks.Reset();
  Prepare(banks, guest, false);
  banks.SetUploadRegisters(1, 17);
  arena.used = 0;
  assert(banks.ResolveUploads(++serial, allocate, slices));
  assert(arena.used == 256 + 512 + 768);
  const auto small = slices;
  banks.SetUploadRegisters(256, 256);
  assert(banks.ResolveUploads(serial, allocate, slices));
  Verify(Oracle(guest, false), slices);
  assert(slices[0].mapped != small[0].mapped && slices[1].mapped != small[1].mapped);
  assert(slices[2].mapped == small[2].mapped);
  banks.SetUploadRegisters(0, 0);
  guest[0] ^= 1;
  Prepare(banks, guest, false);
  assert(banks.ResolveUploads(serial, allocate, slices));
  const auto short_vertex = slices[0];
  banks.SetUploadRegisters(256, 0);
  assert(banks.ResolveUploads(serial, allocate, slices));
  assert(slices[0].mapped != short_vertex.mapped);
  assert(std::memcmp(slices[0].mapped, banks.Bytes().data(), 4096) == 0);
  int preparations = 0;
  auto build = [&] { return ++preparations; };
  assert(prepared.Resolve(1, build) == 1);
  assert(prepared.Resolve(1, build) == 1);
  assert(prepared.Resolve(2, build) == 2);
  prepared.Reset();
  assert(prepared.Resolve(2, build) == 3);
}
