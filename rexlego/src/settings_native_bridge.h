#pragma once

#include "native_frontend_binding.h"
#include "native_frontend_component.h"
#include "native_frontend_layout.h"
#include "settings_frontend.h"

#include <rex/ppc/func.h>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace legodimensions::settings {
uint32_t NativeMenuInputEvent(uint32_t forced, uint32_t directions, uint32_t buttons) noexcept;

// The production adapter supplies actual TU23 functions and the SDK's guest
// heap/thunk allocator. CPU tests can exercise this same bridge without a game
// entry point or a graphics device; mocked boundaries must be reported.
struct NativeBridgeApi {
  std::function<uint32_t(uint32_t)> allocate;
  std::function<uint32_t(PPCFunc*)> thunk;
  std::function<std::shared_ptr<QueuedSettings>()> settings;
  QueuedSettings::Now now = [] { return Clock::now(); };
  std::function<void()> quit;
  std::function<void(std::string)> trace;
  std::function<bool(uint32_t, void*, uint32_t)> read_metadata;
  PPCFunc *load_layout{}, *find_page{}, *find_widget{}, *construct_component{},
      *add_component{}, *find_component{}, *set_text{}, *page_event{},
      *selected_widget{}, *configure_popup{}, *reset_popup{}, *publish_popup{}, *request_page{},
      *set_visible{}, *refresh_list{};
  PPCFunc* construct_menu{};
  PPCFunc* select_widget{};
  PPCFunc* play_sound{};
};

// One bridge per title instance. Guest storage/thunks are retained for the
// title lifetime, including after a component's native destructor callback.
// Initialize and all event/frame calls belong to the guest frontend thread.
class NativeSettingsBridge {
 public:
  explicit NativeSettingsBridge(NativeBridgeApi api);
  ~NativeSettingsBridge();
  NativeSettingsBridge(const NativeSettingsBridge&) = delete;
  NativeSettingsBridge& operator=(const NativeSettingsBridge&) = delete;

  bool Initialize(PPCContext& ctx, uint8_t* base, uint32_t manager, bool recreated = false);
  bool ready(uint8_t* base, uint32_t manager) const;
  void RevealOptions(PPCContext& ctx, uint8_t* base, uint32_t options);
  void OptionsEntering(PPCContext& ctx, uint8_t* base, uint32_t options);
  bool DrawPauseEntries(PPCContext& ctx, uint8_t* base, uint32_t menu, PPCFunc* draw_row);
  bool RenderPauseOptions(PPCContext& ctx, uint8_t* base, uint32_t menu, PPCFunc* draw);
  bool UpdatePauseOptions(PPCContext& ctx, uint8_t* base, uint32_t menu, PPCFunc* original);
  bool PauseOverlayActive(uint8_t* base, uint32_t menu) const;
  bool ShowMenuExtra(PPCContext& ctx, uint8_t* base, bool quit);
  uint32_t CompleteMenu(PPCContext& ctx, uint8_t* base, uint32_t xml_node, uint32_t parent);
  bool Input(PPCContext& ctx, uint8_t* base, uint32_t ui, uint32_t packet, uint32_t directions, uint32_t layer = 2);
  uint32_t LookupLiteral(uint8_t* base, uint32_t key, uint32_t* canonical_key = nullptr);
  void BeforeFrame(PPCContext& ctx, uint8_t* base, uint32_t manager);
  void AfterFrame(PPCContext& ctx, uint8_t* base, uint32_t manager);
  const std::string& error() const { return error_; }

 private:
  struct Buffer { uint32_t address{}, capacity{}; };
  struct Page {
    FrontendPage id{};
    FrontendLayout layout;
    uint32_t name{}, root{}, component{}, title{}, title_name{}, menu_name{}, root_vtable{}, root_name{};
    bool retired = false;
    size_t selected_row = 0;
    std::vector<uint32_t> rows, values;
    std::vector<uint32_t> row_names, value_names;
  };
  static NativeSettingsBridge* active_;
  static void Destroy(PPCContext&, uint8_t*);
  static void Event(PPCContext&, uint8_t*);
  static void GetResult(PPCContext&, uint8_t*);
  static void SetResult(PPCContext&, uint8_t*);
  static void AllowSwitch(PPCContext&, uint8_t*);
  static void Accept(PPCContext&, uint8_t*);
  static void Back(PPCContext&, uint8_t*);

  uint32_t Allocate(uint8_t* base, uint32_t bytes);
  uint32_t String(uint8_t* base, std::string_view text);
  bool Fill(uint8_t* base, Buffer& buffer, std::string_view text);
  bool Text(PPCContext& ctx, uint8_t* base, uint32_t widget, std::string_view text);
  void Tint(uint8_t* base, uint32_t widget, bool enabled, bool selected, bool bright);
  uint32_t Literal(uint8_t* base, std::string key, std::string_view value);
  bool BuildPage(PPCContext& ctx, uint8_t* base, Page& page,
                 const std::array<uint32_t, 5>& callbacks);
  bool ResolveControls(PPCContext& ctx, uint8_t* base, Page& page);
  bool ReusePage(PPCContext& ctx, uint8_t* base, Page& page);
  bool LivePage(uint8_t* base, const Page& page) const;
  bool RegisteredPages() const;
  bool RetireDeadPages(uint8_t* base);
  void SuspendRequest(uint8_t* base, uint32_t manager);
  bool ContinueSetup(PPCContext& ctx, uint8_t* base);
  bool Wait(std::string reason);
  void BeginSetup(uint32_t manager);
  bool LiveOptions(uint8_t* base, uint32_t manager, uint32_t options = 0) const;
  uint32_t LiveMainMenuRoot(PPCContext& ctx, uint8_t* base, uint32_t manager, uint32_t page) const;
  bool LiveExtraSource(PPCContext& ctx, uint8_t* base, uint32_t manager) const;
  bool LiveGeneric(uint8_t* base, uint32_t generic) const;
  bool TryShowMenuExtra(PPCContext& ctx, uint8_t* base, bool quit);
  bool LivePauseOptions(uint8_t* base, uint32_t menu) const;
  bool OpenFromPause(PPCContext& ctx, uint8_t* base, uint32_t menu, uint32_t page);
  void ObservePauseExit(uint8_t* base);
  bool ReturnToOptions(PPCContext& ctx, uint8_t* base, uint32_t manager);
  bool Fail(std::string error);
  void OnEvent(PPCContext& ctx, uint8_t* base);
  void OnAccept(PPCContext& ctx, uint8_t* base);
  void OnBack(PPCContext& ctx, uint8_t* base);
  void ObserveExit(uint8_t* base);
  void Sync(PPCContext& ctx, uint8_t* base);
  bool Request(PPCContext& ctx, uint8_t* base, uint32_t id);
  bool PublishDialog(PPCContext& ctx, uint8_t* base, bool force = false);
  void SyncExtra(PPCContext& ctx, uint8_t* base);
  void RestoreExtraFocus(PPCContext& ctx, uint8_t* base);
  bool PublishExtra(PPCContext& ctx, uint8_t* base);
  void Focus(PPCContext& ctx, uint8_t* base, Page& page, size_t row);
  void FocusPopup(PPCContext& ctx, uint8_t* base, size_t choice);
  void PlaySound(PPCContext& ctx, uint8_t* base, uint32_t sound);
  void TraceTransition(uint8_t* base, uint32_t manager);
  Page* FindPage(uint32_t component);
  static bool IsSettings(uint32_t id) { return id >= 21 && id <= 23; }

  NativeBridgeApi api_;
  native_frontend::SettingsPageBinding binding_;
  std::array<Page, 2> pages_;
  std::unique_ptr<FrontendController> controller_;
  std::unordered_map<uint32_t, std::string> text_cache_;
  struct LiteralEntry {uint32_t key{}, value{};};
  std::mutex literal_mutex_;
  std::unordered_map<std::string, LiteralEntry> literals_;
  std::unordered_map<uint32_t, LiteralEntry> literal_value_aliases_;
  std::unordered_map<std::string, uint32_t> literal_values_;
  Buffer scratch_, message_, label_a_, label_b_;
  uint32_t manager_{}, ui_{}, registry_{}, generic_{}, interface_{}, labels_{};
  uint32_t return_page_ = 21;
  DialogView published_dialog_;
  std::array<uint32_t, 5> component_callbacks_{};
  std::array<uint32_t, 7> popup_callbacks_{};
  uint32_t generation_ = 0, pending_manager_ = 0, popup_event_ = 0;
  bool attempted_ = false, installed_ = false, session_ = false;
  enum class Extra { kNone, kCredits, kQuit };
  Extra extra_ = Extra::kNone;
  Extra pending_extra_ = Extra::kNone;
  uint32_t pending_extra_return_ = UINT32_MAX;
  uint32_t extra_return_ = 0, credit_page_ = 0;
  std::array<uint32_t, 2> extra_entry_names_{};
  uint32_t extra_focus_name_ = 0;
  bool extra_focus_pending_ = false;
  size_t popup_choice_ = 0;
  bool loading_layout_ = false;
  bool setup_requested_ = false;
  std::array<uint32_t, 4> last_transition_{};
  uint32_t pause_rows_menu_ = 0, pause_video_row_ = 0, pause_effects_row_ = 0;
  uint32_t pause_video_text_ = 0, pause_effects_text_ = 0;
  uint32_t pause_menu_ = 0, pause_return_ = 0;
  uint32_t pause_release_menu_ = 0, pause_selection_ = 0, pause_selection_code_ = 0;
  bool pause_entered_ = false;
  uint32_t resume_page_ = UINT32_MAX;
  std::string error_;
};

}  // namespace legodimensions::settings
