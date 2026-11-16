#pragma once

#include "native_frontend_guest.h"

namespace legodimensions::native_frontend {

enum class PageLoadStatus { kInvalid, kExisting, kCreated, kNotCreated };
struct PageLoadResult {
  PageLoadStatus status = PageLoadStatus::kInvalid;
  uint32_t page = 0;
};
struct PageLayoutRequest {
  uint32_t registry = 0;  // Object at TU23 global 0x849E220C, not frontend manager.
  uint32_t xml = 0;       // Guest UTF-8 XML; caller owns it during parsing.
  uint32_t name = 0;      // Guest name of the top-level <page>.
};

// Uses the title's widget-building pass (sub_82F0FAC0), not the initial
// resource/version-only pass. Native widget factories own constructed pages.
// Does not register a frontend controller/ID or change visibility/input.
// Existing names are reported without modifying their controls. The caller
// must distinguish its own cached page from another module's existing page.
template <typename Load, typename Find>
PageLoadResult LoadPageLayout(Load load, Find find, PPCContext& ctx, uint8_t* base,
                             const PageLayoutRequest& request) {
  if (!request.registry || request.registry > 0xFFFFFFE4u || !request.xml || !request.name)
    return {};
  const uint32_t list = request.registry + 24;
  if (const auto existing = CallGuest(find, ctx, base, list, request.name))
    return {PageLoadStatus::kExisting, existing};
  // The parser's root <page> handler creates the parent context. A filename
  // of zero selects the supplied buffer and avoids the game's file IO path.
  CallGuest(load, ctx, base, 0, 0, request.xml);
  const auto page = CallGuest(find, ctx, base, list, request.name);
  return {page ? PageLoadStatus::kCreated : PageLoadStatus::kNotCreated, page};
}

}  // namespace legodimensions::native_frontend
