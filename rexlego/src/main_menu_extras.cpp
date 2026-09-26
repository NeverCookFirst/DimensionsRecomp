// legodimensions - ReXGlue Recompiled Project
//
// See main_menu_extras.h. Everything the hooks rely on was read off the
// recompiled code on 2026-09-26:
//
//   sub_82FBE7A8  main menu AboutToShow. Calls SetEntryVisible(root, code, 0)
//                 for Help (6) and SwitchProfile (8) always, for Quit (5) on
//                 this platform, and for LoadGame (2) when there is no save.
//                 root = [this + 40].
//   sub_82F0D340  SetEntryVisible(root, code, visible): walks the element tree
//                 and writes the flag at +457 of every element whose code
//                 (+36) matches.
//   sub_82F03C40  builds a <text> element from the layout XML and returns it.
//                 The translation attribute is applied inside it as
//                 SetText(element, Localise(key, 0)).
//   sub_82A435C8  Localise(key, 0) -> text for the current language.
//   sub_82EFEFA0  SetText(element, text).
//   sub_82FE4A80  main menu "A pressed": reads the selected element from the
//                 menu page ([0x849F5F40], via sub_82D70708 / sub_82EF9560)
//                 and switches on its code. Help (6) does nothing there, Quit
//                 (5) leaves to the dashboard.
//
// The labels come from the title's own text table so they are translated:
// CREDITS ("Credits") and GEN_QUIT ("Quit Game") are both marked for every
// platform, unlike PCSTRING_QUITWINDOWS that the layout asks for, which only
// exists on PC and would leave the entry blank here.

#include "main_menu_extras.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>

REXCVAR_DEFINE_BOOL(main_menu_extras, true, "UI",
                    "Show Credits and Quit Game in the main menu. Off leaves the menu as the "
                    "Xbox 360 had it; takes effect the next time the main menu is shown.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REX_EXTERN(__imp__sub_82FBE7A8);
REX_EXTERN(__imp__sub_82F03C40);
REX_EXTERN(__imp__sub_82FE4A80);
REX_EXTERN(sub_82F0D340);
REX_EXTERN(sub_82A435C8);
REX_EXTERN(sub_82EFEFA0);
REX_EXTERN(sub_82D70708);
REX_EXTERN(sub_82EF9560);

namespace legodimensions::main_menu {
namespace {

// Entry codes, as the title's name -> code table assigns them.
constexpr uint32_t kCodeQuit = 5;
constexpr uint32_t kCodeHelp = 6;  // shown as Credits
constexpr uint32_t kCodeSwitchLanguage = 7;
// Shown as Quit Game instead of the Quit entry itself: all three share the
// middle-left slot, but the layout's navigation only links SwitchProfile to
// the slot above ("down" from Help goes to xml_switchprofile and nowhere else),
// so with Quit the D-pad could not get from Credits down to it.
constexpr uint32_t kCodeSwitchProfile = 8;

constexpr uint32_t kMenuPageGlobal = 0x849F5F40;

enum Request : int { kNone = 0, kCredits = 1, kQuit = 2 };
std::atomic<int> g_request{kNone};

uint32_t Load32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

// Guest copies of the two text keys; the title's own strings have no
// standalone "CREDITS". Allocated once, never freed - they live as long as
// the game.
uint32_t GuestString(const char* text) {
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->memory()) {
    return 0;
  }
  const uint32_t size = uint32_t(std::strlen(text)) + 1;
  const uint32_t address = ks->memory()->SystemHeapAlloc(size);
  if (address) {
    std::memcpy(ks->memory()->virtual_membase() + address, text, size);
  }
  return address;
}

uint32_t CreditsKey() {
  static const uint32_t address = GuestString("CREDITS");
  return address;
}

uint32_t QuitKey() {
  static const uint32_t address = GuestString("GEN_QUIT");
  return address;
}

// Calls a guest function from inside a hook. Only r3 is ours to hand back, the
// rest of the volatile state the callee may clobber is restored so the code
// around the hook sees what it expects.
template <typename F>
uint32_t CallGuest(F fn, PPCContext& ctx, uint8_t* base, uint32_t r3, uint32_t r4 = 0,
                   uint32_t r5 = 0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  fn(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

}  // namespace
}  // namespace legodimensions::main_menu

using namespace legodimensions::main_menu;

// AboutToShow: let the title hide what it hides, then bring back ours. The
// language entry shares the Quit slot, so it goes; the dashboard sets the
// language on this platform anyway.
REX_HOOK_RAW(sub_82FBE7A8) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_82FBE7A8(ctx, base);
  if (!REXCVAR_GET(main_menu_extras)) {
    return;
  }
  const uint32_t root = Load32(base, self + 40);
  if (!root) {
    return;
  }
  CallGuest(sub_82F0D340, ctx, base, root, kCodeSwitchLanguage, 0);
  CallGuest(sub_82F0D340, ctx, base, root, kCodeQuit, 0);
  CallGuest(sub_82F0D340, ctx, base, root, kCodeHelp, 1);
  CallGuest(sub_82F0D340, ctx, base, root, kCodeSwitchProfile, 1);
}

// Relabel the two entries as they are built.
REX_HOOK_RAW(sub_82F03C40) {
  __imp__sub_82F03C40(ctx, base);
  const uint32_t element = ctx.r3.u32;
  if (!element || !REXCVAR_GET(main_menu_extras)) {
    return;
  }
  const uint32_t code = Load32(base, element + 36);
  uint32_t key = 0;
  if (code == kCodeHelp) {
    key = CreditsKey();
  } else if (code == kCodeSwitchProfile) {
    key = QuitKey();
  }
  if (!key) {
    return;
  }
  const uint32_t text = CallGuest(sub_82A435C8, ctx, base, key, 0);
  if (text) {
    CallGuest(sub_82EFEFA0, ctx, base, element, text);
  }
}

// Selection: ours go to the overlay, everything else to the title.
REX_HOOK_RAW(sub_82FE4A80) {
  const uint32_t page = Load32(base, kMenuPageGlobal);
  if (REXCVAR_GET(main_menu_extras) && page && CallGuest(sub_82D70708, ctx, base, page)) {
    const uint32_t selected = CallGuest(sub_82EF9560, ctx, base, page);
    const uint32_t code = selected ? Load32(base, selected + 36) : 0;
    if (code == kCodeHelp) {
      g_request.store(kCredits);
      return;
    }
    if (code == kCodeSwitchProfile) {
      g_request.store(kQuit);
      return;
    }
  }
  __imp__sub_82FE4A80(ctx, base);
}

namespace legodimensions::main_menu {
namespace {

struct CreditLine {
  const char* name;
  const char* text;
};

// Mirrors the Thanks section of README.md.
constexpr CreditLine kThanks[] = {
    {"Unleashed Recompiled",
     "First and above all. This project exists because of how much I loved what they did."},
    {"ReXGlue", "The toolkit that does the actual recompiling and the runtime this game sits on."},
    {"The LEGO Dimensions Discord",
     "For years of accumulated knowledge about this game, and for encouraging every experiment."},
    {"harrysof", "For the LEGO Toypad app and the Toy Pad protocol work the portal builds on."},
    {"xenia", "Whose research into the Xbox 360 GPU makes the graphics side possible at all."},
    {"connorh315",
     "For BrickVault, the DFLT decompressor, and answering a pile of format questions."},
    {"Everyone who tested", "Every broken build, and every \"that's a good idea\" before it was."},
};

class MenuExtrasDialog final : public rex::ui::ImGuiDialog {
 public:
  MenuExtrasDialog(rex::ui::ImGuiDrawer* drawer, Host host)
      : ImGuiDialog(drawer), host_(std::move(host)) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (state_ == State::kIdle) {
      const int request = g_request.exchange(kNone);
      if (request == kNone) {
        return;
      }
      // The A that picked the entry is still held; start from it so it does
      // not count as a press.
      prev_pad_ = host_.pad_buttons ? host_.pad_buttons() : 0;
      state_ = request == kCredits ? State::kCredits : State::kQuit;
      quit_choice_ = 1;  // "No" first: a stray press should not close the game.
      if (host_.set_paused) {
        host_.set_paused(true);
      }
      return;
    }

    // Buttons as the game's own poll last saw them; the input system is never
    // called from this thread.
    const uint16_t pad = host_.pad_buttons ? host_.pad_buttons() : 0;
    const uint16_t pressed = pad & ~prev_pad_;
    prev_pad_ = pad;

    if (state_ == State::kReleasing) {
      // Resume only once everything is let go, or the button that closed the
      // prompt reaches the menu underneath - B would back out of it.
      const bool keys_down = ImGui::IsKeyDown(ImGuiKey_Enter) ||
                             ImGui::IsKeyDown(ImGuiKey_Escape) ||
                             ImGui::IsKeyDown(ImGuiKey_Space);
      if (!pad && !keys_down) {
        state_ = State::kIdle;
        g_request.store(kNone);
        if (host_.set_paused) {
          host_.set_paused(false);
        }
      }
      return;
    }

    DrawDim(io);
    if (state_ == State::kQuit) {
      DrawQuit(io, pressed);
    } else {
      DrawCredits(io, pressed, pad);
    }
  }

 private:
  enum class State { kIdle, kQuit, kCredits, kReleasing };

  static bool Pressed(uint16_t pressed, uint16_t mask) { return (pressed & mask) != 0; }

  void Close() { state_ = State::kReleasing; }

  static void DrawDim(ImGuiIO& io) {
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize,
                                                  IM_COL32(0, 0, 0, 150));
  }

  static void BeginCentered(ImGuiIO& io, const char* id, ImVec2 size) {
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowFocus();
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);
  }

  static void CenteredText(const char* text) {
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x - width) * 0.5f);
    ImGui::TextUnformatted(text);
  }

  static bool Choice(const char* label, bool selected, ImVec2 size) {
    if (selected) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    const bool clicked = ImGui::Button(label, size);
    if (selected) {
      ImGui::PopStyleColor();
    }
    return clicked;
  }

  void DrawQuit(ImGuiIO& io, uint16_t pressed) {
    if (Pressed(pressed, rex::input::X_INPUT_GAMEPAD_DPAD_LEFT) ||
        ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
      quit_choice_ = 0;
    }
    if (Pressed(pressed, rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT) ||
        ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
      quit_choice_ = 1;
    }
    bool yes = false;
    bool no = Pressed(pressed, rex::input::X_INPUT_GAMEPAD_B) ||
              ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (Pressed(pressed, rex::input::X_INPUT_GAMEPAD_A) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
        ImGui::IsKeyPressed(ImGuiKey_Space)) {
      (quit_choice_ == 0 ? yes : no) = true;
    }

    const float line = ImGui::GetTextLineHeightWithSpacing();
    const float button_w = line * 5.0f;
    BeginCentered(io, "##quit_prompt", ImVec2(line * 20.0f, line * 6.0f));
    ImGui::Dummy(ImVec2(0, line * 0.8f));
    CenteredText("Are you sure you want to close the game?");
    ImGui::Dummy(ImVec2(0, line * 0.8f));
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x - button_w * 2 - line) * 0.5f);
    if (Choice("Yes", quit_choice_ == 0, ImVec2(button_w, line * 1.4f))) {
      yes = true;
    }
    ImGui::SameLine(0, line);
    if (Choice("No", quit_choice_ == 1, ImVec2(button_w, line * 1.4f))) {
      no = true;
    }
    ImGui::End();

    if (yes) {
      REXLOG_INFO("Main menu: Quit Game confirmed, closing.");
      if (host_.quit) {
        host_.quit();
      }
    } else if (no) {
      Close();
    }
  }

  void DrawCredits(ImGuiIO& io, uint16_t pressed, uint16_t held) {
    bool close = Pressed(pressed, rex::input::X_INPUT_GAMEPAD_B) ||
                 Pressed(pressed, rex::input::X_INPUT_GAMEPAD_A) ||
                 ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter);

    const float line = ImGui::GetTextLineHeightWithSpacing();
    const float width = std::min(io.DisplaySize.x - 32.0f, line * 34.0f);
    BeginCentered(io, "##credits",
                  ImVec2(width, std::min(io.DisplaySize.y - 32.0f, line * 26.0f)));
    ImGui::Dummy(ImVec2(0, line * 0.4f));
    CenteredText("DIMENSIONS RECOMPILED");
    ImGui::Dummy(ImVec2(0, line * 0.3f));
    CenteredText("Made by NeverCookFirst");
    ImGui::Dummy(ImVec2(0, line * 0.5f));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, line * 0.3f));
    CenteredText("Thanks");
    ImGui::Dummy(ImVec2(0, line * 0.3f));

    const float child_h = ImGui::GetContentRegionAvail().y - line * 2.2f;
    ImGui::BeginChild("##thanks", ImVec2(0, child_h));
    // D-pad scrolls, for anyone on a controller.
    if (held & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) {
      ImGui::SetScrollY(ImGui::GetScrollY() + line * 0.5f);
    } else if (held & rex::input::X_INPUT_GAMEPAD_DPAD_UP) {
      ImGui::SetScrollY(ImGui::GetScrollY() - line * 0.5f);
    }
    for (const CreditLine& credit : kThanks) {
      ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive), "%s", credit.name);
      ImGui::Indent();
      ImGui::TextWrapped("%s", credit.text);
      ImGui::Unindent();
      ImGui::Dummy(ImVec2(0, line * 0.25f));
    }
    ImGui::EndChild();

    const float button_w = line * 6.0f;
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x - button_w) * 0.5f);
    if (Choice("Close", true, ImVec2(button_w, line * 1.4f))) {
      close = true;
    }
    ImGui::End();

    if (close) {
      Close();
    }
  }

  Host host_;
  State state_ = State::kIdle;
  uint16_t prev_pad_ = 0;
  int quit_choice_ = 1;
};

}  // namespace

std::unique_ptr<rex::ui::ImGuiDialog> CreateOverlay(rex::ui::ImGuiDrawer* drawer, Host host) {
  return std::make_unique<MenuExtrasDialog>(drawer, std::move(host));
}

}  // namespace legodimensions::main_menu
