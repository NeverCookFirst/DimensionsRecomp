#ifdef LEGODIMENSIONS_NATIVE_GPU

#include "settings_native_bridge.h"
#include "settings_native_hooks.h"
#include "main_menu_extras.h"
#include "gpu_native/guest_metadata.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <atomic>

REXCVAR_DEFINE_BOOL(native_settings_frontend, false, "Graphics",
    "Enable native Video/Effects settings with frame/presentation/window acknowledgement and native confirmation dialogs. Experimental owner test option.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_82FCF858);
REX_EXTERN(__imp__sub_82FB8628);
REX_EXTERN(__imp__sub_82FD9F40);
REX_EXTERN(__imp__sub_839D08A0);
REX_EXTERN(__imp__sub_839D4020);
REX_EXTERN(__imp__sub_839C4D08);
REX_EXTERN(__imp__sub_82FEA350);
REX_EXTERN(__imp__sub_82F0DF30);
REX_EXTERN(__imp__sub_82F0F8D0);
REX_EXTERN(__imp__sub_82A435C8);
REX_EXTERN(sub_82EFEEB8);
REX_EXTERN(sub_82EFCB70);
REX_EXTERN(__imp__sub_82EFCB70);
REX_EXTERN(sub_82F02270);
REX_EXTERN(sub_82F0FAC0);
REX_EXTERN(sub_82EF9730);
REX_EXTERN(sub_82F0CF40);
REX_EXTERN(sub_82EFB478);
REX_EXTERN(sub_82EFB1E8);
REX_EXTERN(sub_82EF8D80);
REX_EXTERN(sub_82EFEFA0);
REX_EXTERN(__imp__sub_82EFEFA0);
REX_EXTERN(sub_82F0FF38);
REX_EXTERN(sub_82EF9560);
REX_EXTERN(sub_82FBDFF8);
REX_EXTERN(sub_82FBDF58);
REX_EXTERN(sub_82FD0508);
REX_EXTERN(sub_82FD8C30);
REX_EXTERN(sub_82EF51B0);
REX_EXTERN(sub_82F0F958);
REX_EXTERN(sub_82EFD540);
REX_EXTERN(sub_82F0E608);
REX_EXTERN(sub_82EFE8F0);
REX_EXTERN(sub_82EFEFA0);

namespace legodimensions::settings {
namespace {
std::unique_ptr<NativeSettingsBridge> bridge;
std::atomic<NativeSettingsBridge*> literal_bridge{nullptr};
thread_local uint32_t drawing_pause_options = 0;
bool Enabled() {
  // Do not enable/disable native page ownership halfway through a session.
  static const bool enabled = REXCVAR_GET(native_settings_frontend);
  return enabled;
}
void RefreshOptionsVisibility(PPCContext& ctx, uint8_t* base) {
  const auto list = ctx.r3.u32;
  // Full RefreshList also clones every dynamic entry (82F02C38). The title
  // performs that once at initialization. Repeating it after each Options
  // entry leaves extra text elements behind. Only refresh visibility/layout.
  native_frontend::CallGuest(sub_82EFD540, ctx, base, list);
  native_frontend::CallGuest(sub_82F0E608, ctx, base, list);
}
void SetOwnedWidgetText(PPCContext& ctx, uint8_t* base) {
  const auto widget = ctx.r3.u32, key = ctx.r4.u32;
  auto* value = literal_bridge.load(std::memory_order_acquire);
  const auto text = value ? value->LookupLiteral(base, key) : 0;
  if (!text) { __imp__sub_82EFEFA0(ctx, base); return; }
  // The native setter owns both wrapper and child updates. Owned retained
  // value pointers also resolve through the localization hook, so its own
  // child path displays the literal without a second child setter call.
  native_frontend::CallGuest(__imp__sub_82EFEFA0, ctx, base, widget, text);
}
NativeSettingsBridge* GetBridge(uint8_t* base) {
  if (!Enabled()) return nullptr;
  if (bridge) return bridge.get();
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !kernel->memory() || !kernel->function_dispatcher() ||
      kernel->memory()->virtual_membase() != base) return nullptr;
  auto* memory = kernel->memory();
  auto* dispatcher = kernel->function_dispatcher();
  NativeBridgeApi api;
  api.allocate = [memory](uint32_t bytes) { return memory->SystemHeapAlloc(bytes); };
  api.read_metadata = [memory](uint32_t address, void* data, uint32_t bytes) {
    return gpu_native::CopyNativeGuestMetadata(*memory, address, data, bytes);
  };
  api.thunk = [dispatcher](PPCFunc* function) { return dispatcher->AllocateThunk(function, 0); };
  api.settings = AcquireNativeSettingsService;
  api.quit = legodimensions::main_menu::RequestNativeQuit;
  api.trace = [](std::string message) {
    static unsigned count = 0;
    const bool input = message.starts_with("Native menu input:") || message.starts_with("Native menu focus:");
    if (!input || count++ < 160) REXKRNL_INFO("{}", message);
  };
  api.load_layout = sub_82F0FAC0;
  api.find_page = sub_82EF9730;
  api.find_widget = sub_82F0CF40;
  api.construct_component = sub_82EFB478;
  api.add_component = sub_82EFB1E8;
  api.find_component = sub_82EF8D80;
  api.set_text = sub_82EFEFA0;
  api.page_event = sub_82F0FF38;
  api.selected_widget = sub_82EF9560;
  api.configure_popup = sub_82FBDFF8;
  api.reset_popup = sub_82FBDF58;
  api.publish_popup = sub_82FD0508;
  api.request_page = sub_82FD8C30;
  api.set_visible = sub_82EF51B0;
  api.refresh_list = RefreshOptionsVisibility;
  api.construct_menu = sub_82F02270;
  api.select_widget = sub_82EFEEB8;
  api.play_sound = sub_82EFCB70;
  bridge = std::make_unique<NativeSettingsBridge>(std::move(api));
  literal_bridge.store(bridge.get(), std::memory_order_release);
  return bridge.get();
}
}  // namespace
bool ShowNativeMenuExtra(PPCContext& ctx, uint8_t* base, bool quit) {
  if (!Enabled()) return false;
  // A second Accept during the transition must not open the legacy overlay
  // above the native dialog and block the guest's own input.
  if (auto* value = GetBridge(base)) value->ShowMenuExtra(ctx, base, quit);
  return true;
}
uint32_t LookupNativeLiteral(uint8_t* base, uint32_t key, uint32_t* canonical_key) {
  auto* value = literal_bridge.load(std::memory_order_acquire);
  return value ? value->LookupLiteral(base, key, canonical_key) : 0;
}
}  // namespace legodimensions::settings

REX_HOOK_RAW(sub_82FCF858) {
  const auto manager = ctx.r3.u32;
  __imp__sub_82FCF858(ctx, base);
  if (auto* value = legodimensions::settings::GetBridge(base)) {
    if (!value->Initialize(ctx, base, manager, true))
      REXKRNL_WARN("Native settings frontend remains unavailable: {}", value->error());
  }
}

REX_HOOK_RAW(sub_82FB8628) {
  const auto options = ctx.r3.u32;
  __imp__sub_82FB8628(ctx, base);
  if (auto* value = legodimensions::settings::GetBridge(base)) value->RevealOptions(ctx, base, options);
}

REX_HOOK_RAW(sub_82FD9F40) {
  const auto options = ctx.r3.u32;
  __imp__sub_82FD9F40(ctx, base);
  if (auto* value = legodimensions::settings::GetBridge(base)) value->OptionsEntering(ctx, base, options);
}

REX_HOOK_RAW(sub_839D08A0) {
  const auto menu = ctx.r3.u32;
  auto* value = legodimensions::settings::GetBridge(base);
  if (value && value->PauseOverlayActive(base, menu)) return;
  const auto previous = legodimensions::settings::drawing_pause_options;
  legodimensions::settings::drawing_pause_options = menu;
  if (!value || !value->RenderPauseOptions(ctx, base, menu, __imp__sub_839D08A0))
    __imp__sub_839D08A0(ctx, base);
  legodimensions::settings::drawing_pause_options = previous;
}

REX_HOOK_RAW(sub_839C4D08) {
  // Exact TU23 call site for the final Back row of gameplay Options.
  if (ctx.lr == 0x839D11B0 && ctx.r3.u32 == legodimensions::settings::drawing_pause_options)
    if (auto* value = legodimensions::settings::GetBridge(base))
      value->DrawPauseEntries(ctx, base, ctx.r3.u32, __imp__sub_839C4D08);
  __imp__sub_839C4D08(ctx, base);
}

REX_HOOK_RAW(sub_839D4020) {
  if (auto* value = legodimensions::settings::GetBridge(base))
    if (value->UpdatePauseOptions(ctx, base, ctx.r3.u32, __imp__sub_839D4020)) return;
  __imp__sub_839D4020(ctx, base);
}

REX_HOOK_RAW(sub_82FEA350) {
  const auto manager = ctx.r3.u32;
  auto* value = legodimensions::settings::GetBridge(base);
  if (value) value->BeforeFrame(ctx, base, manager);
  __imp__sub_82FEA350(ctx, base);
  if (value) value->AfterFrame(ctx, base, manager);
}

REX_HOOK_RAW(sub_82F0DF30) {
  const auto node = ctx.r3.u32, parent = ctx.r4.u32;
  __imp__sub_82F0DF30(ctx, base);
  if (!ctx.r3.u32) {
    if (auto* value = legodimensions::settings::GetBridge(base))
      ctx.r3.u32 = value->CompleteMenu(ctx, base, node, parent);
  }
}

REX_HOOK_RAW(sub_82F0F8D0) {
  if (auto* value = legodimensions::settings::GetBridge(base))
    if (value->Input(ctx, base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32)) return;
  __imp__sub_82F0F8D0(ctx, base);
}

REX_HOOK_RAW(sub_82EFCB70) {
  // Observe both native callers and bridge requests. No time debounce or
  // input/sound suppression: first identify the second real request path.
  if (legodimensions::settings::Enabled()) {
    static std::atomic<uint32_t> sequence{0};
    const auto serial=sequence.fetch_add(1,std::memory_order_relaxed);
    if (serial<256)
      REXKRNL_INFO("Native UI sound: seq={} ui={:08X} code={} caller={:08X}",
          serial,ctx.r3.u32,ctx.r4.u32,uint32_t(ctx.lr));
  }
  __imp__sub_82EFCB70(ctx, base);
}

REX_HOOK_RAW(sub_82A435C8) {
  const auto key = ctx.r3.u32, metadata = ctx.r4.u32;
  uint32_t canonical_key = 0;
  if (const auto text = legodimensions::settings::LookupNativeLiteral(base, key, &canonical_key)) {
    if (metadata) legodimensions::native_frontend::component_detail::Store(base, metadata, canonical_key);
    ctx.r3.u32 = text;
    return;
  }
  __imp__sub_82A435C8(ctx, base);
}

REX_HOOK_RAW(sub_82EFEFA0) {
  // GenericPage also sends our owned button keys through this setter.
  // Apply the same contract correction to those calls, while all ordinary
  // title text retains the exact original path.
  legodimensions::settings::SetOwnedWidgetText(ctx, base);
}

#endif  // LEGODIMENSIONS_NATIVE_GPU
