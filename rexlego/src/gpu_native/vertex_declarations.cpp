#include "gpu_native/vertex_declarations.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <plume_render_interface.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <xxhash.h>

#include "gpu_native/d3d.h"
#include "gpu_native/device.h"

namespace legodimensions::gpu_native {
namespace {

constexpr u32 kDeclarationFlag = 0x00100005;
constexpr u32 kMaxElements = 64;

enum class DeclUsage : u8 {
  kPosition = 0,
  kBlendWeight = 1,
  kBlendIndices = 2,
  kNormal = 3,
  kPSize = 4,
  kTexCoord = 5,
  kTangent = 6,
  kBinormal = 7,
  kTessFactor = 8,
  kPositionT = 9,
  kColor = 10,
  kFog = 11,
  kDepth = 12,
  kSample = 13,
};

struct GuestVertexElement {
  be_u16 stream;
  be_u16 offset;
  be_u32 type;
  u8 method;
  u8 usage;
  u8 usage_index;
  u8 padding;
};
static_assert(sizeof(GuestVertexElement) == 12);

struct VertexDeclarationResource {
  u32 guest_address = 0;
  u64 content_hash = 0;
  bool owns_guest_memory = true;
  std::vector<plume::RenderInputElement> inputs;
  bool supported = true;
  u32 swapped_texcoords = 0;
  u32 swapped_normals = 0;
  u32 swapped_binormals = 0;
  u32 swapped_tangents = 0;
  u32 swapped_blend_weights = 0;
  u32 swapped_positions = 0;
  u32 sint_texcoords = 0;
  bool has_r11g11b10_normal = false;
  u64 reversed_byte_elements = 0;
};

std::mutex g_declarations_mutex;
std::unordered_map<u32, std::shared_ptr<VertexDeclarationResource>>
    g_declarations;

const char* UsageName(DeclUsage usage) {
  switch (usage) {
    case DeclUsage::kPosition: return "POSITION";
    case DeclUsage::kBlendWeight: return "BLENDWEIGHT";
    case DeclUsage::kBlendIndices: return "BLENDINDICES";
    case DeclUsage::kNormal: return "NORMAL";
    case DeclUsage::kPSize: return "PSIZE";
    case DeclUsage::kTexCoord: return "TEXCOORD";
    case DeclUsage::kTangent: return "TANGENT";
    case DeclUsage::kBinormal: return "BINORMAL";
    case DeclUsage::kTessFactor: return "TESSFACTOR";
    case DeclUsage::kPositionT: return "POSITIONT";
    case DeclUsage::kColor: return "COLOR";
    case DeclUsage::kFog: return "FOG";
    case DeclUsage::kDepth: return "DEPTH";
    case DeclUsage::kSample: return "SAMPLE";
  }
  return "UNKNOWN";
}

u32 InputLocation(DeclUsage usage, u32 index) {
  if (usage == DeclUsage::kPosition && index <= 4) return index;
  if (usage == DeclUsage::kNormal && index == 0) return 5;
  if (usage == DeclUsage::kTangent && index == 0) return 6;
  if (usage == DeclUsage::kTexCoord && index <= 2) return 7 + index;
  if (usage == DeclUsage::kColor && index == 0) return 10;
  return ~u32{0};
}

plume::RenderFormat ConvertDeclFormat(u32 type) {
  using RF = plume::RenderFormat;
  switch (type) {
    case 0x002C83A4: return RF::R32_FLOAT;
    case 0x002C23A5: return RF::R32G32_FLOAT;
    case 0x002A23B9: return RF::R32G32B32_FLOAT;
    case 0x001A23A6: return RF::R32G32B32A32_FLOAT;
    case 0x00182886: return RF::B8G8R8A8_UNORM;
    case 0x001A2286:
    case 0x001A2386: return RF::R8G8B8A8_UINT;
    case 0x002C2359: return RF::R16G16_SINT;
    case 0x001A235A: return RF::R16G16B16A16_SNORM;
    case 0x001A225A: return RF::R16G16B16A16_SINT;
    case 0x001A2086:
    case 0x001A2186: return RF::R8G8B8A8_UNORM;
    // TT UBYTE4[N] with WZYX component swizzle (bits 10..21 = 0x053).
    // Sign bit 8 is CLEAR; bit 9 selects integer vs normalized conversion.
    // Apply the component reversal to the host vertex upload, separately
    // from the DWORD endian swap used for guest floating-point attributes.
    case 0x00014C86: return RF::R8G8B8A8_UNORM;
    case 0x00014E86: return RF::R8G8B8A8_UINT;
    case 0x002C2159: return RF::R16G16_SNORM;
    case 0x001A215A: return RF::R16G16B16A16_SNORM;
    case 0x002C2059: return RF::R16G16_UNORM;
    case 0x001A205A: return RF::R16G16B16A16_UNORM;
    case 0x002C82A1: return RF::R32_UINT;
    case 0x002A2190:
    case 0x002A2390: return RF::R32_UINT;
    case 0x002C235F: return RF::R16G16_FLOAT;
    case 0x001A2360: return RF::R16G16B16A16_FLOAT;
    default: return RF::UNKNOWN;
  }
}

bool NeedsPairSwap(u32 type) {
  switch (type) {
    case 0x002C2359:
    case 0x001A235A:
    case 0x002C2159:
    case 0x001A215A:
    case 0x002C2059:
    case 0x001A205A:
    case 0x002C235F:
    case 0x001A2360:
      return true;
    default:
      return false;
  }
}

std::shared_ptr<VertexDeclarationResource> FindDeclaration(u32 address) {
  std::lock_guard lock(g_declarations_mutex);
  const auto it = g_declarations.find(address);
  return it == g_declarations.end() ? nullptr : it->second;
}

D3DResource* GuestHeader(u32 address) {
  return REX_KERNEL_MEMORY()->TranslateVirtual<D3DResource*>(address);
}

}  // namespace

static u32 BuildVertexDeclaration(u32 elements_address, u32 existing_address = 0,
                                  u32 existing_count = 0) {
  if (!elements_address) {
    return 0;
  }
  const auto* elements =
      REX_KERNEL_MEMORY()->TranslateVirtual<const GuestVertexElement*>(
          elements_address);
  u32 count = existing_count;
  while (!existing_address && count < kMaxElements && static_cast<u16>(elements[count].stream) != 0xFF &&
         static_cast<u32>(elements[count].type) != 0xFFFFFFFFu) {
    ++count;
  }
  if (count >= kMaxElements) {
    REXLOG_ERROR("Native GPU: unterminated vertex declaration");
    return 0;
  }

  auto resource = std::make_shared<VertexDeclarationResource>();
  resource->content_hash = XXH3_64bits(elements, count * sizeof(GuestVertexElement));
  resource->owns_guest_memory = existing_address == 0;
  resource->inputs.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    const auto& element = elements[i];
    const auto usage = static_cast<DeclUsage>(element.usage);
    plume::RenderInputElement input;
    input.semanticName = UsageName(usage);
    input.semanticIndex = element.usage_index;
    input.location = InputLocation(usage, element.usage_index);
    input.format = ConvertDeclFormat(element.type);
    input.slotIndex = element.stream;
    input.alignedByteOffset = element.offset;
    if (u32(element.type) == 0x00014C86 || u32(element.type) == 0x00014E86)
      resource->reversed_byte_elements |= u64{1} << i;
    if (input.format == plume::RenderFormat::UNKNOWN) {
      resource->supported = false;
      REXLOG_WARN("Native GPU: unsupported vertex element usage={} index={} "
                  "type=0x{:08X}", element.usage, element.usage_index,
                  static_cast<u32>(element.type));
    }
    const u32 usage_index_bit = 1u << std::min<u32>(element.usage_index, 31);
    if (NeedsPairSwap(element.type)) {
      switch (usage) {
        case DeclUsage::kPosition:
          resource->swapped_positions |= usage_index_bit;
          break;
        case DeclUsage::kNormal:
          resource->swapped_normals |= usage_index_bit;
          break;
        case DeclUsage::kBinormal:
          resource->swapped_binormals |= usage_index_bit;
          break;
        case DeclUsage::kTangent:
          resource->swapped_tangents |= usage_index_bit;
          break;
        case DeclUsage::kBlendWeight:
          resource->swapped_blend_weights |= usage_index_bit;
          break;
        case DeclUsage::kTexCoord:
          resource->swapped_texcoords |= usage_index_bit;
          break;
        default:
          break;
      }
    }
    if ((usage == DeclUsage::kNormal || usage == DeclUsage::kTangent ||
         usage == DeclUsage::kBinormal) && element.type == 0x002A23B9) {
      input.format = plume::RenderFormat::R32G32B32_UINT;
    }
    if ((usage == DeclUsage::kNormal || usage == DeclUsage::kTangent ||
         usage == DeclUsage::kBinormal) &&
        (static_cast<u32>(element.type) == 0x002A2190 ||
         static_cast<u32>(element.type) == 0x002A2390)) {
      resource->has_r11g11b10_normal = true;
    }
    if (usage == DeclUsage::kTexCoord &&
        (static_cast<u32>(element.type) == 0x002C2359 ||
         static_cast<u32>(element.type) == 0x001A235A)) {
      input.format = static_cast<u32>(element.type) == 0x002C2359
                         ? plume::RenderFormat::R16G16_UINT
                         : plume::RenderFormat::R16G16B16A16_UINT;
      resource->sint_texcoords |= usage_index_bit;
    }
    resource->inputs.push_back(input);
  }

  const auto add_synthetic = [&](DeclUsage usage, u32 index, u32 location) {
    const char* name = UsageName(usage);
    for (const auto& input : resource->inputs) {
      if (input.semanticIndex == index &&
          std::strcmp(input.semanticName, name) == 0) {
        return;
      }
    }
    plume::RenderFormat format = plume::RenderFormat::R32_FLOAT;
    if (usage == DeclUsage::kPosition) {
      format = plume::RenderFormat::R32G32B32A32_FLOAT;
    } else if (usage == DeclUsage::kNormal || usage == DeclUsage::kTangent) {
      format = plume::RenderFormat::R32G32B32_FLOAT;
    }
    resource->inputs.emplace_back(name, index, location, format, 15, 0);
  };
  for (u32 i = 0; i <= 4; ++i) add_synthetic(DeclUsage::kPosition, i, i);
  add_synthetic(DeclUsage::kNormal, 0, 5);
  add_synthetic(DeclUsage::kTangent, 0, 6);
  for (u32 i = 0; i <= 2; ++i) add_synthetic(DeclUsage::kTexCoord, i, 7 + i);
  add_synthetic(DeclUsage::kColor, 0, 10);

  const u32 allocation_size = 52 + (count + 1) * sizeof(GuestVertexElement);
  auto* memory = REX_KERNEL_MEMORY();
  const u32 guest_address = existing_address ? existing_address :
      memory->SystemHeapAlloc(allocation_size, 0x10);
  if (!guest_address) {
    return 0;
  }
  if (!existing_address) {
    memory->Zero(guest_address, allocation_size);
    auto* header = GuestHeader(guest_address);
    header->common = kDeclarationFlag;
    header->reference_count = 1;
    header->base_flush = 0xFFFF0000u;
    auto* bytes = memory->TranslateVirtual<uint8_t*>(guest_address);
    *reinterpret_cast<be_u32*>(bytes + 24) = count;
    std::memcpy(bytes + 52, elements,
                (count + 1) * sizeof(GuestVertexElement));
  }

  resource->guest_address = guest_address;
  std::shared_ptr<VertexDeclarationResource> previous;
  {
    std::lock_guard lock(g_declarations_mutex);
    auto& entry = g_declarations[guest_address];
    previous = std::exchange(entry, resource);
  }
  if (previous) HostDevice::RetireResource(std::move(previous));
  return guest_address;
}

u32 CreateVertexDeclarationResource(u32 elements_address) {
  return BuildVertexDeclaration(elements_address);
}

VertexDeclarationView ResolveVertexDeclaration(u32 guest_address) {
  auto resource = FindDeclaration(guest_address);
  if (guest_address && (!resource || !resource->owns_guest_memory)) {
    const auto* words = REX_KERNEL_MEMORY()->TranslateVirtual<const be_u32*>(guest_address);
    if ((static_cast<u32>(words[0]) & 0xFu) == 5 &&
        static_cast<u32>(words[6]) < kMaxElements) {
      const auto* elements = REX_KERNEL_MEMORY()->TranslateVirtual<const u8*>(guest_address + 52);
      const u64 hash = XXH3_64bits(elements, u32(words[6]) * sizeof(GuestVertexElement));
      if (!resource || resource->content_hash != hash) {
        if (resource)
          REXLOG_INFO("Native GPU: placement declaration contents changed at {:08X}", guest_address);
        BuildVertexDeclaration(guest_address + 52, guest_address, words[6]);
        resource = FindDeclaration(guest_address);
      }
    } else {
      return {};
    }
  }
  if (!resource) {
    return {};
  }
  return {resource->inputs.data(),
          static_cast<u32>(resource->inputs.size()),
          resource->supported,
          resource->swapped_texcoords,
          resource->swapped_normals,
          resource->swapped_binormals,
          resource->swapped_tangents,
          resource->swapped_blend_weights,
          resource->swapped_positions,
          resource->sint_texcoords,
          resource->has_r11g11b10_normal,
          resource->reversed_byte_elements,
          resource->content_hash};
}

bool IsNativeVertexDeclaration(u32 guest_address) {
  std::lock_guard lock(g_declarations_mutex);
  return g_declarations.contains(guest_address);
}

u32 AddRefNativeVertexDeclaration(u32 guest_address) {
  const auto resource = FindDeclaration(guest_address);
  if (!resource) return 0;
  auto* header = GuestHeader(guest_address);
  const u32 count = static_cast<u32>(header->reference_count) + 1;
  header->reference_count = count;
  return count;
}

u32 ReleaseNativeVertexDeclaration(u32 guest_address) {
  std::shared_ptr<VertexDeclarationResource> released;
  {
    std::lock_guard lock(g_declarations_mutex);
    const auto it = g_declarations.find(guest_address);
    if (it == g_declarations.end()) return ~u32{0};
    auto* header = GuestHeader(guest_address);
    const u32 old_count = header->reference_count;
    const u32 count = old_count ? old_count - 1 : 0;
    header->reference_count = count;
    if (count) return count;
    released = std::move(it->second);
    g_declarations.erase(it);
  }
  HostDevice::RetireResource(released);
  if (released->owns_guest_memory) REX_KERNEL_MEMORY()->SystemHeapFree(guest_address);
  return 0;
}

void ResetVertexDeclarations() {
  std::vector<u32> addresses;
  {
    std::lock_guard lock(g_declarations_mutex);
    addresses.reserve(g_declarations.size());
    for (const auto& [address, resource] : g_declarations) {
      if (resource->owns_guest_memory) addresses.push_back(address);
    }
    g_declarations.clear();
  }
  for (const u32 address : addresses) {
    REX_KERNEL_MEMORY()->SystemHeapFree(address);
  }
}

}  // namespace legodimensions::gpu_native
