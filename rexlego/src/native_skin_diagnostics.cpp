#ifdef LEGODIMENSIONS_NATIVE_GPU
#include "gpu_native/guest_metadata.h"
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include "native_heap_journal.h"
#include "slot_growth.h"
#include <array>
#include <atomic>
#include <iomanip>
#include <sstream>

REXCVAR_DEFINE_BOOL(native_scene_diagnostics, true, "Development",
    "Log NuCpuSkin scene teardown ownership in this diagnostic development build.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REX_EXTERN(__imp__sub_82C9A000);
REX_EXTERN(__imp__sub_82A34208);
REX_EXTERN(__imp__sub_82A33D28);
REX_EXTERN(__imp__sub_82E0F480);
REX_EXTERN(__imp__sub_82DD3220);
REX_EXTERN(__imp__sub_82A2AA28);

namespace {
std::atomic<uint64_t> next_skin{0};
thread_local uint64_t skin_sequence = 0;
thread_local uint32_t skin_address = 0;
struct HeapFreeOrigin {
  uint32_t heap{}, pointer{}, caller{}, frame{}, r29{}, r30{}, r31{};
};
thread_local HeapFreeOrigin free_origin;

bool ReadWords(uint32_t address, uint32_t* words, uint32_t count) {
  auto* kernel = rex::system::kernel_state();
  if (!kernel || !kernel->memory() || !count || count > 1024 ||
      !legodimensions::gpu_native::CopyNativeGuestMetadata(*kernel->memory(), address, words, count * 4)) return false;
  for (uint32_t i=0;i<count;++i) words[i]=__builtin_bswap32(words[i]);
  return true;
}
std::string HexWords(const uint32_t* words, uint32_t count) {
  std::ostringstream text; text << std::hex << std::setfill('0');
  for (uint32_t i=0;i<count;++i) { if(i)text << ',';text << std::setw(8) << words[i]; }
  return text.str();
}
void LogSkin(uint32_t self, uint32_t caller) {
  std::array<uint32_t,84> cache{};
  const uint64_t cache_address=uint64_t(self)+364;
  if (cache_address>UINT32_MAX || !ReadWords(uint32_t(cache_address),cache.data(),uint32_t(cache.size()))) {
    REXLOG_WARN("Native scene teardown: skin={} object={:08X} caller={:08X} cache metadata unavailable",
        skin_sequence,self,caller);
    return;
  }
  REXLOG_INFO("Native scene teardown: NuCpuSkin skin={} object={:08X} caller={:08X} cache+364={}",
      skin_sequence,self,caller,HexWords(cache.data(),uint32_t(cache.size())));
  // Actual cleanup sub_82C95F40 walks these arrays. Snapshot bounded prefixes
  // before native deletion, without editing their pointers, counts or payloads.
  for (uint32_t offset : {48u,60u,128u,148u,160u,180u,192u}) {
    const auto* array=cache.data()+offset/4;
    const uint32_t stride=offset==160?16:4;
    const uint32_t count=std::min(array[2],8u);
    std::array<uint32_t,32> values{};
    const bool readable=array[0] && count && ReadWords(array[0],values.data(),count*stride/4);
    REXLOG_INFO("Native scene teardown array: skin={} offset={} data={:08X} capacity={} count={} prefix={}",
        skin_sequence,offset,array[0],array[1],array[2],
        readable?HexWords(values.data(),count*stride/4):std::string("unavailable"));
    if (offset==192 && readable) for(uint32_t i=0;i<count;++i) {
      std::array<uint32_t,48> child{};
      if (!ReadWords(values[i],child.data(),uint32_t(child.size()))) continue;
      REXLOG_INFO("Native scene teardown child: skin={} index={} object={:08X} words={}",
          skin_sequence,i,values[i],HexWords(child.data(),uint32_t(child.size())));
    }
  }
}

native_heap_diagnostics::Journal heap_journal;
struct TreeOwnerObservation {
  uint32_t object=0, caller=0, root=0;
  std::array<uint32_t,16> object_words{}, root_words{};
  bool object_readable=false, root_readable=false;
};
struct TreeNodeObservation {
  uint32_t object=0, caller=0;
  std::array<uint32_t,16> words{};
  bool readable=false;
};
thread_local TreeOwnerObservation tree_owner;
thread_local TreeNodeObservation tree_node;
void LogHeapOwnershipHistory(uint32_t pointer) {
  const auto history=heap_journal.Read(pointer);
  REXLOG_ERROR("Native heap history: pointer={:08X} key={:X} records={} bounded=true missing_is_unknown=true",
      pointer,native_heap_diagnostics::Journal::Key(pointer),history.count);
  for(uint32_t i=0;i<history.count;++i) {
    const auto& e=history.events[i];
    REXLOG_ERROR("Native heap history event: seq={} op={} pointer={:08X} heap={:08X} caller={:08X} frame={:08X} thread={:08X} bytes={}",
        e.sequence,uint32_t(e.operation),e.pointer,e.heap,e.caller,e.frame,e.thread,e.bytes);
  }
  if(tree_owner.object)
    REXLOG_ERROR("Native tree cleanup owner: object={:08X} caller={:08X} tree_offset=188 root={:08X} before_object={} before_root={}",
        tree_owner.object,tree_owner.caller,tree_owner.root,
        tree_owner.object_readable?HexWords(tree_owner.object_words.data(),uint32_t(tree_owner.object_words.size())):std::string("unavailable"),
        tree_owner.root_readable?HexWords(tree_owner.root_words.data(),uint32_t(tree_owner.root_words.size())):std::string("unavailable"));
  if(tree_node.object)
    REXLOG_ERROR("Native tree cleanup node: object={:08X} caller={:08X} before_header_and_node={}",
        tree_node.object,tree_node.caller,
        tree_node.readable?HexWords(tree_node.words.data(),uint32_t(tree_node.words.size())):std::string("unavailable"));
}

void LogHeapAssertion(const PPCContext& ctx) {
  // This is the original "Expected allocated block" assertion. Record its
  // caller state before the title formats the error and deliberately faults.
  // Minidump indirect scanning sees host pointers; guest r1 is a 32-bit
  // address and its translated stack was absent in the Scooby-Doo dump.
  const std::array<uint32_t,32> registers{
      ctx.r0.u32,ctx.r1.u32,ctx.r2.u32,ctx.r3.u32,ctx.r4.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,
      ctx.r8.u32,ctx.r9.u32,ctx.r10.u32,ctx.r11.u32,ctx.r12.u32,ctx.r13.u32,ctx.r14.u32,ctx.r15.u32,
      ctx.r16.u32,ctx.r17.u32,ctx.r18.u32,ctx.r19.u32,ctx.r20.u32,ctx.r21.u32,ctx.r22.u32,ctx.r23.u32,
      ctx.r24.u32,ctx.r25.u32,ctx.r26.u32,ctx.r27.u32,ctx.r28.u32,ctx.r29.u32,ctx.r30.u32,ctx.r31.u32};
  REXLOG_ERROR("Native heap assertion: caller={:08X} heap={:08X} block={:08X} r0-r31={}",
      uint32_t(ctx.lr),ctx.r3.u32,ctx.r4.u32,HexWords(registers.data(),uint32_t(registers.size())));
  if (free_origin.pointer) {
    REXLOG_ERROR("Native heap free origin: heap={:08X} pointer={:08X} caller={:08X} frame={:08X} r29={:08X} r30={:08X} r31={:08X}",
        free_origin.heap,free_origin.pointer,free_origin.caller,free_origin.frame,
        free_origin.r29,free_origin.r30,free_origin.r31);
    // Exact TU23 linked-list clear call site in the Scooby-Doo crash. r31 is
    // its owning list before the allocator overwrites the caller registers.
    if (free_origin.caller==0x83DD05A4) {
      std::array<uint32_t,16> list{};
      if (ReadWords(free_origin.r31,list.data(),uint32_t(list.size())))
        REXLOG_ERROR("Native heap assertion list: object={:08X} words={}",free_origin.r31,
            HexWords(list.data(),uint32_t(list.size())));
      std::array<uint32_t,8> node{};
      if (ReadWords(free_origin.pointer,node.data(),uint32_t(node.size())))
        REXLOG_ERROR("Native heap assertion node: object={:08X} words={}",free_origin.pointer,
            HexWords(node.data(),uint32_t(node.size())));
    }
  }
  LogHeapOwnershipHistory(free_origin.pointer ? free_origin.pointer : ctx.r4.u32+4);
  std::array<uint32_t,32> block{};
  if (ReadWords(ctx.r4.u32,block.data(),uint32_t(block.size())))
    REXLOG_ERROR("Native heap assertion block: address={:08X} words={}",ctx.r4.u32,
        HexWords(block.data(),uint32_t(block.size())));
  uint32_t frame=ctx.r1.u32;
  for (uint32_t depth=0;depth<12;++depth) {
    std::array<uint32_t,64> words{};
    if (!frame || (frame&15) || !ReadWords(frame,words.data(),uint32_t(words.size()))) {
      REXLOG_ERROR("Native heap assertion stack: depth={} frame={:08X} unavailable",depth,frame);
      break;
    }
    REXLOG_ERROR("Native heap assertion stack: depth={} frame={:08X} words={}",depth,frame,
        HexWords(words.data(),uint32_t(words.size())));
    const uint32_t previous=words[0];
    if (previous<=frame || uint64_t(previous)-frame>0x20000) break;
    frame=previous;
  }
}
}

REX_HOOK_RAW(sub_82C9A000) {
  if (!REXCVAR_GET(native_scene_diagnostics)) { __imp__sub_82C9A000(ctx,base);return; }
  struct Scope {
    uint64_t previous_sequence=skin_sequence;
    uint32_t previous_address=skin_address;
    ~Scope(){skin_sequence=previous_sequence;skin_address=previous_address;}
  } scope;
  skin_sequence=++next_skin;skin_address=ctx.r3.u32;
  LogSkin(skin_address,uint32_t(ctx.lr));
  __imp__sub_82C9A000(ctx,base);
  REXLOG_INFO("Native scene teardown: skin={} object={:08X} completed",skin_sequence,skin_address);
}

REX_HOOK_RAW(sub_82A34208) {
  // The scene destructor has already run at this call site. A heap value
  // of four cannot own the block, so leave it alone instead of entering
  // the allocator with an invalid heap. The caller ignores the return value.
  if (uint32_t(ctx.lr) == 0x82CC0488 && ctx.r3.u32 == 4 && ctx.r4.u32) {
    REXLOG_WARN("Native scene cleanup: skipped free of {:08X} with invalid heap 00000004",
        ctx.r4.u32);
    return;
  }
  // Diagnostic only: retain the original allocator call and failure behavior.
  // The owner crash released 0x10000, then read its header at 0xFFFC.
  if (!REXCVAR_GET(native_scene_diagnostics)) { __imp__sub_82A34208(ctx,base);return; }
  struct OriginScope {
    HeapFreeOrigin previous=free_origin;
    ~OriginScope(){free_origin=previous;}
  } origin_scope;
  free_origin={ctx.r3.u32,ctx.r4.u32,uint32_t(ctx.lr),ctx.r1.u32,ctx.r29.u32,ctx.r30.u32,ctx.r31.u32};
  if (skin_sequence && ctx.r4.u32 && (ctx.r4.u32<0x100000 || (ctx.r4.u32&3)))
    REXLOG_WARN("Native scene teardown free: skin={} object={:08X} pointer={:08X} heap={:08X} caller={:08X}",
        skin_sequence,skin_address,ctx.r4.u32,ctx.r3.u32,uint32_t(ctx.lr));
  heap_journal.Record(native_heap_diagnostics::Operation::kFreeBegin,
      free_origin.pointer,free_origin.heap,free_origin.caller,free_origin.frame,ctx.r13.u32);
  __imp__sub_82A34208(ctx,base);
  heap_journal.Record(native_heap_diagnostics::Operation::kFreeComplete,
      free_origin.pointer,free_origin.heap,free_origin.caller,free_origin.frame,ctx.r13.u32);
}

REX_HOOK_RAW(sub_82A2AA28) {
  if (REXCVAR_GET(native_scene_diagnostics)) LogHeapAssertion(ctx);
  // Preserve the assertion and its failure behavior. Diagnostics must never
  // turn an invalid/free block into a silently skipped deallocation.
  __imp__sub_82A2AA28(ctx,base);
}

REX_HOOK_RAW(sub_82A33D28) {
  native_load_heap::AllocationAttemptScope allocation_attempt;
  if (!REXCVAR_GET(native_scene_diagnostics)) {__imp__sub_82A33D28(ctx,base);return;}
  const uint32_t heap=ctx.r3.u32,bytes=ctx.r4.u32,caller=uint32_t(ctx.lr),frame=ctx.r1.u32,thread=ctx.r13.u32;
  __imp__sub_82A33D28(ctx,base);
  heap_journal.Record(native_heap_diagnostics::Operation::kAllocated,
      ctx.r3.u32,heap,caller,frame,thread,bytes);
}

REX_HOOK_RAW(sub_82E0F480) {
  if (!REXCVAR_GET(native_scene_diagnostics)) {__imp__sub_82E0F480(ctx,base);return;}
  struct Scope {TreeOwnerObservation previous=tree_owner;~Scope(){tree_owner=previous;}} scope;
  tree_owner={};tree_owner.object=ctx.r3.u32;tree_owner.caller=uint32_t(ctx.lr);
  tree_owner.object_readable=ReadWords(tree_owner.object,tree_owner.object_words.data(),uint32_t(tree_owner.object_words.size()));
  std::array<uint32_t,2> tree{};
  if(uint64_t(tree_owner.object)+188<=UINT32_MAX && ReadWords(tree_owner.object+188,tree.data(),2)) {
    tree_owner.root=tree[0];
    if(tree_owner.root>=4)tree_owner.root_readable=ReadWords(tree_owner.root-4,tree_owner.root_words.data(),uint32_t(tree_owner.root_words.size()));
  }
  __imp__sub_82E0F480(ctx,base);
}

REX_HOOK_RAW(sub_82DD3220) {
  if (!REXCVAR_GET(native_scene_diagnostics) || !tree_owner.object) {__imp__sub_82DD3220(ctx,base);return;}
  struct Scope {TreeNodeObservation previous=tree_node;~Scope(){tree_node=previous;}} scope;
  tree_node={};tree_node.object=ctx.r3.u32;tree_node.caller=uint32_t(ctx.lr);
  if(tree_node.object>=4)tree_node.readable=ReadWords(tree_node.object-4,tree_node.words.data(),uint32_t(tree_node.words.size()));
  __imp__sub_82DD3220(ctx,base);
}

#endif
