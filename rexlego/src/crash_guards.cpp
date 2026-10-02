// Guards for game code that dereferences a null pointer in this port but
// apparently never did on the console.
//
// sub_83C01C08: crash right after achievement 12 or 42 unlocks
// ------------------------------------------------------------
// A state handler, called indirectly from sub_82E28320. It walks three lists
// owned by the object at [0x848BFE74]+140. Entries are 16 bytes, and the
// pointer at +4 is an object whose +428 it reads straight away. In a player's
// log (2026-09-30) that pointer was null 8 times out of 8, each time 10-25 ms
// after the game wrote achievement 12 or 42: "read of guest 0x000001AC",
// lr=83C01C64. The hook below is the recompiled body unchanged, apart from one
// check that skips an entry whose object is null, which is all a null-safe
// version of the loop would do.

#include "legodimensions_pch.h"

#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>

DECLARE_REX_FUNC(__restgprlr_15);
DECLARE_REX_FUNC(__savegprlr_15);
DECLARE_REX_FUNC(sub_82A29A28);
DECLARE_REX_FUNC(sub_82E28900);
DECLARE_REX_FUNC(sub_836F62D8);
DECLARE_REX_FUNC(sub_836F9C38);
DECLARE_REX_FUNC(sub_836FDFC0);
DECLARE_REX_FUNC(sub_836FE440);
DECLARE_REX_FUNC(sub_836FE470);
DECLARE_REX_FUNC(sub_836FEA38);
DECLARE_REX_FUNC(sub_838233C8);
DECLARE_REX_FUNC(sub_83A0A9B8);
DECLARE_REX_FUNC(sub_83E46698);

namespace {
std::atomic<uint32_t> g_null_entries_skipped{0};
}

REX_HOOK_RAW(sub_83C01C08) {
	REX_FUNC_PROLOGUE();
	uint32_t ea{};
	ctx.r12.u64 = ctx.lr;
	ctx.lr = 0x83C01C10;
	__savegprlr_15(ctx, base);
	ctx.r31.s64 = ctx.r1.s64 + -240;
	ea = -240 + ctx.r1.u32;
	REX_STORE_U32(ea, ctx.r1.u32);
	ctx.r1.u32 = ea;
	ctx.r26.s64 = -2071199744;
	ctx.r15.u64 = ctx.r3.u64;
	ctx.r11.u64 = REX_LOAD_U32(ctx.r26.u32 + -396);
	ctx.r17.u64 = REX_LOAD_U32(ctx.r11.u32 + 140);
	ctx.cr6.compare<uint32_t>(ctx.r17.u32, 0, ctx.xer);
	if (ctx.cr6.eq) goto loc_83C01D98;
	ctx.r10.s64 = -2128871424;
	ctx.r11.s64 = -2072051712;
	ctx.r18.s64 = 0;
	ctx.r21.s64 = -2072051712;
	ctx.r19.u64 = ctx.r10.u64 | 40389;
	ctx.r22.s64 = -2069692416;
	ctx.r20.s64 = -2071199744;
	ctx.r24.s64 = -2069692416;
	ctx.r25.s64 = -2069692416;
	ctx.r16.s64 = ctx.r11.s64 + -26384;
loc_83C01C58:
	ctx.r4.u64 = ctx.r18.u64;
	ctx.r3.u64 = ctx.r17.u64;
	ctx.lr = 0x83C01C64;
	sub_836F62D8(ctx, base);
	ctx.r11.u64 = REX_LOAD_U32(ctx.r3.u32 + 32);
	ctx.r23.s64 = 0;
	ctx.r28.s64 = ctx.r3.s64 + 24;
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	if (ctx.cr6.eq) goto loc_83C01D8C;
	ctx.r27.s64 = 0;
loc_83C01C7C:
	ctx.r11.u64 = REX_LOAD_U32(ctx.r28.u32 + 0);
	ctx.r4.s64 = ctx.r31.s64 + 80;
	ctx.r3.u64 = REX_LOAD_U32(ctx.r26.u32 + -396);
	ctx.r11.u64 = ctx.r11.u64 + ctx.r27.u64;
	ctx.r30.u64 = REX_LOAD_U32(ctx.r11.u32 + 4);
	if (ctx.r30.u32 == 0) {
		// The guard. Everything else in this function is the original.
		if (g_null_entries_skipped.fetch_add(1, std::memory_order_relaxed) < 8) {
			REXLOG_WARN("crash guard: sub_83C01C08 skipped an entry with a null object (list {}, entry {})",
			            ctx.r18.u32, ctx.r23.u32);
		}
		goto loc_83C01D78;
	}
	ctx.r10.u64 = REX_LOAD_U32(ctx.r30.u32 + 428);
	REX_STORE_U32(ctx.r31.u32 + 84, ctx.r30.u32);
	REX_STORE_U32(ctx.r31.u32 + 80, ctx.r10.u32);
	ctx.lr = 0x83C01CA0;
	sub_836F9C38(ctx, base);
	ctx.r4.u64 = ctx.r3.u64;
	ctx.cr6.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);
	if (ctx.cr6.lt) goto loc_83C01D78;
	ctx.r11.u64 = REX_LOAD_U32(ctx.r21.u32 + 5820);
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	if (ctx.cr6.eq) goto loc_83C01D78;
	ctx.r3.u64 = REX_LOAD_U32(ctx.r20.u32 + -520);
	ctx.lr = 0x83C01CC0;
	sub_838233C8(ctx, base);
	ctx.r11.u64 = REX_LOAD_U32(ctx.r25.u32 + -25984);
	ctx.r29.u64 = ctx.r3.u64;
	ctx.r10.u64 = ctx.r11.u32 & 0x1;
	ctx.cr6.compare<uint32_t>(ctx.r10.u32, 0, ctx.xer);
	if (!ctx.cr6.eq) goto loc_83C01D0C;
	ctx.r11.u64 = ctx.r11.u64 | 1;
	REX_STORE_U32(ctx.r25.u32 + -25984, ctx.r11.u32);
	ctx.r3.u64 = REX_LOAD_U32(ctx.r21.u32 + 5820);
	ctx.cr6.compare<uint32_t>(ctx.r3.u32, 0, ctx.xer);
	if (ctx.cr6.eq) goto loc_83C01CF8;
	ctx.r4.u64 = ctx.r19.u64;
	ctx.lr = 0x83C01CF0;
	sub_82A29A28(ctx, base);
	REX_STORE_U32(ctx.r24.u32 + -25980, ctx.r3.u32);
	goto loc_83C01D00;
loc_83C01CF8:
	ctx.r11.s64 = 0;
	REX_STORE_U32(ctx.r24.u32 + -25980, ctx.r11.u32);
loc_83C01D00:
	ctx.r11.s64 = -2079260672;
	ctx.r3.s64 = ctx.r11.s64 + -344;
	ctx.lr = 0x83C01D0C;
	sub_83E46698(ctx, base);
loc_83C01D0C:
	ctx.r10.u64 = REX_LOAD_U32(ctx.r29.u32 + 28);
	ctx.r11.u64 = REX_LOAD_U32(ctx.r24.u32 + -25980);
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, ctx.r10.u32, ctx.xer);
	if (!ctx.cr6.eq) goto loc_83C01D78;
	ctx.r11.u64 = REX_LOAD_U8(ctx.r22.u32 + -27206);
	ctx.cr6.compare<uint32_t>(ctx.r11.u32, 0, ctx.xer);
	if (ctx.cr6.eq) goto loc_83C01D78;
	ctx.r3.s64 = ctx.r16.s64 + 2416;
	ctx.lr = 0x83C01D30;
	sub_83A0A9B8(ctx, base);
	ctx.cr6.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);
	if (!ctx.cr6.eq) goto loc_83C01D78;
	ctx.r11.s64 = 0;
	ctx.r3.u64 = ctx.r30.u64;
	REX_STORE_U8(ctx.r22.u32 + -27206, ctx.r11.u8);
	ctx.lr = 0x83C01D48;
	sub_836FEA38(ctx, base);
	ctx.r11.u64 = REX_LOAD_U32(ctx.r30.u32 + 424);
	ctx.r4.s64 = ctx.r31.s64 + 84;
	ctx.r3.u64 = REX_LOAD_U32(ctx.r26.u32 + -396);
	REX_STORE_U32(ctx.r31.u32 + 84, ctx.r11.u32);
	ctx.lr = 0x83C01D5C;
	sub_836FDFC0(ctx, base);
	ctx.r6.s64 = 0;
	ctx.r5.s64 = 0;
	ctx.r4.s64 = 0;
	ctx.r3.u64 = ctx.r30.u64;
	ctx.lr = 0x83C01D70;
	sub_836FE470(ctx, base);
	ctx.r3.u64 = ctx.r30.u64;
	ctx.lr = 0x83C01D78;
	sub_836FE440(ctx, base);
loc_83C01D78:
	ctx.r11.u64 = REX_LOAD_U32(ctx.r28.u32 + 8);
	ctx.r23.s64 = ctx.r23.s64 + 1;
	ctx.r27.s64 = ctx.r27.s64 + 16;
	ctx.cr6.compare<uint32_t>(ctx.r23.u32, ctx.r11.u32, ctx.xer);
	if (ctx.cr6.lt) goto loc_83C01C7C;
loc_83C01D8C:
	ctx.r18.s64 = ctx.r18.s64 + 1;
	ctx.cr6.compare<int32_t>(ctx.r18.s32, 3, ctx.xer);
	if (ctx.cr6.lt) goto loc_83C01C58;
loc_83C01D98:
	ctx.r4.s64 = 0;
	ctx.r3.s64 = ctx.r15.s64 + 60;
	ctx.lr = 0x83C01DA4;
	sub_82E28900(ctx, base);
	ctx.r1.s64 = ctx.r31.s64 + 240;
	__restgprlr_15(ctx, base);
	return;
}
