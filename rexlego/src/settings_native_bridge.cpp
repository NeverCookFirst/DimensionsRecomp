#include "settings_native_bridge.h"
#include "mod_menu.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <utility>

namespace legodimensions::settings {
using native_frontend::CallGuest;
using native_frontend::component_detail::Load;
using native_frontend::component_detail::Store;
namespace {
constexpr uint32_t kUi = 0x849E21CC, kRegistry = 0x849E220C, kGeneric = 0x849F562C, kManager = 0x849F5640;
constexpr std::string_view kCredits[] = {
  "Thanks for playing Dimensions Recompiled! Made by NeverCookFirst",
};
std::string Translate(std::string_view value) {
  // The language mod and its font are loaded together on startup.
  static const auto translations = [] {
    std::unordered_map<std::string, std::string> entries;
    const auto path = mods::RussianNativeMenuTranslations();
    if (path.empty()) return entries;
    std::ifstream stream(path);
    for (std::string line; std::getline(stream, line);) {
      const auto tab = line.find('\t');
      if (tab != std::string::npos)
        entries.emplace(line.substr(0, tab), line.substr(tab + 1));
    }
    return entries;
  }();
  if (const auto it = translations.find(std::string(value)); it != translations.end()) return it->second;
  constexpr std::string_view timer = " seconds to restore";
  constexpr std::string_view preview = "Keep display settings - ";
  if (!translations.empty() && value.starts_with(preview) && value.ends_with(timer))
    return "Сохранить параметры экрана — до отмены: " +
        std::string(value.substr(preview.size(), value.size() - preview.size() - timer.size())) + " с";
  return std::string(value);
}
}
NativeSettingsBridge* NativeSettingsBridge::active_ = nullptr;
uint32_t NativeMenuInputEvent(uint32_t forced, uint32_t directions, uint32_t buttons) noexcept {
  return forced ? forced : directions & 8 ? 11u : directions & 2 ? 12u :
      directions & 1 ? 9u : directions & 4 ? 10u : buttons & 0x40000000u ? 15u :
      buttons & 0x80000000u ? 16u : 0u;
}
NativeSettingsBridge::NativeSettingsBridge(NativeBridgeApi api) : api_(std::move(api)) {
  // Callback thunks carry native guest object addresses, never host pointers.
  // Multiple live title bridges are intentionally not supported by this ABI.
  if (!active_) active_ = this;
}
NativeSettingsBridge::~NativeSettingsBridge() { if (active_ == this) active_ = nullptr; }
bool NativeSettingsBridge::Fail(std::string error) { error_ = std::move(error); return false; }
uint32_t NativeSettingsBridge::Allocate(uint8_t* base, uint32_t bytes) {
  const auto p = api_.allocate(bytes);
  if (!p || p > UINT32_MAX - bytes) return 0;
  std::memset(base + p, 0, bytes);
  return p;
}
uint32_t NativeSettingsBridge::String(uint8_t* base, std::string_view text) {
  if (text.size() >= 1024 * 1024) return 0;
  const auto p = Allocate(base, uint32_t(text.size() + 1));
  if (p) std::memcpy(base + p, text.data(), text.size());
  return p;
}
bool NativeSettingsBridge::Fill(uint8_t* base, Buffer& buffer, std::string_view text) {
  if (text.size() >= 1024 * 1024) return false;
  const auto needed = uint32_t(text.size() + 1);
  if (needed > buffer.capacity) {
    const auto capacity = (needed + 255) & ~255u;
    const auto p = Allocate(base, capacity);
    if (!p) return false;
    buffer = {p, capacity};
  }
  std::memcpy(base + buffer.address, text.data(), text.size());
  base[buffer.address + text.size()] = 0;
  return true;
}
bool NativeSettingsBridge::Text(PPCContext& ctx, uint8_t* base, uint32_t widget, std::string_view text) {
  if (!widget) return false;
  auto it = text_cache_.find(widget);
  if (it != text_cache_.end() && it->second == text) return true;
  const auto key = Literal(base, "NATIVE_FRONTEND_WIDGET_" + std::to_string(widget), text);
  if (!key) return false;
  CallGuest(api_.set_text, ctx, base, widget, key);
  text_cache_[widget] = text;
  return true;
}
uint32_t NativeSettingsBridge::Literal(uint8_t* base, std::string key, std::string_view value) {
  const auto translated = Translate(value);
  value = translated;
  uint32_t value_address = 0;
  {
    std::lock_guard lock(literal_mutex_);
    if (const auto it = literal_values_.find(std::string(value)); it != literal_values_.end()) value_address = it->second;
  }
  // Strings are immutable and retained: native glyph/layout caches may keep
  // the translated pointer after a later page has changed its text. Never
  // mutate an already-published string or call the guest heap under our lock.
  if (!value_address) value_address = String(base, value);
  if (!value_address) return 0;
  {
    std::lock_guard lock(literal_mutex_);
    value_address = literal_values_.try_emplace(std::string(value), value_address).first->second;
    // Change the key as well as the resolved pointer: the native glyph cache
    // may use either as its identity. Published key/value pairs never change.
    key += "_" + std::to_string(value_address);
    if (const auto it = literals_.find(key); it != literals_.end()) return it->second.key;
  }
  const auto key_address = String(base, key);
  if (!key_address) return 0;
  std::lock_guard lock(literal_mutex_);
  const auto entry = literals_.try_emplace(std::move(key), LiteralEntry{key_address, value_address}).first->second;
  // The native setter copies plain text to both the wrapper and its child,
  // then localizes the original plain-text pointer. Match only our retained
  // immutable value pointers; identical unowned English strings stay native.
  literal_value_aliases_.try_emplace(value_address, entry);
  return entry.key;
}
void NativeSettingsBridge::Tint(uint8_t* base, uint32_t widget, bool enabled, bool selected, bool bright) {
  // TU23 text wrappers are type 6. sub_82EFBF70 writes the base colour at
  // +420 R, +424 G, +428 B, +432 A; sub_82EFE948 consumes these for text.
  // Keep native alpha/fades and every layout/animation/selection field intact.
  std::array<uint8_t, 440> identity{};
  if (!widget || !api_.read_metadata ||
      !api_.read_metadata(widget, identity.data(), uint32_t(identity.size())) ||
      !Load(identity.data(), 0) || Load(identity.data(), 436) != 6) return;
  const float level = !enabled ? (selected ? 0.50f : 0.32f) : selected && !bright ? 0.72f : 1.0f;
  const float blue = enabled && selected ? 0.0f : level;
  Store(base, widget + 420, std::bit_cast<uint32_t>(level));
  Store(base, widget + 424, std::bit_cast<uint32_t>(level));
  Store(base, widget + 428, std::bit_cast<uint32_t>(blue));
}
uint32_t NativeSettingsBridge::LookupLiteral(uint8_t* base, uint32_t key, uint32_t* canonical_key) {
  if (!key) return 0;
  {
    std::lock_guard lock(literal_mutex_);
    if (const auto it = literal_value_aliases_.find(key); it != literal_value_aliases_.end()) {
      if (canonical_key) *canonical_key = it->second.key;
      return it->second.value;
    }
  }
  constexpr std::string_view prefix = "NATIVE_FRONTEND_";
  const auto* text = reinterpret_cast<const char*>(base + key);
  for (size_t i = 0; i < prefix.size(); ++i) if (text[i] != prefix[i]) return 0;
  size_t length = prefix.size();
  while (length < 96 && text[length]) ++length;
  if (length == 96) return 0;
  std::lock_guard lock(literal_mutex_);
  const auto it = literals_.find(std::string(text, length));
  if (canonical_key && it != literals_.end()) *canonical_key = it->second.key;
  return it == literals_.end() ? 0 : it->second.value;
}
bool NativeSettingsBridge::ResolveControls(PPCContext& ctx, uint8_t* base, Page& page) {
  page.rows.clear(); page.values.clear();
  if (!page.menu_name) page.menu_name = String(base, page.layout.name + "Menu");
  if (!page.menu_name || !CallGuest(api_.find_widget, ctx, base, page.root, page.menu_name)) return false;
  if (!page.title_name) page.title_name = String(base, page.layout.title_element);
  if (!page.title_name) return false;
  page.title = CallGuest(api_.find_widget, ctx, base, page.root, page.title_name);
  if (!page.title) return false;
  if (page.row_names.size() != page.layout.rows.size() || page.value_names.size() != page.layout.rows.size()) {
    page.row_names.clear(); page.value_names.clear();
    for (const auto& row : page.layout.rows) {
      page.row_names.push_back(String(base, row.element));
      page.value_names.push_back(String(base, row.value_element));
    }
  }
  for (size_t i = 0; i < page.layout.rows.size(); ++i) {
    const auto label_name = page.row_names[i], value_name = page.value_names[i];
    if (!label_name || !value_name) return false;
    const auto label = CallGuest(api_.find_widget, ctx, base, page.root, label_name);
    const auto value = CallGuest(api_.find_widget, ctx, base, page.root, value_name);
    if (!label || !value) return false;
    page.rows.push_back(label); page.values.push_back(value);
  }
  // Resolve the complete graph before changing any retained widget text.
  for (size_t i = 0; i < page.layout.rows.size(); ++i) {
    const auto& row = page.layout.rows[i];
    if (!Text(ctx, base, page.rows[i], row.label) ||
        !Text(ctx, base, page.values[i], row.action == RowAction::kSetting ? "..." : "")) return false;
  }
  return Text(ctx, base, page.title, page.layout.title);
}
bool NativeSettingsBridge::BuildPage(PPCContext& ctx, uint8_t* base, Page& page,
                                    const std::array<uint32_t, 5>& callbacks) {
  page.layout = MakeFrontendLayout(page.id, generation_);
  page.name = String(base, page.layout.name);
  const auto xml = String(base, page.layout.xml), component = Allocate(base, 28), vtable = Allocate(base, 20);
  if (!page.name || !xml || !component || !vtable) return Fail("Could not allocate native settings page");
  loading_layout_ = true;
  auto loaded = native_frontend::LoadPageLayout(api_.load_layout, api_.find_page, ctx, base, {registry_, xml, page.name});
  loading_layout_ = false;
  if (loaded.status != native_frontend::PageLoadStatus::kCreated)
    return Fail("Native settings page name is occupied or construction failed: " + page.layout.name);
  page.root = loaded.page;
  if (!ResolveControls(ctx, base, page)) return Fail("Native settings control graph is incomplete");
  if (!native_frontend::InitializeComponent(api_.construct_component, ctx, base,
      native_frontend::ComponentRequest{component, vtable, page.name, callbacks, 19}))
    return Fail("Could not initialize owned native settings component");
  if (!native_frontend::AttachComponentPage(api_.add_component, api_.find_component, ctx, base, ui_, component, page.root))
    return Fail("Native settings component name or page already has an owner");
  page.component = component;
  page.root_vtable = Load(base, page.root);
  page.root_name = Load(base, page.root + 12);
  CallGuest(api_.page_event, ctx, base, page.root, 1, 0);
  return true;
}
uint32_t NativeSettingsBridge::CompleteMenu(PPCContext& ctx, uint8_t* base, uint32_t xml_node, uint32_t parent) {
  // TU23's XML menu handler creates a widget only under MainMenuPage; other
  // original pages rely on their prebuilt GSC menu. Our fresh pages have none.
  // Extend that exact factory path only during our owned synchronous XML load.
  if (!loading_layout_ || !api_.construct_menu || !xml_node || !parent) return 0;
  const auto factory = Load(base, 0x849E2208);
  if (!factory || !Load(base, factory + 4)) return 0;
  return CallGuest(api_.construct_menu, ctx, base, Load(base, factory + 4), xml_node, parent);
}
bool NativeSettingsBridge::ReusePage(PPCContext& ctx, uint8_t* base, Page& page) {
  if (page.retired || !page.name || !page.root || !page.component) return false;
  if (!LivePage(base, page)) return false;
  // Search live registries first. Do not dereference a cached root merely
  // because a new frontend happens to have the same global object addresses.
  if (CallGuest(api_.find_page, ctx, base, registry_ + 24, page.name) != page.root) return false;
  const auto owner = CallGuest(api_.find_component, ctx, base, Load(base, ui_ + 104), page.name);
  if (owner && owner != page.component) return false;
  if (Load(base, page.root + 476) != page.component || Load(base, page.component + 20) != page.root) return false;
  Page refreshed = page;
  if (!ResolveControls(ctx, base, refreshed)) return false;
  bool initialize = false;
  if (!owner) {
    // A new UI component list may retain the page registry. Adopt this exact
    // owned page using a fresh component; never insert an old intrusive node
    // into two lists or overwrite another component with a matching name.
    const auto component = Allocate(base, 28), vtable = Allocate(base, 20);
    if (!component || !vtable || !native_frontend::InitializeComponent(api_.construct_component, ctx, base,
        native_frontend::ComponentRequest{component, vtable, page.name, component_callbacks_, 19})) return false;
    CallGuest(api_.add_component, ctx, base, ui_, component);
    Store(base, page.root + 476, component);
    Store(base, component + 20, page.root);
    Store(base, page.component + 20, 0);
    refreshed.component = component;
    initialize = true;
  }
  page = std::move(refreshed);
  if (initialize) CallGuest(api_.page_event, ctx, base, page.root, 1, 0);
  return true;
}
bool NativeSettingsBridge::LivePage(uint8_t* base, const Page& page) const {
  // GSC page storage belongs to the title's scene, while our component and
  // manager slots can survive returning to the main menu. Never dispatch a
  // virtual call through the retained root before checking its live identity.
  std::array<uint8_t, 480> data{};
  if (page.retired || !page.root || !page.component || !page.root_vtable ||
      !api_.read_metadata || !api_.read_metadata(page.root, data.data(), uint32_t(data.size()))) return false;
  return Load(data.data(), 0) == page.root_vtable && Load(data.data(), 12) == page.root_name &&
      Load(data.data(), 476) == page.component && Load(base, page.component + 20) == page.root;
}
bool NativeSettingsBridge::RegisteredPages() const {
  // Inspect the registry without calling methods on its page nodes. In
  // particular, the title's FindPage calls GetName through each node's vtable.
  uint32_t head{};
  if (!api_.read_metadata || !api_.read_metadata(registry_ + 24, &head, 4)) return false;
  auto node = __builtin_bswap32(head);
  unsigned found = 0;
  for (unsigned count = 0; node && count < 4096; ++count) {
    if (node == pages_[0].root) found |= 1;
    if (node == pages_[1].root) found |= 2;
    if (found == 3) return true;
    uint32_t next{};
    if (!api_.read_metadata(node + 32, &next, 4)) return false;
    const auto previous = node;
    node = __builtin_bswap32(next);
    if (node == previous) return false;
  }
  return false;
}
bool NativeSettingsBridge::RetireDeadPages(uint8_t* base) {
  for (auto& page : pages_) {
    if (!page.component || LivePage(base, page)) continue;
    // This component was allocated by the bridge and retained for the title
    // lifetime. Nulling its dead root also makes the native hide path safe.
    Store(base, page.component + 20, 0);
    page.retired = true;
  }
  uint32_t link = registry_ + 24;
  for (unsigned count = 0; count < 4096; ++count) {
    uint32_t word{};
    if (!api_.read_metadata(link, &word, 4)) return false;
    const auto node = __builtin_bswap32(word);
    if (!node) return true;
    uint32_t next{};
    if (!api_.read_metadata(node + 32, &next, 4) || __builtin_bswap32(next) == node) return false;
    bool retired = false;
    for (const auto& page : pages_) if (page.retired && node == page.root) {
      std::array<uint8_t, 16> identity{};
      if (!api_.read_metadata(node, identity.data(), uint32_t(identity.size()))) return false;
      // An address reused by a different live page belongs to that page.
      const auto name = Load(identity.data(), 12);
      retired = name == page.root_name || (!name && !Load(identity.data(), 0));
    }
    if (retired) Store(base, link, __builtin_bswap32(next));
    else link = node + 32;
  }
  return false;
}
void NativeSettingsBridge::SuspendRequest(uint8_t* base, uint32_t manager) {
  using Availability = native_frontend::SettingsPageBinding::Availability;
  const auto slots = native_frontend::SettingsPageBinding::Inspect(base, manager);
  const bool cleared = manager == manager_ && (slots == Availability::kAvailable || slots == Availability::kBusy);
  if ((!binding_.Owns(base, manager) && !cleared) || IsSettings(Load(base, manager + 164))) return;
  for (const auto offset : {168u, 172u}) {
    const auto target = Load(base, manager + offset);
    if (target == 21 || target == 22) {
      resume_page_ = target;
      Store(base, manager + offset, UINT32_MAX);
    }
  }
}
bool NativeSettingsBridge::Wait(std::string reason) {
  if (reason != error_ && api_.trace) api_.trace("Native settings waiting: " + reason);
  error_ = std::move(reason); return false;
}
void NativeSettingsBridge::BeginSetup(uint32_t manager) {
  installed_ = false; attempted_ = false; pending_manager_ = manager;
  extra_focus_pending_ = false;
  popup_event_ = 0; published_dialog_ = {};
  extra_ = Extra::kNone;
  ++generation_;
  if (controller_ && !controller_->exit_requested()) controller_->Leave();
}
bool NativeSettingsBridge::ReturnToOptions(PPCContext& ctx, uint8_t* base, uint32_t manager) {
  const auto current = Load(base, manager + 164), pending = Load(base, manager + 168), deferred = Load(base, manager + 172);
  if (!IsSettings(current) && !IsSettings(pending) && !IsSettings(deferred)) return true;
  if (pending == UINT32_MAX && deferred == UINT32_MAX) {
    const auto array = Load(base, manager + 140);
    const auto destination = pause_menu_ ? pause_return_ : 19u;
    if (array && (!destination || Load(base, array + destination * 4))) CallGuest(api_.request_page, ctx, base, manager, destination);
  }
  return false;
}
bool NativeSettingsBridge::Initialize(PPCContext& ctx, uint8_t* base, uint32_t manager, bool recreated) {
  setup_requested_ = true; // The native manager's initialization is a safe setup boundary.
  if (!recreated && ready(base, manager)) return true;
  if (!recreated && pending_manager_ == manager) return ContinueSetup(ctx, base);
  if (!recreated && attempted_ && !installed_) return false;
  BeginSetup(manager);
  return ContinueSetup(ctx, base);
}
bool NativeSettingsBridge::ContinueSetup(PPCContext& ctx, uint8_t* base) {
  const auto manager = pending_manager_;
  if (!manager) return false;
  if (active_ != this) { pending_manager_ = 0; attempted_ = true; return Fail("Only one native settings bridge may own callbacks"); }
  if (!api_.allocate || !api_.thunk || !api_.settings || !api_.now || !api_.read_metadata ||
      !api_.load_layout || !api_.find_page || !api_.find_widget || !api_.construct_component ||
      !api_.add_component || !api_.find_component || !api_.set_text || !api_.page_event ||
      !api_.selected_widget || !api_.configure_popup || !api_.reset_popup || !api_.publish_popup || !api_.request_page ||
      !api_.set_visible || !api_.refresh_list || !api_.construct_menu || !api_.select_widget) {
    pending_manager_ = 0; attempted_ = true; return Fail("Native settings runtime is unavailable");
  }
  if (controller_) {
    controller_->Poll();
    if (controller_->busy()) return Wait("Waiting for native settings cancellation before rebuilding frontend");
    if (controller_->exit_requested()) controller_.reset();
    else if (controller_->dialog().kind != FrontendDialog::kRecovery)
      return Wait("Waiting for native settings restoration before rebuilding frontend");
    // A failed restoration retains its controller. The new pages expose its
    // Retry restore dialog; a new settings transaction cannot replace it.
  }
  if (Load(base, kManager) != manager || !api_.settings()) return Wait("Waiting for current native frontend runtime");
  const auto ui = Load(base, kUi), registry = Load(base, kRegistry), generic = Load(base, kGeneric);
  if (!ui || !registry || !generic || !Load(base, ui + 104) || !LiveGeneric(base, generic))
    return Wait("Native frontend resources are not bound");
  using Binding = native_frontend::SettingsPageBinding;
  if (registry == registry_ && !RetireDeadPages(base)) return Wait("Waiting for a readable native page registry");
  if (binding_.Owns(base, manager)) {
    // A queued request must not reach the native Show routine with a dead
    // root. Suspend only our own destination while rebuilding inactive pages;
    // preserve the owner's click and replay it after the slots are rebound.
    SuspendRequest(base, manager);
    if (!ReturnToOptions(ctx, base, manager)) return Wait("Waiting to leave the previous native settings page");
    if (!binding_.Release(base, manager)) return Wait("Waiting to release previous native settings slots");
  }
  const auto available = Binding::Inspect(base, manager);
  if (available == Binding::Availability::kInvalid) return Wait("Native frontend page arrays are not initialized");
  if (available == Binding::Availability::kBusy) {
    ReturnToOptions(ctx, base, manager);
    return Wait("Waiting for the native frontend transition to finish");
  }
  if (available == Binding::Availability::kOccupied) {
    pending_manager_ = 0; attempted_ = true;
    return Fail("Retained native settings slots are occupied or in transition");
  }
  manager_ = manager; ui_ = ui; registry_ = registry; generic_ = generic;
  binding_ = {}; text_cache_.clear(); session_ = false;
  pending_manager_ = 0; attempted_ = true;
  if (!component_callbacks_[0]) {
    const auto destroy = api_.thunk(Destroy), event = api_.thunk(Event), query = api_.thunk(GetResult),
        set = api_.thunk(SetResult), allow = api_.thunk(AllowSwitch), accept = api_.thunk(Accept), back = api_.thunk(Back);
    if (!destroy || !event || !query || !set || !allow || !accept || !back)
      return Fail("Could not allocate native settings callback thunks");
    component_callbacks_ = {destroy, event, query, allow, set};
    popup_callbacks_ = {allow, set, event, allow, accept, back, query};
  }
  for (unsigned i = 0; i < pages_.size(); ++i) {
    if (ReusePage(ctx, base, pages_[i])) continue;
    Page fresh; fresh.id = i == 0 ? FrontendPage::kVideo : FrontendPage::kEffects;
    if (!BuildPage(ctx, base, fresh, component_callbacks_)) return false;
    pages_[i] = std::move(fresh);
  }
  // Fresh interface identity rejects late callbacks from the outgoing dialog.
  const auto vtable = Allocate(base, 28);
  interface_ = Allocate(base, 8); labels_ = Allocate(base, 8);
  if (!vtable || !interface_ || !labels_) return Fail("Could not allocate native dialog interface");
  for (unsigned i = 0; i < popup_callbacks_.size(); ++i) Store(base, vtable + i * 4, popup_callbacks_[i]);
  Store(base, interface_, vtable); Store(base, interface_ + 4, 21);
  if (!binding_.Install(base, manager_, {pages_[0].component, pages_[1].component, generic_},
                         {interface_, interface_, interface_}))
    return Fail("Retained native settings slots are occupied or in transition");
  installed_ = true; error_.clear();
  setup_requested_ = false;
  if (api_.trace) api_.trace("Native settings graph bound: generation=" + std::to_string(generation_));
  if (!pause_menu_) RevealOptions(ctx, base, Load(base, Load(base, manager_ + 140) + 19 * 4));
  if (resume_page_ != UINT32_MAX) {
    const auto target = std::exchange(resume_page_, UINT32_MAX);
    Request(ctx, base, target);
  }
  return true;
}
bool NativeSettingsBridge::ready(uint8_t* base, uint32_t manager) const {
  return installed_ && manager == manager_ && Load(base, kManager) == manager_ &&
      Load(base, kUi) == ui_ && Load(base, kRegistry) == registry_ &&
      Load(base, kGeneric) == generic_ && binding_.Owns(base, manager) &&
      LivePage(base, pages_[0]) && LivePage(base, pages_[1]) && RegisteredPages() && LiveGeneric(base, generic_);
}
bool NativeSettingsBridge::LiveGeneric(uint8_t* base, uint32_t generic) const {
  std::array<uint8_t, 48> component{};
  if (!api_.read_metadata || !api_.read_metadata(generic, component.data(), uint32_t(component.size()))) return false;
  const auto root = Load(component.data(), 20);
  std::array<uint8_t, 480> identity{};
  if (!root || root != Load(component.data(), 28) || !api_.read_metadata(root, identity.data(), uint32_t(identity.size())) ||
      !Load(identity.data(), 0) || !Load(identity.data(), 12) || Load(identity.data(), 476) != generic) return false;
  for (auto offset : {32u, 36u, 40u, 44u}) {
    uint32_t vtable{};
    const auto widget = Load(component.data(), offset);
    if (!widget || !api_.read_metadata(widget, &vtable, 4) || !vtable) return false;
  }
  return true;
}
uint32_t NativeSettingsBridge::LiveMainMenuRoot(PPCContext& ctx, uint8_t* base, uint32_t manager, uint32_t page) const {
  if (page != 17 || !manager || Load(base, kManager) != manager || Load(base, manager + 164) != page) return 0;
  if (Load(base, manager + 168) != UINT32_MAX || Load(base, manager + 172) != UINT32_MAX) return 0;
  const auto array = Load(base, manager + 140), count = Load(base, manager + 148);
  if (!array || page >= count || !api_.read_metadata || !api_.find_widget) return 0;
  const auto component = Load(base, array + page * 4);
  std::array<uint8_t, 44> controller{};
  if (!component || !api_.read_metadata(component, controller.data(), uint32_t(controller.size())) ||
      !Load(controller.data(), 0)) return 0;
  // TU23 stores MainMenuPage at controller+40 (also the base component+20).
  // Global 849F5F40 instead holds FindWidget(page, "MainMenu"), a menu child
  // whose +484 is not a page selection pointer. Never use it as a page root.
  const auto root = Load(controller.data(), 40), menu = Load(base, 0x849F5F40);
  std::array<uint8_t, 480> identity{};
  if (!root || root != Load(controller.data(), 20) || !menu || menu == root ||
      !api_.read_metadata(root, identity.data(), uint32_t(identity.size())) ||
      !Load(identity.data(), 0) || !Load(identity.data(), 12) || Load(identity.data(), 436) != 0 ||
      Load(identity.data(), 476) != component || Load(identity.data(), 448) != 2) return 0;
  // 821BF734 is TU23's immutable "MainMenu" name, used by its own constructor.
  return CallGuest(api_.find_widget, ctx, base, root, 0x821BF734) == menu ? root : 0;
}
bool NativeSettingsBridge::LiveExtraSource(PPCContext& ctx, uint8_t* base, uint32_t manager) const {
  return LiveMainMenuRoot(ctx, base, manager, pending_extra_return_) != 0;
}
bool NativeSettingsBridge::LiveOptions(uint8_t* base, uint32_t manager, uint32_t entering) const {
  if (!manager || Load(base, kManager) != manager) return false;
  const auto array = Load(base, manager + 140);
  if (!array || Load(base, manager + 148) <= 19) return false;
  const auto options = Load(base, array + 19 * 4);
  if (!options || (entering && options != entering)) return false;
  std::array<uint8_t, 24> component{};
  if (!api_.read_metadata(options, component.data(), uint32_t(component.size())) || !Load(component.data(), 0)) return false;
  const auto root = Load(component.data(), 20);
  std::array<uint8_t, 480> identity{};
  if (!root || !api_.read_metadata(root, identity.data(), uint32_t(identity.size())) ||
      !Load(identity.data(), 0) || !Load(identity.data(), 12) || Load(identity.data(), 476) != options) return false;
  // AboutToShow runs before the title pushes layer 2. Per-frame retries must
  // instead prove the original Options root is still visible and current.
  if (entering) return true;
  if (Load(base, manager + 164) != 19 || Load(identity.data(), 448) != 2) return false;
  for (const auto offset : {168u, 172u}) {
    const auto target = Load(base, manager + offset);
    if (target != UINT32_MAX && target != 19 && target != 21 && target != 22) return false;
  }
  return true;
}
void NativeSettingsBridge::OptionsEntering(PPCContext& ctx, uint8_t* base, uint32_t options) {
  const auto manager = Load(base, kManager);
  if (!LiveOptions(base, manager, options)) return;
  if (!ready(base, manager)) {
    if (!pending_manager_ || pending_manager_ != manager) BeginSetup(manager);
    setup_requested_ = true;
    ContinueSetup(ctx, base);
  }
  RevealOptions(ctx, base, options);
}
bool NativeSettingsBridge::LivePauseOptions(uint8_t* base, uint32_t menu) const {
  if (!api_.read_metadata) return false;
  // TU23's legacy menu stack stores 12 records of 240 bytes. Its depth is
  // the active index; record+20 is a descriptor index, not a menu ID.
  const auto depth = Load(base, 0x847D2D58);
  if (!depth || depth >= 12 || menu != 0x849C7370u + depth * 240) return false;
  std::array<uint8_t, 240> context{};
  if (!api_.read_metadata(menu, context.data(), uint32_t(context.size()))) return false;
  const auto index = Load(context.data(), 20) >> 16;
  if (index >= 64) return false;
  std::array<uint8_t, 28> descriptor{};
  return api_.read_metadata(0x849C7EB8u + index * 28, descriptor.data(), uint32_t(descriptor.size())) &&
      Load(descriptor.data(), 0) == 4 && Load(descriptor.data(), 8) == 0x839D08A0 &&
      Load(descriptor.data(), 12) == 0x839D4020;
}
bool NativeSettingsBridge::PauseOverlayActive(uint8_t* base, uint32_t menu) const {
  return pause_menu_ == menu && LivePauseOptions(base, menu);
}
bool NativeSettingsBridge::RenderPauseOptions(PPCContext& ctx, uint8_t* base, uint32_t menu, PPCFunc* draw) {
  if (!draw || !LivePauseOptions(base, menu)) return false;
  // TU23's legacy row emitter uses normalized Y (upwards), +224 for text
  // width, and +228 for both line height and spacing. Zero selects globals.
  // Fit the twelve Options rows below the HUD, preserving all native rows,
  // selection ordinals and the count-only initialization pass.
  const auto y = Load(base, menu + 168), width = Load(base, menu + 224), height = Load(base, menu + 228);
  const auto scaled = [base](uint32_t current, uint32_t fallback) {
    const auto value = std::bit_cast<float>(current);
    return std::bit_cast<uint32_t>((value == 0 ? std::bit_cast<float>(Load(base, fallback)) : value) * 0.70f);
  };
  Store(base, menu + 168, std::bit_cast<uint32_t>(0.40f));
  Store(base, menu + 224, scaled(width, 0x847D2D78));
  Store(base, menu + 228, scaled(height, 0x847D2D80));
  CallGuest(draw, ctx, base, menu);
  Store(base, menu + 168, y); Store(base, menu + 224, width); Store(base, menu + 228, height);
  return true;
}
bool NativeSettingsBridge::DrawPauseEntries(PPCContext& ctx, uint8_t* base, uint32_t menu, PPCFunc* draw_row) {
  if (!draw_row || !LivePauseOptions(base, menu)) return false;
  if (!pause_video_text_) pause_video_text_ = String(base, Translate("Video"));
  if (!pause_effects_text_) pause_effects_text_ = String(base, Translate("Effects"));
  if (!pause_video_text_ || !pause_effects_text_) return false;
  // Called immediately before the original Back row. The original emitter
  // owns layout, highlighting, row counts and navigation mask publication.
  pause_rows_menu_ = menu;
  pause_video_row_ = Load(base, menu + 156);
  CallGuest(draw_row, ctx, base, menu, pause_video_text_, 255);
  pause_effects_row_ = Load(base, menu + 156);
  CallGuest(draw_row, ctx, base, menu, pause_effects_text_, 255);
  return true;
}
bool NativeSettingsBridge::OpenFromPause(PPCContext& ctx, uint8_t* base, uint32_t menu, uint32_t page) {
  const auto manager = Load(base, kManager);
  if (!LivePauseOptions(base, menu) || !manager || Load(base, manager + 164) != 0) return false;
  pause_menu_ = menu; pause_return_ = 0; pause_entered_ = false;
  pause_selection_ = Load(base, menu + 8); pause_selection_code_ = Load(base, menu + 64);
  resume_page_ = page;
  if (!ready(base, manager)) {
    if (!pending_manager_ || pending_manager_ != manager) BeginSetup(manager);
    setup_requested_ = true;
    ContinueSetup(ctx, base);
  } else {
    resume_page_ = UINT32_MAX;
    Request(ctx, base, page);
  }
  return true;
}
bool NativeSettingsBridge::UpdatePauseOptions(PPCContext& ctx, uint8_t* base, uint32_t menu, PPCFunc* original) {
  if (!LivePauseOptions(base, menu)) return false;
  if (PauseOverlayActive(base, menu) || pause_release_menu_ == menu) {
    // The legacy controller must not consume the same press as our modal.
    for (const auto offset : {104u, 108u, 112u, 116u}) Store(base, menu + offset, 0);
    Store(base, menu + 8, pause_selection_); Store(base, menu + 64, pause_selection_code_);
    pause_release_menu_ = 0;
    return true;
  }
  if (pause_rows_menu_ != menu) return false;
  const auto row = Load(base, menu + 64);
  if (row == pause_video_row_ || row == pause_effects_row_) {
    if (Load(base, menu + 112)) OpenFromPause(ctx, base, menu, row == pause_video_row_ ? 21 : 22);
    if (Load(base, menu + 116)) {
      // Back leaves Options through the title's own controller.
      Store(base, menu + 64, pause_video_row_);
      CallGuest(original, ctx, base, menu);
      Store(base, menu + 64, row);
    }
    return true;
  }
  if (row == pause_effects_row_ + 1) {
    Store(base, menu + 64, pause_video_row_); // original Back ordinal
    CallGuest(original, ctx, base, menu);
    Store(base, menu + 64, row);
    return true;
  }
  return false;
}
void NativeSettingsBridge::ObservePauseExit(uint8_t* base) {
  if (!pause_menu_) return;
  const bool live = LivePauseOptions(base, pause_menu_);
  const bool returned = pause_entered_ && Load(base, manager_ + 164) == pause_return_ &&
      Load(base, manager_ + 168) == UINT32_MAX && Load(base, manager_ + 172) == UINT32_MAX;
  if (!live || returned) {
    if (returned) pause_release_menu_ = pause_menu_;
    pause_menu_ = 0; pause_entered_ = false;
    for (auto& page : pages_) if (page.component) Store(base, page.component + 24, 19);
    if (!live && controller_ && !controller_->exit_requested()) controller_->Leave();
  }
}
void NativeSettingsBridge::RevealOptions(PPCContext& ctx, uint8_t* base, uint32_t options) {
  if (!ready(base, manager_) || !options) return;
  const auto video = Load(base, options + 108), effects = Load(base, options + 112), list = Load(base, options + 152);
  if (!video || !effects || !list) return;
  // RefreshList first enables every registered entry, then applies this mask.
  // Setting widget visibility alone is immediately undone by that refresh.
  const auto entries = Load(base, list + 28), count = Load(base, list + 32);
  if (!entries || count > 32) return;
  uint32_t visible = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const auto widget = Load(base, entries + i * 4);
    if (!widget) continue;
    const bool supported = widget == video || widget == effects ||
        (widget != Load(base, options + 116) && widget != Load(base, options + 136) && base[widget + 457]);
    if (supported) visible |= 1u << i;
  }
  Store(base, list + 16, visible);
  CallGuest(api_.set_visible, ctx, base, video, 1);
  CallGuest(api_.set_visible, ctx, base, effects, 1);
  // The console has no PC binding controller at state 20. Keep its working
  // native controls screen and remove the otherwise dangling PC-only row.
  const auto controls = Load(base, options + 116);
  if (controls) CallGuest(api_.set_visible, ctx, base, controls, 0);
  CallGuest(api_.refresh_list, ctx, base, list);
  // Compact only the original OptionsMenu group. The logo/footer are siblings
  // on OptionsPage. TU23 traverses +144 as the current local transform and
  // restores it from +208 after animation, so set both absolute scales once
  // per entry rather than multiplying an already compacted transform.
  const auto root = Load(base, options + 20), menu = Load(base, list);
  std::array<uint8_t, 480> page_identity{}, menu_identity{};
  if (root && menu && root != menu && api_.read_metadata &&
      api_.read_metadata(root, page_identity.data(), uint32_t(page_identity.size())) &&
      Load(page_identity.data(), 0) && Load(page_identity.data(), 476) == options &&
      api_.read_metadata(menu, menu_identity.data(), uint32_t(menu_identity.size())) &&
      Load(menu_identity.data(), 0) && Load(menu_identity.data(), 436) == 2 &&
      CallGuest(api_.find_widget, ctx, base, root, 0x821BF48C) == menu) {
    bool simple = true;
    for (uint32_t matrix : {144u, 208u}) for (uint32_t i = 0; i < 16; ++i) {
      const float value = std::bit_cast<float>(Load(menu_identity.data(), matrix + i * 4));
      if (!std::isfinite(value)) { simple = false; continue; }
      if (i == 0 || i == 5) simple &= std::abs(value - 1.0f) < 0.001f || std::abs(value - 0.85f) < 0.001f;
      else if (i == 10 || i == 15) simple &= std::abs(value - 1.0f) < 0.001f;
      else if (i < 12) simple &= std::abs(value) < 0.001f;
    }
    if (simple) for (uint32_t offset : {144u, 164u, 208u, 228u})
      Store(base, menu + offset, std::bit_cast<uint32_t>(0.85f));
  }
  // Options initialization may restore localized text on the same widgets.
  for (auto widget : {video, effects}) text_cache_.erase(widget);
  Text(ctx, base, video, "Video");
  Text(ctx, base, effects, "Effects");
}
NativeSettingsBridge::Page* NativeSettingsBridge::FindPage(uint32_t component) {
  for (auto& page : pages_) if (page.component && page.component == component) return &page;
  return nullptr;
}
void NativeSettingsBridge::Destroy(PPCContext& ctx, uint8_t*) {
  if (!active_) return;
  if (auto* page = active_->FindPage(ctx.r3.u32)) {
    page->retired = true;
    active_->installed_ = false;
    active_->popup_event_ = 0;
    if (active_->controller_) active_->controller_->Leave();
  }
  // Guest allocation remains valid until title teardown. A later init signal
  // may create a fresh graph, but never reuses a retired native component.
}
void NativeSettingsBridge::AllowSwitch(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 1; }
void NativeSettingsBridge::SetResult(PPCContext& ctx, uint8_t* base) {
  if (!active_ || !active_->ready(base, active_->manager_)) return;
  const auto self = ctx.r3.u32;
  if (self == active_->interface_) Store(base, self + 4, ctx.r4.u32);
  else if (active_->FindPage(self)) Store(base, self + 24, ctx.r4.u32);
}
void NativeSettingsBridge::GetResult(PPCContext& ctx, uint8_t* base) {
  if (!active_ || !active_->ready(base, active_->manager_)) { ctx.r3.u32 = 19; return; }
  const auto self = ctx.r3.u32;
  ctx.r3.u32 = self == active_->interface_ ? active_->return_page_ :
      active_->FindPage(self) ? Load(base, self + 24) : 19;
}
void NativeSettingsBridge::Event(PPCContext& ctx, uint8_t* base) { if (active_) active_->OnEvent(ctx, base); }
void NativeSettingsBridge::Accept(PPCContext& ctx, uint8_t* base) { if (active_) active_->OnAccept(ctx, base); }
void NativeSettingsBridge::Back(PPCContext& ctx, uint8_t* base) { if (active_) active_->OnBack(ctx, base); }
void NativeSettingsBridge::OnEvent(PPCContext& ctx, uint8_t* base) {
  if (!ready(base, manager_)) return;
  const auto self = ctx.r3.u32, event = ctx.r5.u32;
  if (self == interface_) {
    popup_event_ = 0;
    if ((!session_ && extra_ == Extra::kNone) || Load(base, manager_ + 164) != 23 || Load(base, generic_ + 176) != interface_ ||
        ctx.r4.u32 != Load(base, generic_ + 20)) return;
    if (event == 15 || event == 16) popup_event_ = event;
    if (event == 2) {
      if (extra_ != Extra::kNone) PublishExtra(ctx, base);
      else PublishDialog(ctx, base, true);
    }
    return;
  }
  auto* page = FindPage(self);
  if (!page || ctx.r4.u32 != page->root) return;
  if (event == 2) {
    Focus(ctx, base, *page, 0);
    if (pause_menu_) { pause_entered_ = true; Store(base, page->component + 24, pause_return_); }
    else Store(base, page->component + 24, 19);
    return_page_ = uint32_t(page->id);
    if (!session_) {
      session_ = true;
      if (!controller_ || controller_->exit_requested()) {
        controller_ = std::make_unique<FrontendController>(api_.settings(), api_.now);
        controller_->SetTrace(api_.trace);
        controller_->Enter(page->id);
      } else controller_->RebindPage(page->id);
      // A controller carried across failed rollback opens its recovery dialog,
      // not a new transaction. Generation suffixes do not change row semantics.
    } else if (controller_ && controller_->page() != page->id) controller_->RebindPage(page->id);
  } else if (event == 4) {
    ObserveExit(base);
  } else if (controller_ && session_ && Load(base, manager_ + 164) == uint32_t(page->id)) {
    if (event == 16) controller_->Back();
    else if (event == 11 || event == 12 || event == 15) {
      const auto selected = CallGuest(api_.selected_widget, ctx, base, page->root);
      for (size_t i = 0; i < page->rows.size(); ++i) {
        if (selected != page->rows[i] && selected != page->values[i]) continue;
        if (event == 15) controller_->Activate(i);
        else controller_->Adjust(i, event == 11 ? -1 : 1);
        break;
      }
    }
  }
}
void NativeSettingsBridge::Focus(PPCContext& ctx, uint8_t* base, Page& page, size_t row) {
  if (row >= page.rows.size()) return;
  page.selected_row = row;
  CallGuest(api_.select_widget, ctx, base, page.root, page.rows[row], 1);
  if (api_.trace) api_.trace("Native menu focus: page=" + std::to_string(uint32_t(page.id)) +
      " row=" + std::to_string(row) + " selected=" +
      std::to_string(CallGuest(api_.selected_widget, ctx, base, page.root)));
}
void NativeSettingsBridge::FocusPopup(PPCContext& ctx, uint8_t* base, size_t choice) {
  popup_choice_ = choice;
  // SelectWidget expects the page root, whose +484 field is the current
  // selection. GenericMenu is a child widget: its +484 is not that pointer.
  // Passing the child crashes when the selector tries to deselect address 1.
  const auto root = Load(base, generic_ + 28), widget = Load(base, generic_ + (choice ? 44 : 40));
  if (root && widget) CallGuest(api_.select_widget, ctx, base, root, widget, 1);
}
void NativeSettingsBridge::PlaySound(PPCContext& ctx, uint8_t* base, uint32_t sound) {
  if (!api_.play_sound) return;
  if (api_.trace) api_.trace("Native menu sound request: source=bridge ui=" + std::to_string(ui_) +
      " code=" + std::to_string(sound) + " caller=" + std::to_string(uint32_t(ctx.lr)));
  CallGuest(api_.play_sound, ctx, base, ui_, sound);
}
bool NativeSettingsBridge::Input(PPCContext& ctx, uint8_t* base, uint32_t ui,
                                 uint32_t packet, uint32_t directions, uint32_t layer) {
  if (layer != 2 || !packet || ui != ui_ || !ready(base, manager_)) return false;
  const auto current = Load(base, manager_ + 164);
  if (current == 23 && Load(base, generic_ + 176) == interface_ && (session_ || extra_ != Extra::kNone)) {
    const auto event = NativeMenuInputEvent(Load(base, ui + 148), directions, Load(base, packet + 8));
    if (event != 9 && event != 10 && event != 11 && event != 12 && event != 15 && event != 16) return false;
    if (Load(base, manager_ + 168) != UINT32_MAX || Load(base, manager_ + 172) != UINT32_MAX) return true;
    const auto count = extra_ == Extra::kQuit ? 2u : extra_ == Extra::kCredits ? 1u :
        controller_ ? controller_->dialog().buttons.size() : 0u;
    if (event >= 9 && event <= 12) {
      if (count > 1) { FocusPopup(ctx, base, (popup_choice_ + 1) % count);
        PlaySound(ctx, base, 0); }
    } else if (event == 16 || count) {
      // Own the modal input route just as Video/Effects do. The native GSC
      // child may consume Accept before GenericPage receives it (Continue).
      popup_event_ = event;
      CallGuest(event == 15 ? Accept : Back, ctx, base, interface_, uint32_t(popup_choice_));
      PlaySound(ctx, base, event == 16 ? 2 : 1);
    }
    return true;
  }
  Page* page = nullptr;
  for (auto& p : pages_) if (uint32_t(p.id) == current) page = &p;
  if (!page) return false; // GenericPage and all original screens keep their input path.
  const auto buttons = Load(base, packet + 8), forced = Load(base, ui + 148);
  const auto event = NativeMenuInputEvent(forced, directions, buttons);
  if (event != 9 && event != 10 && event != 11 && event != 12 && event != 15 && event != 16) return false;
  // Own the input controller as well as the rendered widgets. The original
  // dispatcher can target a GSC menu child instead of our newly built root.
  // Skipping it for this event also prevents the same press being applied twice.
  if (Load(base, manager_ + 168) != UINT32_MAX || Load(base, manager_ + 172) != UINT32_MAX) return true;
  const auto selected = CallGuest(api_.selected_widget, ctx, base, page->root);
  size_t row = page->selected_row;
  for (size_t i = 0; i < page->rows.size(); ++i)
    if (selected == page->rows[i] || selected == page->values[i]) row = i;
  if (api_.trace) api_.trace("Native menu input: page=" + std::to_string(current) +
      " event=" + std::to_string(event) + " row=" + std::to_string(row) +
      " session=" + std::to_string(session_));
  if (event == 9 || event == 10) {
    Focus(ctx, base, *page, (row + page->rows.size() + (event == 9 ? -1 : 1)) % page->rows.size());
    // The selection-change event animates focus; V30 owner logs confirm
    // that it does not request a move sound for these owned pages.
    PlaySound(ctx, base, 0);
  } else if (controller_ && session_) {
    const bool handled = event == 16 ? controller_->Back() : event == 15 ? controller_->Activate(row) :
        controller_->Adjust(row, event == 11 ? -1 : 1);
    if (handled) PlaySound(ctx, base, event == 16 ? 2 : 1);
  }
  return true;
}
void NativeSettingsBridge::OnAccept(PPCContext& ctx, uint8_t* base) {
  const auto event = std::exchange(popup_event_, 0u);
  if (event == 15 && extra_ != Extra::kNone && ready(base, manager_) && ctx.r3.u32 == interface_) {
    const auto choice = ctx.r4.u32;
    if (extra_ == Extra::kQuit && choice == 0) {
      extra_ = Extra::kNone;
      if (api_.quit) api_.quit();
    } else if (extra_ == Extra::kCredits && choice == 0 && ++credit_page_ < std::size(kCredits)) {
      CallGuest(api_.reset_popup, ctx, base, generic_);
      PublishExtra(ctx, base);
      CallGuest(api_.publish_popup, ctx, base, generic_);
    } else {
      extra_ = Extra::kNone;
      Request(ctx, base, extra_return_);
    }
    return;
  }
  if (event == 15 && controller_ && session_ && ready(base, manager_) && ctx.r3.u32 == interface_)
    controller_->Choose(ctx.r4.u32);
}
void NativeSettingsBridge::OnBack(PPCContext& ctx, uint8_t* base) {
  const auto event = std::exchange(popup_event_, 0u);
  if (event == 16 && extra_ != Extra::kNone && ready(base, manager_) && ctx.r3.u32 == interface_) {
    extra_ = Extra::kNone;
    Request(ctx, base, extra_return_);
    return;
  }
  if (event == 16 && controller_ && session_ && ready(base, manager_) && ctx.r3.u32 == interface_)
    controller_->Back();
}
void NativeSettingsBridge::ObserveExit(uint8_t* base) {
  if (!controller_ || !session_) return;
  const auto current = Load(base, manager_ + 164), pending = Load(base, manager_ + 168), deferred = Load(base, manager_ + 172);
  if (!IsSettings(current) || (pending != UINT32_MAX && !IsSettings(pending)) ||
      (deferred != UINT32_MAX && !IsSettings(deferred))) {
    if (controller_->exit_requested()) session_ = false;
    else controller_->Leave();
  }
}
bool NativeSettingsBridge::Request(PPCContext& ctx, uint8_t* base, uint32_t id) {
  if (Load(base, manager_ + 164) == id) return true;
  const auto pending = Load(base, manager_ + 168), deferred = Load(base, manager_ + 172);
  if (pending != UINT32_MAX || deferred != UINT32_MAX) return pending == id || deferred == id;
  CallGuest(api_.request_page, ctx, base, manager_, id);
  return Load(base, manager_ + 168) == id || Load(base, manager_ + 172) == id;
}
bool NativeSettingsBridge::PublishDialog(PPCContext& ctx, uint8_t* base, bool force) {
  if (!controller_ || Load(base, generic_ + 176) != interface_) return false;
  const auto view = controller_->dialog();
  if (view.kind == FrontendDialog::kNone) return false;
  if (!force && view.kind == published_dialog_.kind && view.message == published_dialog_.message && view.buttons == published_dialog_.buttons) return true;
  if (api_.trace && !controller_->reply().error.empty())
    api_.trace("Native settings dialog error: " + controller_->reply().error);
  const auto message = Literal(base, "NATIVE_FRONTEND_MESSAGE", view.message);
  const auto a = Literal(base, "NATIVE_FRONTEND_BUTTON_A", view.buttons.empty() ? "" : view.buttons[0]);
  const auto b = Literal(base, "NATIVE_FRONTEND_BUTTON_B", view.buttons.size() == 2 ? view.buttons[1] : "");
  if (view.buttons.size() > 2 || !message || !a || !b)
    return Fail("Could not prepare native settings dialog text");
  Store(base, labels_, a); Store(base, labels_ + 4, b);
  // GenericPage's publish only shows controls; switching to a buttonless wait
  // must first hide the old menu. The entry event already resets it. Preserve
  // current selection for ordinary same-button countdown/text updates.
  if (!force && view.buttons.size() != published_dialog_.buttons.size()) {
    popup_event_ = 0;
    CallGuest(api_.reset_popup, ctx, base, generic_);
  }
  native_frontend::PopupRequest request{generic_, message,
      view.buttons.empty() ? native_frontend::PopupButtons::kNone :
          view.buttons.size() == 2 ? native_frontend::PopupButtons::kAcceptCancel : native_frontend::PopupButtons::kAccept,
      view.buttons.empty() ? 0u : 3u, uint32_t(view.buttons.size()), labels_, true};
  if (!native_frontend::ConfigurePopup(api_.configure_popup, ctx, base, request)) return false;
  if (!view.buttons.empty() && (force || view.buttons.size() != published_dialog_.buttons.size())) FocusPopup(ctx, base, 0);
  published_dialog_ = view;
  // Entry event 3 publishes the initial dialog. Later countdown/error updates
  // need an explicit native publish while the GenericPage is already active.
  if (!force && Load(base, manager_ + 164) == 23) CallGuest(api_.publish_popup, ctx, base, generic_);
  return true;
}
bool NativeSettingsBridge::ShowMenuExtra(PPCContext& ctx, uint8_t* base, bool quit) {
  const auto manager = Load(base, kManager);
  if (!manager || Load(base, manager + 164) != 17) return false;
  if (pending_extra_ == Extra::kNone) {
    pending_extra_ = quit ? Extra::kQuit : Extra::kCredits;
    pending_extra_return_ = Load(base, manager + 164);
  }
  if (api_.trace) api_.trace(std::string("Native main menu request: ") + (quit ? "quit" : "credits"));
  if (LiveExtraSource(ctx, base, manager)) TryShowMenuExtra(ctx, base, pending_extra_ == Extra::kQuit);
  return true;
}
bool NativeSettingsBridge::TryShowMenuExtra(PPCContext& ctx, uint8_t* base, bool quit) {
  // This is an explicit native main-menu action, rather than loading-frame
  // maintenance. It may safely restore the popup bindings after a scene change.
  const auto manager = Load(base, kManager);
  if (!ready(base, manager)) {
    if (!pending_manager_ || pending_manager_ != manager) BeginSetup(manager);
    if (!ContinueSetup(ctx, base)) return false;
  }
  ObserveExit(base);
  if (!ready(base, manager_) || !LiveGeneric(base, generic_) || session_ || extra_ != Extra::kNone ||
      Load(base, manager_ + 168) != UINT32_MAX || Load(base, manager_ + 172) != UINT32_MAX ||
      (quit && !api_.quit)) return false;
  extra_return_ = Load(base, manager_ + 164);
  if (IsSettings(extra_return_)) return false;
  return_page_ = extra_return_;
  credit_page_ = 0;
  extra_ = quit ? Extra::kQuit : Extra::kCredits;
  if (Request(ctx, base, 23)) {
    pending_extra_ = Extra::kNone;
    auto& name = extra_entry_names_[quit ? 1 : 0];
    if (!name) name = String(base, quit ? "xml_switchprofile" : "xml_help");
    extra_focus_name_ = name;
    extra_focus_pending_ = name != 0;
    if (api_.trace) api_.trace(std::string("Native main menu dialog opened: ") + (quit ? "quit" : "credits"));
    return true;
  }
  extra_ = Extra::kNone;
  return false;
}
bool NativeSettingsBridge::PublishExtra(PPCContext& ctx, uint8_t* base) {
  if (extra_ == Extra::kNone || Load(base, generic_ + 176) != interface_) return false;
  const bool quit = extra_ == Extra::kQuit;
  const auto message = quit ? std::string_view("Quit to desktop") : kCredits[credit_page_];
  const auto message_key = Literal(base, "NATIVE_FRONTEND_MESSAGE", message);
  const auto a = Literal(base, "NATIVE_FRONTEND_BUTTON_A", quit ? "Quit Game" :
      credit_page_ + 1 < std::size(kCredits) ? "Next" : "Close");
  const auto b = Literal(base, "NATIVE_FRONTEND_BUTTON_B", "Cancel");
  if (!message_key || !a || !b) return false;
  Store(base, labels_, a); Store(base, labels_ + 4, b);
  const bool configured = native_frontend::ConfigurePopup(api_.configure_popup, ctx, base,
      native_frontend::PopupRequest{generic_, message_key,
        quit ? native_frontend::PopupButtons::kAcceptCancel : native_frontend::PopupButtons::kAccept,
        3, quit ? 2u : 1u, labels_, true});
  // Match the title's first visible row. A hidden Cancel default makes the
  // first Down press select Quit again instead of moving to Cancel.
  if (configured) FocusPopup(ctx, base, 0);
  return configured;
}
void NativeSettingsBridge::SyncExtra(PPCContext&, uint8_t* base) {
  if (extra_ == Extra::kNone) return;
  const auto current = Load(base, manager_ + 164), pending = Load(base, manager_ + 168);
  if (current != 23 && pending != 23 && current != extra_return_) extra_ = Extra::kNone;
}
void NativeSettingsBridge::RestoreExtraFocus(PPCContext& ctx, uint8_t* base) {
  if (!extra_focus_pending_ || extra_ != Extra::kNone) return;
  const auto current = Load(base, manager_ + 164), pending = Load(base, manager_ + 168),
      deferred = Load(base, manager_ + 172);
  if (current == 23 || pending == 23 || deferred == 23) return;
  if (current != extra_return_) { extra_focus_pending_ = false; return; }
  if (pending != UINT32_MAX || deferred != UINT32_MAX) return;
  // The original page's enter handler resets its selected row. Restore after
  // that handler finishes, resolving the live named row rather than an old
  // widget pointer. SelectWidget requires the root, not its GenericMenu child.
  const auto root = LiveMainMenuRoot(ctx, base, manager_, extra_return_);
  if (!root) return;
  const auto widget = CallGuest(api_.find_widget, ctx, base, root, extra_focus_name_);
  if (widget) {
    CallGuest(api_.select_widget, ctx, base, root, widget, 1);
    extra_focus_pending_ = false;
  }
}
void NativeSettingsBridge::Sync(PPCContext& ctx, uint8_t* base) {
  if (!controller_ || !session_) return;
  const auto current = Load(base, manager_ + 164);
  if (!IsSettings(current)) return;
  if (controller_->exit_requested()) { Request(ctx, base, pause_menu_ ? pause_return_ : 19u); return; }
  for (auto& page : pages_) {
    Text(ctx, base, page.title, page.layout.title + (controller_->busy() ? " - Please wait..." : ""));
    const bool visible = current == uint32_t(page.id);
    const auto selected = visible ? CallGuest(api_.selected_widget, ctx, base, page.root) : 0;
    const bool bright = (std::chrono::duration_cast<std::chrono::milliseconds>(api_.now().time_since_epoch()).count() / 500) % 2 == 0;
    for (size_t i = 0; i < page.layout.rows.size(); ++i) {
      const auto& row = page.layout.rows[i];
      if (visible) {
        const bool enabled = row.action != RowAction::kSetting || FrontendSettingEnabled(row.key, controller_->reply().values);
        const bool focused = selected == page.rows[i] || selected == page.values[i];
        Tint(base, page.rows[i], enabled, focused, bright);
        Tint(base, page.values[i], enabled, focused, bright);
      }
      if (row.action != RowAction::kSetting) continue;
      const auto it = controller_->reply().values.find(row.key);
      Text(ctx, base, page.values[i], it == controller_->reply().values.end() ? "..." : DisplaySettingValue(row.key, it->second));
    }
  }
  if (controller_->dialog().kind != FrontendDialog::kNone) {
    Request(ctx, base, 23);
    if (current == 23) PublishDialog(ctx, base);
  } else if (current == 23) {
    published_dialog_ = {};
    Request(ctx, base, return_page_);
  }
}
void NativeSettingsBridge::BeforeFrame(PPCContext& ctx, uint8_t* base, uint32_t manager) {
  TraceTransition(base, manager);
  ObservePauseExit(base);
  if (pending_extra_ != Extra::kNone) {
    const auto current = Load(base, manager + 164);
    if (LiveExtraSource(ctx, base, manager)) TryShowMenuExtra(ctx, base, pending_extra_ == Extra::kQuit);
    else if (current != pending_extra_return_ && Load(base, manager + 168) == UINT32_MAX &&
             Load(base, manager + 172) == UINT32_MAX) pending_extra_ = Extra::kNone;
  }
  if (!ready(base, manager)) {
    SuspendRequest(base, manager);
    if (installed_ || (!pending_manager_ && manager == manager_ && binding_.Owns(base, manager))) BeginSetup(manager);
    // An explicit manager reinitialization can be waiting for preview
    // cancellation while GenericPage is current. Leave that page without
    // loading new layouts; its native Options entry will authorize the retry.
    const auto array = Load(base, manager + 140);
    if (setup_requested_ && pending_manager_ == manager && array &&
        LiveOptions(base, manager, Load(base, array + 19 * 4))) ReturnToOptions(ctx, base, manager);
    const bool options_live = LiveOptions(base, manager) || (pause_menu_ && LivePauseOptions(base, pause_menu_));
    if (!options_live) setup_requested_ = false;
    // Scene loading invalidates dynamic roots while holding title resource
    // locks. Never clone GSC/XML pages here merely because roots disappeared.
    // Rebuild only for a real Options entry or a suspended Video/Effects click.
    if (pending_manager_ && options_live && (setup_requested_ || resume_page_ != UINT32_MAX)) {
      if (pending_manager_ != manager) BeginSetup(manager);
      ContinueSetup(ctx, base);
    } else if (controller_) controller_->Poll();
    if (!ready(base, manager)) return;
  }
  ObserveExit(base);
  SyncExtra(ctx, base);
  if (controller_) controller_->Poll();
  Sync(ctx, base);
}
void NativeSettingsBridge::AfterFrame(PPCContext& ctx, uint8_t* base, uint32_t manager) {
  TraceTransition(base, manager);
  if (!ready(base, manager)) return;
  ObserveExit(base);
  Sync(ctx, base);
  RestoreExtraFocus(ctx, base);
}
void NativeSettingsBridge::TraceTransition(uint8_t* base, uint32_t manager) {
  if (!manager || !api_.trace) return;
  const std::array<uint32_t, 4> state{manager, Load(base, manager + 164), Load(base, manager + 168), Load(base, manager + 172)};
  if (state == last_transition_) return;
  last_transition_ = state;
  api_.trace("Native frontend transition: manager=" + std::to_string(manager) +
      " current=" + std::to_string(state[1]) + " pending=" + std::to_string(state[2]) + " deferred=" + std::to_string(state[3]));
}

}  // namespace legodimensions::settings
