#pragma once

#include "settings_registry.h"
#include "settings_service.h"

#include <deque>

namespace legodimensions::settings {

enum class FrontendPage : uint32_t { kVideo = 21, kEffects = 22 };
enum class RowAction { kSetting, kApply, kDefaults, kBack };
struct FrontendRow {
  RowAction action = RowAction::kSetting;
  std::string element, value_element, key, label;
};
struct FrontendLayout {
  std::string name, title_element, title, xml;
  std::vector<FrontendRow> rows;
};
FrontendLayout MakeFrontendLayout(FrontendPage page, uint32_t generation = 0);
std::string DisplaySettingValue(std::string_view key, std::string_view value);
NormalizedValue AdjustSettingValue(std::string_view key, std::string_view value, int direction);
bool FrontendSettingEnabled(std::string_view key, const Values& values);

enum class FrontendDialog { kNone, kKeep, kSaveFailed, kDiscard, kDefaults, kInfo, kError, kStartup, kRecovery, kApplying, kRestoring };
struct DialogView {
  FrontendDialog kind = FrontendDialog::kNone;
  std::string message;
  std::vector<std::string> buttons;
};

// Guest frontend thread only. UI setters/persistence are reached exclusively
// through the existing queued service. Poll never waits on a future. Native
// widget/controller hooks consume this model; it has no ImGui/F-key UI path.
class FrontendController {
 public:
  using Now = QueuedSettings::Now;
  explicit FrontendController(std::shared_ptr<QueuedSettings> service,
                              Now now = [] { return Clock::now(); });
  bool Enter(FrontendPage page);
  // Presentation-only remap for a rebuilt screen. Preserve the transaction,
  // pending copied commands and any recovery dialog; never submit Open here.
  void RebindPage(FrontendPage page);
  bool Adjust(size_t row, int direction);
  bool Activate(size_t row);
  bool Back();
  bool Choose(unsigned button);
  // Unexpected transition out of settings: finish any in-flight operation,
  // then cancel/restore on the UI thread. Poll until the reply is consumed.
  void Leave();
  void Poll();
  void SetTrace(std::function<void(std::string)> trace) { trace_ = std::move(trace); }

  bool busy() const;
  bool exit_requested() const { return exit_; }
  const FrontendLayout& layout() const { return layout_; }
  FrontendPage page() const { return page_; }
  const Reply& reply() const { return reply_; }
  DialogView dialog() const;

 private:
  bool Send(Command command);
  void StartNext();
  void Receive(CommandKind kind, Reply reply);
  void Show(FrontendDialog kind, std::string message);
  bool Interactive() const;

  std::shared_ptr<QueuedSettings> service_;
  Now now_;
  std::function<void(std::string)> trace_;
  FrontendLayout layout_;
  FrontendPage page_ = FrontendPage::kVideo;
  Reply reply_;
  std::future<Reply> pending_;
  CommandKind pending_kind_ = CommandKind::kSnapshot;
  std::deque<Command> commands_;
  Clock::time_point received_at_{}, snapshot_sent_at_{};
  FrontendDialog dialog_ = FrontendDialog::kNone;
  std::string message_;
  bool leaving_ = false, exit_ = false, session_open_ = false;
};

}  // namespace legodimensions::settings
