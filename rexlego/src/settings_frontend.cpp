#include "settings_frontend.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <stdexcept>

namespace legodimensions::settings {
namespace {
const char* CommandName(CommandKind kind) {
  switch (kind) {
    case CommandKind::kOpen: return "open";
    case CommandKind::kEdit: return "edit";
    case CommandKind::kDefaults: return "defaults";
    case CommandKind::kApply: return "apply";
    case CommandKind::kKeep: return "keep";
    case CommandKind::kRevert: return "revert";
    case CommandKind::kCancel: return "cancel";
    case CommandKind::kSnapshot: return "snapshot";
    case CommandKind::kAcknowledgeStartupError: return "acknowledge startup error";
  }
  return "unknown";
}
const char* PhaseName(Phase phase) {
  switch (phase) {
    case Phase::kClosed: return "closed";
    case Phase::kEditing: return "editing";
    case Phase::kPreview: return "preview";
    case Phase::kRecoveryFailed: return "recovery failed";
    case Phase::kApplying: return "applying";
    case Phase::kRestoring: return "restoring";
  }
  return "unknown";
}
const char* NoticeName(Notice notice) {
  switch (notice) {
    case Notice::kNone: return "none";
    case Notice::kInvalidValue: return "invalid value";
    case Notice::kConflict: return "conflict";
    case Notice::kApplyFailed: return "apply failed";
    case Notice::kSaveFailed: return "save failed";
    case Notice::kKeepChanges: return "keep changes";
    case Notice::kReverted: return "restored";
    case Notice::kSaved: return "saved";
    case Notice::kRecoveryFailed: return "recovery failed";
    case Notice::kApplying: return "applying";
    case Notice::kRestoring: return "restoring";
  }
  return "unknown";
}
const SettingSpec* FindSpec(std::string_view key) {
  for (const auto& spec : NativeSettingsCatalog()) if (spec.key == key) return &spec;
  return nullptr;
}
std::string Number(double value) {
  char buffer[64];
  auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, 6);
  return result.ec == std::errc{} ? std::string(buffer, result.ptr) : std::string{};
}
}  // namespace

FrontendLayout MakeFrontendLayout(FrontendPage page, uint32_t generation) {
  if (page != FrontendPage::kVideo && page != FrontendPage::kEffects)
    throw std::invalid_argument("Unknown native settings page");
  FrontendLayout result;
  result.name = page == FrontendPage::kVideo ? "NativeVideoOptions" : "NativeEffectsOptions";
  if (generation > 1) result.name += "_g" + std::to_string(generation);
  result.title = page == FrontendPage::kVideo ? "VIDEO" : "EFFECTS";
  result.title_element = result.name + "Title";
  for (const auto& spec : NativeSettingsCatalog()) {
    if ((spec.group == SettingsGroup::kGraphics) != (page == FrontendPage::kEffects)) continue;
    result.rows.push_back({RowAction::kSetting, {}, {}, std::string(spec.key), std::string(spec.label)});
    static constexpr std::pair<std::string_view, std::string_view> labels[] = {
      {"fullscreen", "Fullscreen"}, {"native_vsync", "VSync"},
      {"native_fps_limit", "Frame rate limit"}, {"native_resolution_scale", "Render resolution"},
      {"native_show_fps", "FPS meter"},
      {"native_bloom", "Bloom glow"}, {"native_cel_shading", "Cel shading"},
      {"native_fsr3", "FSR 3 upscaling"}, {"native_fsr3_quality", "FSR 3 quality"},
      {"native_fsr3_sharpen", "FSR 3 sharpening"}, {"native_fsr3_sharpness", "FSR 3 sharpness"},
      {"native_fsr3_mip_bias", "Texture detail"}, {"present_effect", "Spatial upscaling"},
      {"present_fsr_sharpness_reduction", "FSR 1 sharpness"},
      {"present_fsr_max_upsampling_passes", "FSR 1 upscaling passes"},
      {"native_anisotropic_override", "Texture filtering"}, {"native_depth_of_field", "Depth of field"},
    };
    for (auto [key, label] : labels) if (spec.key == key) result.rows.back().label = label;
  }
  for (auto [action, label] : {std::pair{RowAction::kApply, "Apply"},
                             {RowAction::kDefaults, "Restore defaults"},
                             {RowAction::kBack, "Back"}})
    result.rows.push_back({action, {}, {}, {}, label});
  for (size_t i = 0; i < result.rows.size(); ++i) {
    auto& row = result.rows[i];
    row.element = result.name + "Row" + std::to_string(i);
    row.value_element = row.element + "Value";
  }
  // Native title widgets/templates only. Labels/values are set through the
  // title's text setter after loading; XML names contain no user data.
  result.xml = "<page name=\"" + result.name + "\" focus=\"" + result.rows.front().element +
      "\" parserversion=\"1\"><menu name=\"" + result.name +
      "Menu\"/><screensize width=\"1920\" height=\"1080\"/><uielements>";
  result.xml += "<text name=\"" + result.title_element +
      "\" size=\"18\" width=\"1440\" align=\"left\" lines=\"1\"><dimensions x=\"240\" y=\"285\" origin=\"top_left\"/></text>";
  for (size_t i = 0; i < result.rows.size(); ++i) {
    const auto& row = result.rows[i];
    const auto y = std::to_string(365 + i * 40);
    result.xml += "<text name=\"" + row.element +
        "\" size=\"9\" width=\"1000\" align=\"left\" lines=\"1\" text_template=\"simpleselect\"><dimensions x=\"240\" y=\"" +
        y + "\" origin=\"top_left\"/></text>";
    result.xml += "<text name=\"" + row.value_element +
        "\" size=\"9\" width=\"420\" align=\"right\" lines=\"1\"><dimensions x=\"1680\" y=\"" +
        y + "\" origin=\"top_left\"/></text>";
  }
  result.xml += "</uielements><navigation>";
  for (size_t i = 0; i < result.rows.size(); ++i) {
    result.xml += "<navigate from=\"" + result.rows[i].element + "\" direction=\"up\" to=\"" +
        result.rows[(i + result.rows.size() - 1) % result.rows.size()].element + "\"/>";
    result.xml += "<navigate from=\"" + result.rows[i].element + "\" direction=\"down\" to=\"" +
        result.rows[(i + 1) % result.rows.size()].element + "\"/>";
  }
  result.xml += "</navigation></page>";
  return result;
}

std::string DisplaySettingValue(std::string_view key, std::string_view value) {
  if (value == "true") return "On";
  if (value == "false") return "Off";
  if (key == "native_fps_limit" && value == "0") return "Unlocked";
  if (key == "native_cel_shading") {
    if (value == "original") return "Original";
    if (value == "everywhere") return "Everywhere";
    if (value == "off") return "Original";
  }
  if (key == "native_fsr3_quality") {
    if (value == "native") return "Native resolution";
    if (value == "quality") return "Quality";
    if (value == "balanced") return "Balanced";
    if (value == "performance") return "Performance";
    if (value == "ultra_performance") return "Ultra performance";
  }
  if (key == "present_effect") {
    if (value == "bilinear") return "Bilinear";
    if (value == "fsr") return "AMD FSR 1";
  }
  if (key == "native_resolution_scale") {
    int scale{};
    const auto r = std::from_chars(value.data(), value.data() + value.size(), scale);
    if (r.ec == std::errc{} && r.ptr == value.data() + value.size()) {
      if (scale == 0) return "Automatic";
      return std::to_string(scale) + "% (" + std::to_string(1280 * scale / 100) + " x " +
          std::to_string(720 * scale / 100) + ")";
    }
  }
  if (key == "native_anisotropic_override") {
    if (value == "-2") return "Installation setting";
    if (value == "-1") return "Game setting";
    if (value == "0") return "Off";
    if (value == "1") return "1x";
    if (value == "2") return "2x";
    if (value == "3") return "4x";
    if (value == "4") return "8x";
    if (value == "5") return "16x";
  }
  return std::string(value);
}

NormalizedValue AdjustSettingValue(std::string_view key, std::string_view value, int direction) {
  const auto* spec = FindSpec(key);
  if (!spec || (direction != -1 && direction != 1)) return {false, {}, "Invalid setting adjustment"};
  if (spec->type == ConfigType::kBoolean) {
    if (value != "true" && value != "false") return {false, {}, "Invalid current setting value"};
    return NormalizeNativeSetting(key, value == "true" ? "false" : "true");
  }
  if (key == "native_fps_limit") {
    const auto current = NormalizeNativeSetting(key, value);
    if (!current.valid) return current;
    int limit{};
    const auto parsed = std::from_chars(current.value.data(), current.value.data() + current.value.size(), limit);
    if (parsed.ec != std::errc{} || parsed.ptr != current.value.data() + current.value.size())
      return {false, {}, "Invalid current frame rate limit"};
    static constexpr std::array limits{30, 60, 100, 120, 144, 240};
    // Keep old custom limits loadable; an edit moves to the nearest preset
    // in the requested direction. Zero is the last, unlocked menu choice.
    if (!limit) return NormalizeNativeSetting(key, direction > 0 ? "30" : "240");
    if (direction > 0) {
      const auto next = std::upper_bound(limits.begin(), limits.end(), limit);
      return NormalizeNativeSetting(key, next == limits.end() ? "0" : std::to_string(*next));
    }
    const auto next = std::lower_bound(limits.begin(), limits.end(), limit);
    return NormalizeNativeSetting(key, next == limits.begin() ? "0" : std::to_string(*std::prev(next)));
  }
  // Keep old Off saves loadable but do not offer the unlit material path.
  if (key == "native_cel_shading") {
    if (value != "original" && value != "everywhere" && value != "off")
      return {false, {}, "Invalid current setting value"};
    return NormalizeNativeSetting(key, value == "everywhere" ? "original" : "everywhere");
  }
  if (!spec->choices.empty()) {
    auto it = std::find(spec->choices.begin(), spec->choices.end(), value);
    const auto size = int(spec->choices.size());
    const auto index = it == spec->choices.end() ? (direction > 0 ? -1 : 0) : int(it - spec->choices.begin());
    return NormalizeNativeSetting(key, spec->choices[(index + direction + size) % size]);
  }
  double number{};
  if (spec->type == ConfigType::kInteger) {
    int integer{};
    const auto p = std::from_chars(value.data(), value.data() + value.size(), integer);
    if (p.ec != std::errc{} || p.ptr != value.data() + value.size())
      return {false, {}, "Invalid current setting value"};
  }
  auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !std::isfinite(number))
    return {false, {}, "Invalid current setting value"};
  if (key == "native_resolution_scale") {
    if (direction > 0) number = number < 20 ? 20 : std::floor(number / 5) * 5 + 5;
    else number = number <= 20 ? 0 : std::ceil(number / 5) * 5 - 5;
  } else number += direction * (spec->type == ConfigType::kFloat ? 0.01 : 1);
  number = std::clamp(number, spec->minimum, spec->maximum);
  return NormalizeNativeSetting(key, spec->type == ConfigType::kInteger ? std::to_string(int(number)) : Number(number));
}

bool FrontendSettingEnabled(std::string_view key, const Values& values) {
  const auto is = [&](std::string_view parent, std::string_view value) {
    const auto it = values.find(parent);
    return it != values.end() && it->second == value;
  };
  if (key == "native_fsr3_quality" || key == "native_fsr3_sharpen" || key == "native_fsr3_mip_bias")
    return is("native_fsr3", "true");
  if (key == "native_fsr3_sharpness")
    return is("native_fsr3", "true") && is("native_fsr3_sharpen", "true");
  if (key == "native_resolution_scale" || key == "present_effect")
    return is("native_fsr3", "false");
  if (key == "present_fsr_sharpness_reduction" || key == "present_fsr_max_upsampling_passes")
    return is("native_fsr3", "false") && is("present_effect", "fsr");
  return true;
}

FrontendController::FrontendController(std::shared_ptr<QueuedSettings> service, Now now)
    : service_(std::move(service)), now_(std::move(now)) {}

bool FrontendController::busy() const {
  if (pending_.valid() && pending_kind_ != CommandKind::kSnapshot) return true;
  return std::any_of(commands_.begin(), commands_.end(), [](const auto& c) { return c.kind != CommandKind::kSnapshot; });
}
bool FrontendController::Interactive() const {
  return !leaving_ && !exit_ && !busy() && dialog_ == FrontendDialog::kNone && reply_.phase == Phase::kEditing;
}
void FrontendController::Show(FrontendDialog kind, std::string message) {
  dialog_ = kind;
  message_ = std::move(message);
}
bool FrontendController::Send(Command command) {
  if (!service_ || commands_.size() >= 4) return false;
  if (trace_ && command.kind != CommandKind::kSnapshot) {
    auto text = std::string("Native settings action: page=") +
        (page_ == FrontendPage::kVideo ? "Video " : "Effects ") + CommandName(command.kind);
    if (command.kind == CommandKind::kEdit) {
      const auto previous = reply_.values.find(command.key);
      text += " " + command.key + " old=" +
          (previous == reply_.values.end() ? "unknown" : previous->second) + " requested=" + command.value;
    } else text += " values=" + NativeSettingsLogValues(reply_.values);
    trace_(std::move(text));
  }
  commands_.push_back(std::move(command));
  StartNext();
  return true;
}
void FrontendController::StartNext() {
  if (pending_.valid() || commands_.empty()) return;
  auto command = std::move(commands_.front());
  commands_.pop_front();
  pending_kind_ = command.kind;
  if (command.kind == CommandKind::kSnapshot) snapshot_sent_at_ = now_();
  pending_ = service_->Submit(std::move(command));
}
bool FrontendController::Enter(FrontendPage page) {
  if (leaving_ || busy()) return false;
  RebindPage(page);
  exit_ = false;
  if (reply_.phase != Phase::kClosed) return true;
  if (!service_) { Show(FrontendDialog::kError, "Native settings are unavailable."); return false; }
  Show(FrontendDialog::kNone, {});
  return Send({CommandKind::kOpen});
}
void FrontendController::RebindPage(FrontendPage page) {
  auto layout = MakeFrontendLayout(page);
  layout_ = std::move(layout);
  page_ = page;
}
bool FrontendController::Adjust(size_t row, int direction) {
  if (!Interactive() || row >= layout_.rows.size() || layout_.rows[row].action != RowAction::kSetting) return false;
  const auto& key = layout_.rows[row].key;
  if (!FrontendSettingEnabled(key, reply_.values)) return false;
  const auto it = reply_.values.find(key);
  if (it == reply_.values.end()) return false;
  auto value = AdjustSettingValue(key, it->second, direction);
  if (!value.valid) { Show(FrontendDialog::kError, value.error); return false; }
  return Send({CommandKind::kEdit, key, std::move(value.value)});
}
bool FrontendController::Activate(size_t row) {
  if (!Interactive() || row >= layout_.rows.size()) return false;
  switch (layout_.rows[row].action) {
    case RowAction::kSetting: return Adjust(row, 1);
    case RowAction::kApply: return Send({CommandKind::kApply});
    case RowAction::kDefaults: Show(FrontendDialog::kDefaults, "Restore default settings"); return true;
    case RowAction::kBack: return Back();
  }
  return false;
}
bool FrontendController::Back() {
  if (leaving_ || exit_ || busy()) return false;
  if (dialog_ == FrontendDialog::kRestoring) return false;
  if (dialog_ != FrontendDialog::kNone) {
    if (dialog_ == FrontendDialog::kKeep || dialog_ == FrontendDialog::kSaveFailed ||
        dialog_ == FrontendDialog::kDiscard || dialog_ == FrontendDialog::kDefaults)
      return Choose(1);
    return Choose(0);
  }
  if (!session_open_ && reply_.phase == Phase::kClosed) { exit_ = true; return true; }
  if (reply_.phase == Phase::kRecoveryFailed) { Show(FrontendDialog::kRecovery, reply_.error); return true; }
  if (reply_.phase != Phase::kEditing) return false;
  if (reply_.dirty) { Show(FrontendDialog::kDiscard, "Discard unsaved changes"); return true; }
  return Send({CommandKind::kCancel});
}
bool FrontendController::Choose(unsigned button) {
  if (leaving_ || exit_ || busy() || dialog_ == FrontendDialog::kNone) return false;
  const bool two = dialog_ == FrontendDialog::kKeep || dialog_ == FrontendDialog::kSaveFailed ||
      dialog_ == FrontendDialog::kDiscard || dialog_ == FrontendDialog::kDefaults;
  if (button > (two ? 1u : 0u)) return false;
  switch (dialog_) {
    case FrontendDialog::kKeep:
    case FrontendDialog::kSaveFailed:
      return Send({button == 0 ? CommandKind::kKeep : CommandKind::kRevert});
    case FrontendDialog::kDiscard:
      if (button == 0) return Send({CommandKind::kCancel});
      break;
    case FrontendDialog::kDefaults:
      if (button == 0) return Send({CommandKind::kDefaults});
      break;
    case FrontendDialog::kStartup: return Send({CommandKind::kAcknowledgeStartupError});
    case FrontendDialog::kRecovery: return Send({CommandKind::kRevert});
    case FrontendDialog::kApplying: return Send({CommandKind::kRevert});
    case FrontendDialog::kRestoring: return false;
    default: break;
  }
  Show(FrontendDialog::kNone, {});
  return true;
}
void FrontendController::Leave() {
  if (exit_ || leaving_) return;
  if (!session_open_ && !pending_.valid() && commands_.empty()) {
    exit_ = true;
    Show(FrontendDialog::kNone, {});
    return;
  }
  leaving_ = true;
  commands_.clear();
  Show(FrontendDialog::kNone, {});
  if (service_) Send({CommandKind::kCancel});
  else { leaving_ = false; exit_ = true; }
}
void FrontendController::Receive(CommandKind kind, Reply reply) {
  const auto previous_phase = reply_.phase;
  const bool audit = kind != CommandKind::kSnapshot || previous_phase != reply.phase ||
      reply_.values != reply.values || reply_.notice != reply.notice || reply_.error != reply.error;
  reply_ = std::move(reply);
  if (trace_ && audit)
    trace_(std::string("Native settings result: action=") + CommandName(kind) +
        " phase=" + PhaseName(reply_.phase) + " ok=" + (reply_.ok ? "true" : "false") +
        " notice=" + NoticeName(reply_.notice) +
        " values=" + NativeSettingsLogValues(reply_.values) +
        (reply_.error.empty() ? "" : " error=" + reply_.error));
  received_at_ = now_();
  if (kind == CommandKind::kOpen && reply_.ok) session_open_ = true;
  if (kind == CommandKind::kCancel && reply_.ok && reply_.phase == Phase::kClosed) session_open_ = false;
  if (reply_.phase == Phase::kPreview || reply_.phase == Phase::kApplying || reply_.phase == Phase::kRestoring)
    snapshot_sent_at_ = received_at_;
  if (!reply_.unavailable && reply_.ok && reply_.phase == Phase::kClosed &&
      (leaving_ || session_open_ || kind == CommandKind::kCancel)) {
    session_open_ = false;
    exit_ = true;
    leaving_ = false;
    Show(FrontendDialog::kNone, {});
    return;
  }
  if (reply_.phase == Phase::kApplying) {
    Show(FrontendDialog::kApplying, "Applying settings");
    return;
  }
  if (reply_.phase == Phase::kRestoring) {
    Show(FrontendDialog::kRestoring, "Restoring settings");
    return;
  }
  if (reply_.phase == Phase::kRecoveryFailed) {
    leaving_ = false;
    Show(FrontendDialog::kRecovery, reply_.error);
    return;
  }
  // The UI service expires previews before processing each command. A Keep
  // or Revert arriving exactly at that boundary must report the completed
  // rollback, rather than turn a normal timeout into a generic failure.
  if (!reply_.unavailable && reply_.phase == Phase::kEditing && reply_.notice == Notice::kReverted &&
      (kind == CommandKind::kKeep || kind == CommandKind::kRevert)) {
    Show(FrontendDialog::kInfo, "Changes restored");
    return;
  }
  if (!reply_.ok || reply_.notice == Notice::kApplyFailed) {
    leaving_ = false;
    Show(reply_.phase == Phase::kPreview ? FrontendDialog::kSaveFailed :
         reply_.phase == Phase::kRecoveryFailed || (reply_.unavailable && session_open_)
             ? FrontendDialog::kRecovery : FrontendDialog::kError,
         reply_.error.empty() ? "Could not change native settings." : reply_.error);
    return;
  }
  if (leaving_ && kind != CommandKind::kCancel) return;
  if (kind == CommandKind::kCancel && reply_.phase == Phase::kClosed) {
    exit_ = true;
    leaving_ = false;
    Show(FrontendDialog::kNone, {});
  } else if (!reply_.startup_error.empty()) {
    Show(FrontendDialog::kStartup, reply_.startup_error);
  } else if (reply_.phase == Phase::kPreview) {
    Show(reply_.error.empty() ? FrontendDialog::kKeep : FrontendDialog::kSaveFailed,
         reply_.error.empty() ? "Keep display settings" : reply_.error);
  } else if ((previous_phase == Phase::kPreview || previous_phase == Phase::kRestoring) && reply_.notice == Notice::kReverted) {
    Show(FrontendDialog::kInfo, "Changes restored");
  } else if (kind == CommandKind::kKeep ||
             ((kind == CommandKind::kApply || previous_phase == Phase::kApplying) && reply_.notice == Notice::kSaved)) {
    Show(FrontendDialog::kInfo, "Settings saved");
  } else if (kind == CommandKind::kDefaults) {
    Show(FrontendDialog::kInfo, "Defaults ready. Select Apply to save");
  } else if (kind != CommandKind::kSnapshot) {
    Show(FrontendDialog::kNone, {});
  }
}
void FrontendController::Poll() {
  if (pending_.valid() && pending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    const auto kind = pending_kind_;
    try {
      Receive(kind, pending_.get());
    } catch (const std::exception& error) {
      Reply interrupted;
      interrupted.unavailable = true;
      interrupted.error = "Native settings reply was interrupted: " + std::string(error.what());
      Receive(kind, std::move(interrupted));
    }
    if (kind == CommandKind::kSnapshot && !commands_.empty() &&
        ((commands_.front().kind == CommandKind::kKeep && reply_.phase != Phase::kPreview) ||
         (commands_.front().kind == CommandKind::kRevert && reply_.phase == Phase::kEditing)))
      commands_.pop_front();
  }
  StartNext();
  const bool asynchronous = reply_.phase == Phase::kApplying || reply_.phase == Phase::kRestoring;
  if (!pending_.valid() && commands_.empty() && !exit_ && (!leaving_ || asynchronous) &&
      (reply_.phase == Phase::kPreview || asynchronous) &&
      now_() - snapshot_sent_at_ >= std::chrono::milliseconds(100))
    Send({CommandKind::kSnapshot});
}
DialogView FrontendController::dialog() const {
  DialogView view{dialog_, message_, {}};
  // The title font and popup have limited space. Keep native prompts short
  // and plain; the reply retains detailed backend errors for the log.
  if (dialog_ == FrontendDialog::kSaveFailed) view.message = "Could not save settings";
  if (dialog_ == FrontendDialog::kError) view.message = "Could not apply settings";
  if (dialog_ == FrontendDialog::kRecovery) view.message = "Could not restore settings";
  if (dialog_ == FrontendDialog::kStartup) view.message = "Saved settings could not be loaded";
  switch (dialog_) {
    case FrontendDialog::kKeep:
    case FrontendDialog::kSaveFailed: {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now_() - received_at_).count();
      const auto remaining = std::max<int64_t>(0, reply_.confirmation_ms - elapsed);
      view.message += " - " + std::to_string((remaining + 999) / 1000) + " seconds to restore";
      view.buttons = {dialog_ == FrontendDialog::kSaveFailed ? "Retry save" : "Keep", "Revert"};
      break;
    }
    case FrontendDialog::kDiscard: view.buttons = {"Discard", "Keep editing"}; break;
    case FrontendDialog::kDefaults: view.buttons = {"Restore defaults", "Cancel"}; break;
    case FrontendDialog::kRecovery: view.buttons = {"Retry restore"}; break;
    case FrontendDialog::kApplying: view.buttons = {"Cancel"}; break;
    case FrontendDialog::kRestoring: break;
    case FrontendDialog::kNone: break;
    default: view.buttons = {"Continue"}; break;
  }
  return view;
}

}  // namespace legodimensions::settings
