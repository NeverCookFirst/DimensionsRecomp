// legodimensions - ReXGlue Recompiled Project
//
// Memory scanner and value freezer behind the DEL overlay.
//
// Ported from the cheat menu in the xenia toypad fork, with the two things
// that made that one unusable fixed: candidates are a bitmap rather than a
// capped address list (so an unknown-value scan can start from "all memory"
// and actually narrow down), and scans run on their own thread so the game
// does not stall for a second or two on every click.
//
// Every address here is a guest PHYSICAL address: all 512 MiB of console RAM
// is reachable through the physical membase whatever virtual window the game
// happens to use for it.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace legodimensions::cheats {

class CheatEngine {
 public:
  enum class ValueType : int {
    kUInt8 = 0,
    kUInt16 = 1,
    kUInt32 = 2,
    kFloat = 3,
  };

  enum class CompareOp : int {
    kIncreased = 0,
    kDecreased = 1,
    kChanged = 2,
    kUnchanged = 3,
  };

  struct CheatWrite {
    uint32_t phys_addr;
    ValueType type;
    double value;
  };

  struct Cheat {
    std::string name;
    bool enabled = false;
    std::vector<CheatWrite> writes;
  };

  static const char* ValueTypeName(ValueType type);
  static size_t ValueTypeSize(ValueType type);

  CheatEngine(rex::memory::Memory* memory, std::filesystem::path save_path);
  ~CheatEngine();

  // --- Scanning ---
  // Each of these hands the work to the scan thread and returns at once; the
  // UI polls scanning() and draws the previous results meanwhile.
  //
  // Drops all filtering and snapshots memory. Every address is a candidate
  // after this, which is what makes an unknown-value hunt possible.
  void StartNewScan(ValueType type);
  // Keeps candidates currently equal to |value|. Usable as a first scan.
  void StartScanExact(double value);
  // Keeps candidates that moved the expected way against the last snapshot.
  void StartScanCompare(CompareOp op);

  bool scanning() const { return scanning_; }
  bool has_snapshot() const { return snapshot_valid_; }
  bool unfiltered() const { return unfiltered_; }
  ValueType scan_type() const { return scan_type_; }
  uint64_t result_count() const { return result_count_; }
  // First |limit| matching addresses. Cheap enough to call per frame once the
  // count is small; returns nothing while a scan is running.
  std::vector<uint32_t> TakeResults(size_t limit) const;

  // --- Direct access ---
  double ReadValue(uint32_t phys_addr, ValueType type) const;
  void WriteValue(uint32_t phys_addr, ValueType type, double value);

  // --- Freezing (re-applied ~20x a second by a worker thread) ---
  void SetFrozen(uint32_t phys_addr, ValueType type, double value, bool on);
  bool IsFrozen(uint32_t phys_addr) const;
  void ClearFrozen();
  size_t frozen_count() const;

  // --- Named cheats (persisted next to the executable) ---
  std::vector<Cheat> cheats() const;
  void AddCheat(const std::string& name, uint32_t phys_addr, ValueType type, double value);
  void RemoveCheat(size_t index);
  void SetCheatEnabled(size_t index, bool enabled);
  void Save();
  void Load();

 private:
  struct Region {
    uint32_t start;
    uint32_t size;
  };

  std::vector<Region> QueryCommittedRegions() const;
  void TakeSnapshot();
  bool IsWritableAddress(uint32_t phys_addr, size_t size) const;
  bool IsReadableAddress(uint32_t phys_addr, size_t size) const;

  // Candidate bitmap, one bit per |scan_type_|-sized aligned slot.
  void ResetMask();
  bool MaskTest(uint64_t slot) const;
  void MaskSet(uint64_t slot, bool on);

  void RunScan(int kind, double value, CompareOp op);
  void Join();
  void FreezeMain();

  rex::memory::Memory* memory_;
  std::filesystem::path save_path_;

  // Scan state. Only the scan thread writes it, and only while scanning_ is
  // true; the UI thread reads it once the flag clears.
  ValueType scan_type_ = ValueType::kUInt32;
  bool snapshot_valid_ = false;
  bool unfiltered_ = true;
  std::vector<uint8_t> snapshot_;
  std::vector<uint64_t> mask_;
  std::atomic<uint64_t> result_count_{0};
  std::atomic<bool> scanning_{false};
  std::thread scan_thread_;

  // Freeze/cheat state, shared with the freeze thread.
  mutable std::mutex apply_mutex_;
  std::vector<CheatWrite> frozen_;
  std::vector<Cheat> cheats_;

  std::atomic<bool> freeze_running_{true};
  std::thread freeze_thread_;
};

}  // namespace legodimensions::cheats
