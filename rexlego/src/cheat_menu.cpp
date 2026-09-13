// legodimensions - ReXGlue Recompiled Project
//
// See cheat_menu.h. Ported from the cheat overlay in the xenia toypad fork.

#include "cheat_menu.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>

#include "cheat_engine.h"

namespace legodimensions::cheats {
namespace {

// At most this many addresses are listed. A scan still this wide is not one
// you pick an address out of by eye anyway - narrow it further.
constexpr size_t kMaxListed = 200;

// The engine outlives the dialog: a hunt takes several passes with the menu
// closed in between so the game can be played, and freezes have to keep being
// applied while it is closed.
std::unique_ptr<CheatEngine> g_engine;

CheatEngine* Engine() {
  if (!g_engine) {
    auto* ks = rex::system::kernel_state();
    if (!ks || !ks->memory()) {
      return nullptr;
    }
    g_engine =
        std::make_unique<CheatEngine>(ks->memory(), std::filesystem::path("cheats_5752084B.txt"));
  }
  return g_engine.get();
}

class CheatMenuDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit CheatMenuDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

  void OnDraw(ImGuiIO& io) override {
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 60.0f), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(470.0f, 560.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.92f);

    if (!ImGui::Begin("Cheats##rex", nullptr,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_HorizontalScrollbar)) {
      ImGui::End();
      return;
    }

    CheatEngine* engine = Engine();
    if (!engine) {
      ImGui::TextUnformatted("The game is not running yet.");
      ImGui::End();
      return;
    }

    DrawSavedCheats(engine);
    DrawScanner(engine);
    DrawResults(engine);
    DrawSelection(engine);

    if (engine->frozen_count()) {
      ImGui::Separator();
      ImGui::Text("%u frozen value(s)", uint32_t(engine->frozen_count()));
      ImGui::SameLine();
      if (ImGui::SmallButton("Unfreeze all")) {
        engine->ClearFrozen();
      }
    }

    ImGui::End();
  }

 private:
  void DrawSavedCheats(CheatEngine* engine) {
    ImGui::TextUnformatted("Saved cheats");
    ImGui::Separator();
    const std::vector<CheatEngine::Cheat> cheats = engine->cheats();
    if (cheats.empty()) {
      ImGui::TextDisabled("None yet. Find a value below, then \"Save as cheat\".");
    }
    int remove_index = -1;
    for (int i = 0; i < int(cheats.size()); ++i) {
      ImGui::PushID(i);
      bool enabled = cheats[size_t(i)].enabled;
      if (ImGui::Checkbox(cheats[size_t(i)].name.c_str(), &enabled)) {
        engine->SetCheatEnabled(size_t(i), enabled);
        engine->Save();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("X")) {
        remove_index = i;
      }
      ImGui::PopID();
    }
    if (remove_index >= 0) {
      engine->RemoveCheat(size_t(remove_index));
      engine->Save();
    }
  }

  void DrawScanner(CheatEngine* engine) {
    ImGui::Spacing();
    ImGui::TextUnformatted("Memory scanner");
    ImGui::Separator();

    const bool busy = engine->scanning();
    ImGui::BeginDisabled(busy);
    ImGui::Combo("Value type", &scan_type_index_,
                 "8-bit\0"
                 "16-bit\0"
                 "32-bit\0"
                 "float\0");
    ImGui::InputText("Value", scan_value_text_, sizeof(scan_value_text_));
    const double scan_value = std::strtod(scan_value_text_, nullptr);
    const auto selected_type = CheatEngine::ValueType(scan_type_index_);

    if (ImGui::Button("First scan")) {
      engine->StartNewScan(selected_type);
      pending_exact_ = scan_value;
      have_pending_exact_ = true;
      selected_address_ = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Next scan =")) {
      engine->StartScanExact(scan_value);
      selected_address_ = -1;
    }
    ImGui::SameLine();
    if (ImGui::Button("Unknown value")) {
      engine->StartNewScan(selected_type);
      selected_address_ = -1;
    }

    if (engine->has_snapshot()) {
      if (ImGui::Button("Increased")) {
        engine->StartScanCompare(CheatEngine::CompareOp::kIncreased);
        selected_address_ = -1;
      }
      ImGui::SameLine();
      if (ImGui::Button("Decreased")) {
        engine->StartScanCompare(CheatEngine::CompareOp::kDecreased);
        selected_address_ = -1;
      }
      ImGui::SameLine();
      if (ImGui::Button("Changed")) {
        engine->StartScanCompare(CheatEngine::CompareOp::kChanged);
        selected_address_ = -1;
      }
      ImGui::SameLine();
      if (ImGui::Button("Unchanged")) {
        engine->StartScanCompare(CheatEngine::CompareOp::kUnchanged);
        selected_address_ = -1;
      }
    }
    ImGui::EndDisabled();

    // "First scan" needs a fresh snapshot before the value filter, and the
    // snapshot runs on the scan thread, so the filter is queued for a later
    // frame rather than racing it.
    if (have_pending_exact_ && !engine->scanning()) {
      have_pending_exact_ = false;
      engine->StartScanExact(pending_exact_);
    }

    if (engine->scanning()) {
      ImGui::TextDisabled("Scanning...");
    } else if (engine->unfiltered()) {
      ImGui::TextDisabled("Snapshot taken. Change the value in game, then filter.");
    } else if (engine->has_snapshot()) {
      ImGui::Text("Results: %llu", static_cast<unsigned long long>(engine->result_count()));
    }
  }

  void DrawResults(CheatEngine* engine) {
    results_ = engine->TakeResults(kMaxListed);
    if (results_.empty()) {
      return;
    }
    ImGui::BeginChild("##scan_results", ImVec2(0.0f, 150.0f), true);
    for (int i = 0; i < int(results_.size()); ++i) {
      const uint32_t address = results_[size_t(i)];
      const double current = engine->ReadValue(address, engine->scan_type());
      char label[64];
      std::snprintf(label, sizeof(label), "%08X = %.6g%s", address, current,
                    engine->IsFrozen(address) ? "  [frozen]" : "");
      if (ImGui::Selectable(label, selected_address_ == int64_t(address))) {
        selected_address_ = int64_t(address);
        std::snprintf(edit_value_text_, sizeof(edit_value_text_), "%g", current);
      }
    }
    if (engine->result_count() > results_.size()) {
      ImGui::TextDisabled(
          "...and %llu more",
          static_cast<unsigned long long>(engine->result_count() - results_.size()));
    }
    ImGui::EndChild();
  }

  void DrawSelection(CheatEngine* engine) {
    if (selected_address_ < 0) {
      return;
    }
    const auto address = uint32_t(selected_address_);
    ImGui::Text("Selected: %08X (now %.6g)", address,
                engine->ReadValue(address, engine->scan_type()));
    ImGui::InputText("Set to", edit_value_text_, sizeof(edit_value_text_));
    const double edit_value = std::strtod(edit_value_text_, nullptr);
    if (ImGui::Button("Write once")) {
      engine->WriteValue(address, engine->scan_type(), edit_value);
    }
    ImGui::SameLine();
    const bool frozen = engine->IsFrozen(address);
    if (ImGui::Button(frozen ? "Unfreeze" : "Freeze")) {
      engine->SetFrozen(address, engine->scan_type(), edit_value, !frozen);
    }
    ImGui::InputText("Cheat name", cheat_name_text_, sizeof(cheat_name_text_));
    ImGui::SameLine();
    if (ImGui::Button("Save as cheat")) {
      engine->AddCheat(cheat_name_text_, address, engine->scan_type(), edit_value);
      engine->Save();
      cheat_name_text_[0] = '\0';
    }
  }

  int scan_type_index_ = int(CheatEngine::ValueType::kUInt32);
  char scan_value_text_[32] = "0";
  char edit_value_text_[32] = "0";
  char cheat_name_text_[64] = "";
  // The address itself, not its row: the list is rebuilt every frame and rows
  // shift under the selection as a scan narrows.
  int64_t selected_address_ = -1;
  std::vector<uint32_t> results_;
  double pending_exact_ = 0.0;
  bool have_pending_exact_ = false;
};

}  // namespace

std::unique_ptr<rex::ui::ImGuiDialog> CreateMenu(rex::ui::ImGuiDrawer* drawer) {
  return std::make_unique<CheatMenuDialog>(drawer);
}

void Shutdown() {
  g_engine.reset();
}

}  // namespace legodimensions::cheats
