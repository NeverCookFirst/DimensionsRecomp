// legodimensions - ReXGlue Recompiled Project
//
// See cheat_engine.h. Values in guest memory are big-endian, so every load and
// store here byte-swaps; that is the whole reason an external tool like Cheat
// Engine is awkward on this game and this menu exists.

#include "cheat_engine.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <rex/logging.h>
#include <rex/system/xmemory.h>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace legodimensions::cheats {
namespace {

// Console RAM. The physical membase covers exactly this much.
constexpr uint32_t kPhysicalSize = 0x20000000;
constexpr double kFloatEpsilon = 1e-3;

// Scan kinds handed to the scan thread.
constexpr int kScanNew = 0;
constexpr int kScanExact = 1;
constexpr int kScanCompare = 2;

double LoadValue(const uint8_t* p, CheatEngine::ValueType type) {
  switch (type) {
    case CheatEngine::ValueType::kUInt8:
      return double(p[0]);
    case CheatEngine::ValueType::kUInt16:
      return double((uint32_t(p[0]) << 8) | p[1]);
    case CheatEngine::ValueType::kUInt32:
      return double((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]);
    case CheatEngine::ValueType::kFloat: {
      uint32_t bits =
          (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
      float f;
      std::memcpy(&f, &bits, sizeof(f));
      return double(f);
    }
  }
  return 0.0;
}

void StoreValue(uint8_t* p, CheatEngine::ValueType type, double value) {
  switch (type) {
    case CheatEngine::ValueType::kUInt8:
      p[0] = uint8_t(int64_t(value));
      break;
    case CheatEngine::ValueType::kUInt16: {
      uint16_t v = uint16_t(int64_t(value));
      p[0] = uint8_t(v >> 8);
      p[1] = uint8_t(v);
      break;
    }
    case CheatEngine::ValueType::kUInt32: {
      uint32_t v = uint32_t(int64_t(value));
      p[0] = uint8_t(v >> 24);
      p[1] = uint8_t(v >> 16);
      p[2] = uint8_t(v >> 8);
      p[3] = uint8_t(v);
      break;
    }
    case CheatEngine::ValueType::kFloat: {
      float f = float(value);
      uint32_t v;
      std::memcpy(&v, &f, sizeof(v));
      p[0] = uint8_t(v >> 24);
      p[1] = uint8_t(v >> 16);
      p[2] = uint8_t(v >> 8);
      p[3] = uint8_t(v);
      break;
    }
  }
}

bool ValuesEqual(double a, double b, CheatEngine::ValueType type) {
  if (type == CheatEngine::ValueType::kFloat) {
    return std::fabs(a - b) < kFloatEpsilon;
  }
  return a == b;
}

}  // namespace

const char* CheatEngine::ValueTypeName(ValueType type) {
  switch (type) {
    case ValueType::kUInt8:
      return "8-bit";
    case ValueType::kUInt16:
      return "16-bit";
    case ValueType::kUInt32:
      return "32-bit";
    case ValueType::kFloat:
      return "float";
  }
  return "?";
}

size_t CheatEngine::ValueTypeSize(ValueType type) {
  switch (type) {
    case ValueType::kUInt8:
      return 1;
    case ValueType::kUInt16:
      return 2;
    default:
      return 4;
  }
}

CheatEngine::CheatEngine(rex::memory::Memory* memory, std::filesystem::path save_path)
    : memory_(memory), save_path_(std::move(save_path)) {
  Load();
  freeze_thread_ = std::thread(&CheatEngine::FreezeMain, this);
}

CheatEngine::~CheatEngine() {
  Join();
  freeze_running_ = false;
  if (freeze_thread_.joinable()) {
    freeze_thread_.join();
  }
  Save();
}

// ---------------------------------------------------------------------------
// Host memory queries.

std::vector<CheatEngine::Region> CheatEngine::QueryCommittedRegions() const {
  std::vector<Region> regions;
#if defined(_WIN32)
  const uint8_t* base = memory_->physical_membase();
  uint32_t offset = 0;
  while (offset < kPhysicalSize) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(base + offset, &info, sizeof(info))) {
      break;
    }
    size_t region_size = std::min(size_t(info.RegionSize), size_t(kPhysicalSize - offset));
    if (!region_size) {
      break;
    }
    const bool readable =
        info.State == MEM_COMMIT && !(info.Protect & PAGE_GUARD) &&
        (info.Protect &
         (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0;
    if (readable) {
      if (!regions.empty() && regions.back().start + regions.back().size == offset) {
        regions.back().size += uint32_t(region_size);
      } else {
        regions.push_back({offset, uint32_t(region_size)});
      }
    }
    offset += uint32_t(region_size);
  }
#else
  regions.push_back({0, kPhysicalSize});
#endif
  return regions;
}

bool CheatEngine::IsReadableAddress(uint32_t phys_addr, size_t size) const {
  if (uint64_t(phys_addr) + size > kPhysicalSize) {
    return false;
  }
#if defined(_WIN32)
  MEMORY_BASIC_INFORMATION info;
  if (!VirtualQuery(memory_->physical_membase() + phys_addr, &info, sizeof(info))) {
    return false;
  }
  return info.State == MEM_COMMIT && !(info.Protect & PAGE_GUARD) &&
         (info.Protect &
          (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0;
#else
  return true;
#endif
}

bool CheatEngine::IsWritableAddress(uint32_t phys_addr, size_t size) const {
  if (uint64_t(phys_addr) + size > kPhysicalSize) {
    return false;
  }
#if defined(_WIN32)
  MEMORY_BASIC_INFORMATION info;
  if (!VirtualQuery(memory_->physical_membase() + phys_addr, &info, sizeof(info))) {
    return false;
  }
  return info.State == MEM_COMMIT && !(info.Protect & PAGE_GUARD) &&
         (info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) != 0;
#else
  return true;
#endif
}

// ---------------------------------------------------------------------------
// Candidate bitmap.
//
// One bit per aligned slot of the current value type: 16 MiB for a 32-bit scan,
// 64 MiB for an 8-bit one. More than an address list costs for a narrow result
// set, but it is what lets a scan start from "every address is a candidate"
// with no cap, which is exactly where the xenia version fell over.

void CheatEngine::ResetMask() {
  const uint64_t slots = kPhysicalSize / ValueTypeSize(scan_type_);
  mask_.assign(size_t((slots + 63) / 64), 0);
}

bool CheatEngine::MaskTest(uint64_t slot) const {
  return ((mask_[size_t(slot >> 6)] >> (slot & 63)) & 1) != 0;
}

void CheatEngine::MaskSet(uint64_t slot, bool on) {
  uint64_t& word = mask_[size_t(slot >> 6)];
  const uint64_t bit = uint64_t(1) << (slot & 63);
  if (on) {
    word |= bit;
  } else {
    word &= ~bit;
  }
}

void CheatEngine::TakeSnapshot() {
  if (snapshot_.size() != kPhysicalSize) {
    snapshot_.assign(kPhysicalSize, 0);
  }
  const uint8_t* base = memory_->physical_membase();
  for (const Region& region : QueryCommittedRegions()) {
    std::memcpy(snapshot_.data() + region.start, base + region.start, region.size);
  }
  snapshot_valid_ = true;
}

// ---------------------------------------------------------------------------
// Scanning.

void CheatEngine::Join() {
  if (scan_thread_.joinable()) {
    scan_thread_.join();
  }
}

void CheatEngine::StartNewScan(ValueType type) {
  if (scanning_) {
    return;
  }
  Join();
  scan_type_ = type;
  scanning_ = true;
  scan_thread_ = std::thread([this] { RunScan(kScanNew, 0.0, CompareOp::kChanged); });
}

void CheatEngine::StartScanExact(double value) {
  if (scanning_) {
    return;
  }
  Join();
  scanning_ = true;
  scan_thread_ = std::thread([this, value] { RunScan(kScanExact, value, CompareOp::kChanged); });
}

void CheatEngine::StartScanCompare(CompareOp op) {
  if (scanning_ || !snapshot_valid_) {
    return;
  }
  Join();
  scanning_ = true;
  scan_thread_ = std::thread([this, op] { RunScan(kScanCompare, 0.0, op); });
}

void CheatEngine::RunScan(int kind, double value, CompareOp op) {
  const uint32_t value_size = uint32_t(ValueTypeSize(scan_type_));
  const uint8_t* base = memory_->physical_membase();

  if (kind == kScanNew) {
    ResetMask();
    unfiltered_ = true;
    result_count_ = 0;
    TakeSnapshot();
    scanning_ = false;
    return;
  }

  // A value scan run without a snapshot behaves like a first scan.
  if (!snapshot_valid_ || mask_.empty()) {
    ResetMask();
    unfiltered_ = true;
    TakeSnapshot();
  }

  auto keep = [&](uint32_t addr) {
    const double now = LoadValue(base + addr, scan_type_);
    if (kind == kScanExact) {
      return ValuesEqual(now, value, scan_type_);
    }
    const double before = LoadValue(snapshot_.data() + addr, scan_type_);
    switch (op) {
      case CompareOp::kIncreased:
        return now > before;
      case CompareOp::kDecreased:
        return now < before;
      case CompareOp::kChanged:
        return !ValuesEqual(now, before, scan_type_);
      case CompareOp::kUnchanged:
        return ValuesEqual(now, before, scan_type_);
    }
    return false;
  };

  uint64_t kept = 0;
  if (unfiltered_) {
    // Nothing filtered yet: every readable, aligned slot is a candidate, so
    // walk the committed regions and set the bits that survive.
    for (const Region& region : QueryCommittedRegions()) {
      uint32_t addr = (region.start + value_size - 1) & ~(value_size - 1);
      const uint32_t end = region.start + region.size;
      for (; uint64_t(addr) + value_size <= end; addr += value_size) {
        if (keep(addr)) {
          MaskSet(addr / value_size, true);
          ++kept;
        }
      }
    }
    unfiltered_ = false;
  } else {
    // Walk only the bits already set. Skipping empty words keeps a narrowed
    // scan fast however big the address space is.
    for (size_t word_index = 0; word_index < mask_.size(); ++word_index) {
      uint64_t word = mask_[word_index];
      while (word) {
        const int bit = std::countr_zero(word);
        word &= word - 1;
        const uint64_t slot = (uint64_t(word_index) << 6) + uint64_t(bit);
        const uint32_t addr = uint32_t(slot * value_size);
        if (keep(addr)) {
          ++kept;
        } else {
          MaskSet(slot, false);
        }
      }
    }
  }

  result_count_ = kept;
  TakeSnapshot();
  scanning_ = false;
}

std::vector<uint32_t> CheatEngine::TakeResults(size_t limit) const {
  std::vector<uint32_t> out;
  if (scanning_ || unfiltered_ || mask_.empty()) {
    return out;
  }
  const uint32_t value_size = uint32_t(ValueTypeSize(scan_type_));
  out.reserve(std::min<size_t>(limit, size_t(result_count_.load())));
  for (size_t word_index = 0; word_index < mask_.size() && out.size() < limit; ++word_index) {
    uint64_t word = mask_[word_index];
    while (word && out.size() < limit) {
      const int bit = std::countr_zero(word);
      word &= word - 1;
      out.push_back(uint32_t(((uint64_t(word_index) << 6) + uint64_t(bit)) * value_size));
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Direct access.

double CheatEngine::ReadValue(uint32_t phys_addr, ValueType type) const {
  if (!IsReadableAddress(phys_addr, ValueTypeSize(type))) {
    return 0.0;
  }
  return LoadValue(memory_->physical_membase() + phys_addr, type);
}

void CheatEngine::WriteValue(uint32_t phys_addr, ValueType type, double value) {
  if (!IsWritableAddress(phys_addr, ValueTypeSize(type))) {
    return;
  }
  StoreValue(memory_->physical_membase() + phys_addr, type, value);
}

// ---------------------------------------------------------------------------
// Freezing and named cheats.

void CheatEngine::SetFrozen(uint32_t phys_addr, ValueType type, double value, bool on) {
  std::lock_guard<std::mutex> lock(apply_mutex_);
  auto it = std::find_if(frozen_.begin(), frozen_.end(),
                         [&](const CheatWrite& w) { return w.phys_addr == phys_addr; });
  if (on) {
    if (it != frozen_.end()) {
      it->type = type;
      it->value = value;
    } else {
      frozen_.push_back({phys_addr, type, value});
    }
  } else if (it != frozen_.end()) {
    frozen_.erase(it);
  }
}

bool CheatEngine::IsFrozen(uint32_t phys_addr) const {
  std::lock_guard<std::mutex> lock(apply_mutex_);
  return std::any_of(frozen_.begin(), frozen_.end(),
                     [&](const CheatWrite& w) { return w.phys_addr == phys_addr; });
}

void CheatEngine::ClearFrozen() {
  std::lock_guard<std::mutex> lock(apply_mutex_);
  frozen_.clear();
}

size_t CheatEngine::frozen_count() const {
  std::lock_guard<std::mutex> lock(apply_mutex_);
  return frozen_.size();
}

std::vector<CheatEngine::Cheat> CheatEngine::cheats() const {
  std::lock_guard<std::mutex> lock(apply_mutex_);
  return cheats_;
}

void CheatEngine::AddCheat(const std::string& name, uint32_t phys_addr, ValueType type,
                           double value) {
  std::lock_guard<std::mutex> lock(apply_mutex_);
  Cheat cheat;
  cheat.name = name.empty() ? "Unnamed cheat" : name;
  cheat.enabled = true;
  cheat.writes.push_back({phys_addr, type, value});
  cheats_.push_back(std::move(cheat));
}

void CheatEngine::RemoveCheat(size_t index) {
  std::lock_guard<std::mutex> lock(apply_mutex_);
  if (index < cheats_.size()) {
    cheats_.erase(cheats_.begin() + index);
  }
}

void CheatEngine::SetCheatEnabled(size_t index, bool enabled) {
  std::lock_guard<std::mutex> lock(apply_mutex_);
  if (index < cheats_.size()) {
    cheats_[index].enabled = enabled;
  }
}

void CheatEngine::FreezeMain() {
  while (freeze_running_) {
    {
      std::lock_guard<std::mutex> lock(apply_mutex_);
      auto apply = [&](const CheatWrite& write) {
        if (IsWritableAddress(write.phys_addr, ValueTypeSize(write.type))) {
          StoreValue(memory_->physical_membase() + write.phys_addr, write.type, write.value);
        }
      };
      for (const CheatWrite& write : frozen_) {
        apply(write);
      }
      for (const Cheat& cheat : cheats_) {
        if (!cheat.enabled) {
          continue;
        }
        for (const CheatWrite& write : cheat.writes) {
          apply(write);
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

// Persistence, one cheat per line:
//   name|enabled|addr:type:value[;addr:type:value...]
// addr is a hex guest physical address, type the ValueType integer, e.g.
//   Infinite Hearts|1|1A2B3C40:3:4
void CheatEngine::Save() {
  std::lock_guard<std::mutex> lock(apply_mutex_);
  std::ofstream file(save_path_, std::ofstream::trunc);
  if (!file.is_open()) {
    REXLOG_WARN("Cheats: failed to save to {}", save_path_.string());
    return;
  }
  for (const Cheat& cheat : cheats_) {
    file << cheat.name << '|' << (cheat.enabled ? 1 : 0) << '|';
    for (size_t i = 0; i < cheat.writes.size(); ++i) {
      const CheatWrite& write = cheat.writes[i];
      if (i) {
        file << ';';
      }
      char addr_buffer[16];
      std::snprintf(addr_buffer, sizeof(addr_buffer), "%08X", write.phys_addr);
      file << addr_buffer << ':' << int(write.type) << ':' << write.value;
    }
    file << '\n';
  }
}

void CheatEngine::Load() {
  std::lock_guard<std::mutex> lock(apply_mutex_);
  cheats_.clear();
  std::ifstream file(save_path_);
  if (!file.is_open()) {
    return;
  }
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) {
      continue;
    }
    const size_t first_sep = line.find('|');
    const size_t second_sep =
        first_sep == std::string::npos ? std::string::npos : line.find('|', first_sep + 1);
    if (second_sep == std::string::npos) {
      continue;
    }
    Cheat cheat;
    cheat.name = line.substr(0, first_sep);
    cheat.enabled = line.substr(first_sep + 1, second_sep - first_sep - 1) == "1";
    std::stringstream writes_stream(line.substr(second_sep + 1));
    std::string write_text;
    while (std::getline(writes_stream, write_text, ';')) {
      unsigned int addr = 0;
      int type = 0;
      double value = 0.0;
      if (std::sscanf(write_text.c_str(), "%x:%d:%lf", &addr, &type, &value) == 3 && type >= 0 &&
          type <= int(ValueType::kFloat)) {
        cheat.writes.push_back({uint32_t(addr), ValueType(type), value});
      }
    }
    if (!cheat.writes.empty()) {
      cheats_.push_back(std::move(cheat));
    }
  }
}

}  // namespace legodimensions::cheats
