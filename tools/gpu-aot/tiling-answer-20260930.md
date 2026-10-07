# Ответ на вопрос tiling-reference-20260929.md (TU23, 2026-09-30)

Метод: скан сгенерированного кода (rexlego/generated/default) на load/store по
смещениям device +13124/+13128/+13368/+13556/+13560/+13564 в диапазоне
0x83F00000–0x84100000, потом ручной просмотр каждой ветки. Совпадения в
0x840xxxxx — это другие структуры с теми же смещениями, их не учитывать.
Статический анализ, в игре НЕ проверялось.

## Что пишет оригинал

BeginTiling (83FBCD28): +13100, +13104, +13108, +13112, +13116 (RT на время
тайлинга), +13120 (DS на время тайлинга), +13124 (count), rects с +13128,
origins с +13368, +13556/+13560 (extent), +13564 (flags), +13588.
Сам вызывает 83FBA618, 83FBA9F8, 83FBC8A0 (→ Clear-путь).

EndTiling (83FBD098): из этого набора сбрасывает ТОЛЬКО +13100 (=0). count
(+13124), flags (+13564), extent и rects остаются устаревшими после End.
Значит «тайлинг активен» потребители определяют НЕ по count != 0, а по
+13100 и/или по совпадению текущих RT/DS (+12828/+12832) с +13116/+13120.
Внутри End: SetPredication (83FBCBB8), 83FB94F8, Resolve (83FBF0D8),
InsertCallback (83FC0F38), KickOff (83FC0C10), 83FC0540, стор +13772,
+13084..+13092, +11060/+11064/+11068/+11071, в конце 83FCBA68(count).

## Незахваченные потребители

| Функция | Поле | Как читает | Кто вызывает |
|---|---|---|---|
| 83FBA710 | extent | если RT==+13116 и DS==+13120 → берёт extent вместо размера поверхности | 83FBA978 (SetViewport, ЗАХВАЧЕН), 83FBA9F8 (не захвачен, зовут 83FB2898, 83FB3B48, Begin) |
| 83FBC8A0 | extent | тот же флаг (байт-аргумент) → прямоугольник клира = extent | Clear 83FBC9D8 (захвачен), Begin |
| 83FBC220 → 83FBBC80 | count, origins (+13368/+13372), +13548/+13552 | цикл по тайлам, эмитит пакеты на каждый тайл, дальше 83FBBA18 и 83FD0BF8 | 83FBC8A0 (клир-путь) |
| 83FB7AB8 | count, rects-массив с +13132 | цикл по тайлам, внутри KickOff 83FC0C10 | 83FB62F8 (←83FB6DD0), SetScissor 83FBA068 (захвачен), 83FBA618, 83FBC220, ResolveEx 83FBDF80, 83FC5048 (←EndExport, захвачен) |
| 83FD0BF8 | count | цикл count-1..0, эмиссия в командный буфер | 83FBBC80, 83FBC220, 83FBDD20, ResolveEx 83FBDF80, 83FC2C80, 83FC6180, Draw* (захвачены), 83FCFBB8 |
| 83FAFB08 | count | если бит 0x20 в флагах → count, иначе 1 (множитель резерва места в ринге) | 83FC7580 ← Swap (захвачен) |
| 83FCAB10 StartWorkerQueue | count, flags, origins | — | именно он крашил (lr=83FCABCC) |
| 83FBDF80 ResolveEx | origins, count | через 83FB7AB8/83FD0BF8 | только Resolve 83FBF0D8 (захвачен) |

Опасные при отсутствии очередей: всё, что ведёт в 83FB7AB8 (KickOff внутри),
83FD0BF8 (пишет в командный буфер) и 83FCAB10. Незахваченные входы туда:
83FBA618, 83FB62F8/83FB6DD0, 83FC2C80/83FC38D0, 83FC6180/83FB2898,
83FCFBB8/83FD0480, 83FBDD20. 83FBA9F8→83FBA710 только читает extent — безопасно.

## Вывод / контракт

1. Минимально безопасно: ничего из +13124..+13564 не писать. Все известные
   читатели extent/count из захваченных точек (Viewport, Clear, Scissor,
   Draw*, Resolve, EndExport, Swap) уже подменены хуками; оставшиеся
   незахваченные читатели count — как раз эмиттеры пакетов/KickOff, которым
   ненулевой count даёт повод работать.
2. Если писать (например, чтобы 83FBA9F8/83FBA710 давали полный viewport):
   достаточно +13116/+13120 (RT/DS тайлинга) и +13556/+13560 (extent);
   count оставить 0 — 83FB7AB8/83FBBC80/83FD0BF8 при count==0 пропускают циклы.
   В End сбросить +13100=0 как оригинал и дополнительно (отклонение от
   оригинала, но безопаснее) +13116/+13120=0.
3. flags +13564 читают только Begin/End/83FCAB10 — не писать.
4. До реализации проверить: вызываются ли 83FBA618 и 83FB6DD0 в рантайме во
   время кадра (лог/брейкпоинт) — если да, их хукать или гарантировать count==0.

Непроверено: точная семантика +13100 (фаза? флаг?), потребители через
указатели/vtable (скан ловит только прямые смещения от базы device).

# Дополнение 2 (2026-09-30): порядок clear и привязок вокруг BeginTiling

Статический анализ, в игре не проверено.

## Кто вызывает Begin/End

Единственный игровой вызов идёт через `sub_82B6A7B0` (переключение группы
render targets; группа — объект игры, текущая лежит в глобале 0x849505F8).
Тонкие обёртки: `82B648C8` → b BeginTiling, `82B648E8` → b EndTiling.

Порядок внутри 82B6A7B0(obj, bool direct):
1. если у старой группы тайлинг (поле +308) → EndTiling (82B648E8), иначе заглушка 837F4088.
2. current = obj.
3. если obj+292 (dirty) или direct → 82B6A0C8(obj) — (пере)создание поверхностей группы.
4. direct=1 → 83FBB110 (пакетная установка 5 поверхностей obj+20/76/132/188/244), Begin НЕ вызывается.
   direct=0 → цикл 5× `82B698A0` (шаг 56 байт) → 82B64778 = b 83FBB758 = b SetRenderTarget 83FBAAA8
   и 82B64818 = b SetDepthStencil 83FBAE38.
5. direct=0 и obj+308 указывает на тайлинг → BeginTiling(flags=1, rects=obj+312) через 82B648C8.

Итого: RT0–RT3 и DS **привязаны ДО Begin**, clear в самом Begin пропущен (flags=1).

## Где clear

Пример прохода сцены `82B685A8`: 82B6A7B0 (Set RTs → Begin) → `82B64FD0` (обёртка clear)
→ 82B6A980 / 82B69FB0 / … → отрисовка. То есть **clear идёт ПОСЛЕ Begin, внутри тайлинга,
до геометрии**. Пересоздание attachment в BeginTilingHook уже выполненный clear НЕ теряет —
при условии, что пересоздание делается синхронно в Begin, а не лениво на первом draw/clear.
Остальные вызыватели 82B64FD0 (20 штук, 82B65230…82BCB010) — не проверял поштучно.

Что очищает 82B64FD0 (все вызовы Clear 83FBC9D8 с count=0, rects=0 = весь extent):
- основной: flags собираются из аргумента: 0x1 (RT0, цвет из аргумента r6),
  0x30 (Z+stencil), 0x20 (stencil), 0x40/0x80 (hi-stencil), вокруг — 83FB9560/83FB9590.
- если группа obj+728 == 1 (MRT-режим): ещё два Clear:
  flags=0x2 (RT1) цвет 0x0080FF00, flags=0x4 (RT2) цвет 0.
Это совпадает с резолвами RT0/RT1/RT2/depth из трейса. Биты flags трактованы по
заголовкам X360 SDK (TARGET0..3 = 1/2/4/8, ZBUFFER 0x10, STENCIL 0x20) — сверить с ClearHook.

## Viewport — поправка принята

Да, 83FBA710 сравнивает все пять: RT0–RT3 (+12816..+12828) с +13104..+13116 и DS +12832 с +13120;
Begin пишет все пять +13104..+13120. Мой прошлый ответ (+13116/+13120) был неполным.
Хранить логический extent только на host и не писать гостевые count/flags — согласен:
все читатели extent (83FBA710 через SetViewport, 83FBC8A0 через Clear) на захваченных путях.
Host-правило «тайлинг активен» = снимок пяти привязок в Begin совпадает с текущими.

# Дополнение 3 (2026-09-30): 0x1A20AB55 и Resolve(0x100)

## 0x1A20AB55 — разбор D3DFORMAT

биты 0–5 = 21 = `k_16_16_16_16_EDRAM`; 6–7 = 1 (endian 8in16); бит 8 = tiled;
знаки X/Y/Z/W = 1 (SIGNED) у всех четырёх; бит 17 NumFormat = 0 (fraction);
swizzle XYZW. Экспоненты в самом D3DFORMAT нет — она в RB_COLOR_INFO[20:25]
(при записи) и в exp_adjust fetch-константы (при чтении).

Семантика (xenia, xenos.h:307 и d3d12/texture_cache.cpp:184): цветовой RT
`k_16_16_16_16` = **фиксированная точка −32…+32** (0x8001 = −32, 0x7FFF = +32),
НЕ 0…1. `_EDRAM`-текстура — тот же формат, «−32…32 range».

Как делает xenia в режиме host RT (command_processor.cpp:4702–4719, cvar
snorm16_render_target_full_range=true по умолчанию):
- host-формат **R16G16B16A16_SNORM** (render_target_cache.cpp:1465), не UNORM;
- на выходе PS цвет умножается на `2^(color_exp_bias − 5)`, т.е. RB_COLOR_INFO
  exp_bias и **дополнительно /32**, чтобы −32…32 легло в −1…1 SNORM;
- resolve копирует сырые биты; для тракта truncated −1…1 xenia добавляет +5 к
  copy_dest_exp_bias (util/draw.cpp:1066–1079), в full-range — нет;
- чтение: знаковый fetch даёт −1…1, fetch exp_adjust **+5** (= ×32) восстанавливает
  −32…32. Поэтому у tonemap-fetch exp=+5.

У вас: UNORM без /32 → значения >1 клипаются в 1.0 при записи, потом fetch ×32 →
**всё ×32 = белый экран**. Плюс UNORM теряет отрицательные значения и неверно
читает signed-биты. Фикс: RT → R16G16B16A16_SNORM, в PS для этого RT умножать
вывод на 2^(exp_bias)/32 (exp_bias из RB_COLOR_INFO, у 360 это signed 6 бит);
при чтении как текстуры — SNORM view + exp_adjust из fetch как есть.
То же самое для RG16 (`k_16_16`, тоже −32…32).
Минус (признан в xenia): блендинг в /32-пространстве не точен, для HDR-сцены обычно терпимо.

## Resolve(flags=0x100)

0x100 = D3DRESOLVE_CLEARRENDERTARGET (резолв RT0 + очистка), по SDK.
Игровые вызовы Resolve идут через обёртку `82B64860` (флаги в r3 игры) из:
- `82BD5BE8` — общий «резолв рендер-текстуры»: к флагам добавляет exponent-bias в
  старших битах — `0x2C000000` если тип объекта ([obj+12]) == 117, `0xEC000000` если 118;
  **dst = obj+120 + 52*[obj+116]** — массив 52-байтных D3D-заголовков текстур ВНУТРИ
  игрового объекта (кольцо по +116).
- `82B65F28` — flags=0, dst тоже [..+44]+120+52*[+116].
Вызыватели 82BD5BE8: 82BD6800, 82BD69A0 (флаги = база|индекс RT или |4 для depth,
256 добавляется условно), 82B66200 (flags = r11+256 | маска), 82B66AC0, 82B68F30,
82B68988, 82B690D8 (эти четыре с flags=0).

Почему dst AC4B9A48 «не текстура»: заголовок не создан через CreateTexture — он
встроен в игровой объект и, вероятно, заполняется напрямую (кандидат — 83FB57C8,
его зовёт 82B6A0C8 при (пере)создании поверхностей группы; не проверено, что это
XGSetTextureHeader). Нужно хукать инициализацию заголовка, а не только CreateTexture.

Экспозиция/readback: статически не доказано. Чтобы отличить, залогируй lr цепочки
(82B64860 → 82BD5BE8 → кто) и [obj+12] для этого вызова, и есть ли потом Lock/чтение
CPU по base этой текстуры. RG16 1-пиксельный/маленький → типично адаптация яркости;
она тоже `k_16_16` (−32…32) — при UNORM+×32 экспозиция развалится так же.

# Addendum 4 (2026-09-30): signed EDRAM RT -> unsigned texture, RB_COLOR_INFO location

Static analysis of generated TU23 code + rexglue-sdk xenia sources. Not verified in game.

## 1. Resolve must convert numerically, not copy SNORM bits

Xenos resolve is a format conversion, not a memcpy: it reads the EDRAM value in the
RT's own range (k_16_16_16_16 = fixed -32..32), multiplies by 2^copy_dest_exp_bias,
then packs into copy_dest_format per copy_dest_number (unsigned repeating fraction
= UNORM, clamp 0..1). xenia only takes the raw-copy path when exp_bias == 0 AND the
dest number format matches (rexglue-sdk src/graphics/util/draw.cpp:1132-1178,
ColorResolveNumberFormatMatches); otherwise it runs the full conversion shader.

Where the scale comes from: the game's resolve wrapper `82BD5BE8` ORs an exponent
bias into the top 6 bits of the D3D Resolve flags (bits 31:26, signed):
- object type [obj+12] == 118 -> `oris 0xEC00` -> 0xEC000000 -> bias = 0b111011 = **-5**
- object type [obj+12] == 117 -> `oris 0x2C00` -> 0x2C000000 -> bias = 0b001011 = +11
  (meaning unclear; possibly a different packing or a deliberate large bias - check the trace)

So the consistent chain for the half-colour texture is:
EDRAM value v in -32..32 -> resolve x2^-5 -> v/32 -> clamp to 0..1 -> UNORM16 ->
fetch unsigned, exp_adjust +5 -> x32 -> v (negatives lost, which the game accepts).
That matches ACEAEF2C: format 26, num 0, sign 0,0,0,0, exp +5.

Consequence for native: if the host RT stores v/32 in SNORM (the xenia full-range
convention), the resolve into the unsigned texture must apply (copy_exp_bias + 5),
i.e. for bias -5 it's a plain SNORM->UNORM re-encode with clamp at 0 (net factor 1),
NOT a bit copy (SNORM bits read as UNORM are wrong for negatives and off by 2x).
Take the bias from the actual Resolve flags (top 6 bits) per call; don't hardcode.

## 2. RB_COLOR_INFO shadow in the guest device

Written by SetRenderTarget `83FBAAA8(device, index, surface)` (hooked by native):
- device+12816+4*i = surface pointer (RT0..RT3; DS at +12832).
- device[(2593 + k)*4] = **surface+28** (u32, big-endian), where k = 0 for RT0, i+1 otherwise:
  - RT0 -> device+**10372**
  - RT1 -> device+**10380**
  - RT2 -> device+**10384**
  - RT3 -> device+**10388**
  (gap after RT0 matches the register order RB_COLOR_INFO=0x2001, RB_COLOR1..3_INFO=0x2003..0x2005.)
- Then a dirty bit is set in the 64-bit mask at device+16.
- For formats 2/3/10/12 (2_10_10_10 family) the stored word is patched according to a
  per-slot value at device+12408+4*i (gamma/float variant switch); 16_16_16_16 is not patched.
- Also RB_COLOR_MASK shadow: device+10460, one nibble per RT, taken from per-slot
  masks at device+12292+4*i (zeroed when the slot's surface is null).

Field layout of that word (same as RB_COLOR_INFO): bits 19:16 = ColorRenderTargetFormat,
bits 25:20 = color_exp_bias (signed 6-bit), low bits = EDRAM base. The source is
surface+28, i.e. it's filled at surface creation from D3DSURFACE_PARAMETERS.ColorExpBias.

Practical: in SetRenderTargetHook read the big-endian u32 at surface+28 directly
(no need to read the device shadow); PS output scale for slot i =
2^(exp_bias_i) * (format == 16_16 or 16_16_16_16 ? 1/32 : 1). Only fixed-16 formats get the /32.

# Addendum 5 (2026-09-30): D3DSURFACE_PARAMETERS and how 83FB52C0 builds surface+28

Correction: 83FB57C8 is GetSurfaceDesc (per the GPU agent), not a texture-header
initializer. The destination-header initializer for obj+120+52*idx is still unknown.

Signature as used (inferred from register use in TU23 `83FB52C0`):
`83FB52C0(width r3, height r4, D3DFORMAT r5, msaa r6, const D3DSURFACE_PARAMETERS* r7,
D3DSurface* r8, u32* out_tiles r9, u32* out_hiz_size r10)`

## D3DSURFACE_PARAMETERS (r7), all big-endian 32-bit
| off | field | used as |
|---|---|---|
| +0 | Base (EDRAM tile base) | `& 0xFFF` into surface+28 |
| +4 | HierarchicalZBase | depth only, surface+32 |
| +8 | ColorExpBias (signed int) | colour only, `& 0x3F` -> bits 25:20 of surface+28 |
| +12 | HiZ func/flags (depth only) | if nonzero: (value-1)&1 into surface+32 |

**parameters == NULL defaults:** Base = 0, ColorExpBias = 0, HierarchicalZBase = -1
(no HiZ) for colour; for depth formats HiZ base = -1 only if the global u32 at
0x84A6A644 is nonzero, otherwise 0. (Verify that address: lis -31577 / -22972.)

## surface+28 for colour formats (D3DFORMAT low 6 bits != 22/23)
```
tf        = D3DFORMAT & 0x3F            // 54 is remapped to 7 first
rt_fmt    = (u16 table @0x824C9660[tf] >> 8) & 0xF   // ColorRenderTargetFormat
if rt_fmt == 0 (8_8_8_8) && (D3DFORMAT & 0x0003FE00) == 0x00007E00
   (sign X,Y,Z = 3 = GAMMA, sign W = 0, NumFormat bit 17 = 0; tests at 83FB54C8..83FB54FC)
   -> rt_fmt = 1 (8_8_8_8_GAMMA)
surface+28 = (Base & 0xFFF) | (rt_fmt << 16) | ((ColorExpBias & 0x3F) << 20)
```
That is exactly the RB_COLOR_INFO layout; SetRenderTarget copies it verbatim to the
device shadow (+10372/+10380/+10384/+10388, see Addendum 4).
For 0x1A20AB55 (tf 21, 16_16_16_16_EDRAM) the table should give rt_fmt 5
(k_16_16_16_16) - read the table in guest memory to confirm, don't assume.

## Depth formats (tf 22 = 24_8, tf 23 = 24_8_FLOAT)
surface+28 = (Base & 0xFFF) | (depth_is_float_from_table << 16)
surface+32 = HiZ info: 0 if HiZ base == -1, else (HiZBase << 13 | func bit) << 4 | 1.

## Other fields written (for a faithful simplified surface)
+0 = 4 (resource type: surface), +4 = 1 (refcount), +20 = 0xFFFF0000,
+24 = (msaa | pitch_tiles<<2) << 16 | pitch-derived value (80- or 40-sample tile rounding by msaa),
+36 = (width-1) | (height-1) << 14 | old low 3 bits kept,
+40 = D3DFORMAT, +44 = size_in_tiles * 5120 (size from 82B64CB0), *out_tiles = size_in_tiles.

## Recommendation for CreateSurfaceHook (83FB5690)
Don't reimplement the table guess: fill surface+28 with the formula above using
params (or the NULL defaults) and rt_fmt taken from the same u16 table at 0x824C9660.
Then SetRenderTargetHook can read exp_bias/format from surface+28 consistently, and
the PS output scale is 2^ColorExpBias (/32 extra only for 16_16 and 16_16_16_16).

# Addendum 6 (2026-09-30): who fills placement-shader physical sections

Static analysis of TU23 generated code. Not verified at runtime.

## Answer: CPU memcpy by the game's own pool allocator. Not memexport, not code generation.

Only one game function creates placement shaders: **`82BD73F0`** (the only caller of
XGSetVertexShaderHeader `83FB7618` and XGSetPixelShaderHeader `83FB74F0`).
Arguments: r3 = owner object (its [+8] leads to the pool), r4 = target record (shader
objects stored at +104 VS, +108 PS), r6 = VS container, r10 = PS container (optional),
stack byte 327(r1) picks the path.

### Path A — fresh load (byte 327(r1) != 0)
Per stage (VS first, then PS):
1. `83FB67A0` = XGGetShaderInfo(container, info): info+0 = container, +4 = virtual size,
   **+8 = physical source = container + [container+4]**, **+12 = physical size = [container+8]**.
2. Allocate the SDK object + virtual section (82A4BB98, align 4, tag 55),
   store it in record+104/+108, then XGSet*ShaderHeader(obj, size, info) copies the virtual section.
3. Physical section, pool = `82CE86C0(ctx[+8]) + 3092`:
   - `82CEC738(pool, phys_size, obj, phys_src)` -> allocates a handle (82CEC2B8), then if
     phys_src != 0: lock the block (82CEBFC0), **memcpy(dst, phys_src, phys_size)** (83E45090),
     unlock (82CEC170). This is the ONLY writer of the microcode.
   - `82CEC7D0(pool, handle)` -> lock again, returns the CPU pointer into the block.
   - `83FB7658(obj, ptr)` = XGRegisterVertexShader (PS: `83FB67F0` = XGRegisterPixelShader);
     stores ptr at obj+0x20 (VS) / +0x18 (PS) — the "backing" your hook reads.
   - `82CEC838(pool, handle)` -> unlock.
### Path B — reuse (byte 327(r1) == 0)
`82B7A5D0(owner)` returns an existing entry; the shader objects are **embedded in an
already-filled block** at [entry+4]->[0] + 64; the pool handle is entry+12. The code
locks it (82CEC7D0), re-registers only if a flag byte is set (VS: arg r9, PS: 311(r1)),
unlocks. No memcpy on this path — it relies on the block content from an earlier path-A fill.
It also accumulates sizes into globals 0x8499667C/0x84996680 (lis -31591 +26236/+26240).

## Pool semantics (ownership / lifetime)
Pool = array of 1568-byte block descriptors at [pool+4]; handle = (block << 23) | offset.
Blocks are 64 KB physical allocations (82CEA680(0x10000, 7, 25, 384) on first use).
- lock `82CEBFC0`: under a lock at pool+16, refcount u16 at desc+20 ++; if desc+12 (CPU
  mapping) is 0 it maps via `82CBC020(desc[+0], 16, 0)` and stores the pointer at desc+12.
- unlock `82CEC170`: refcount --; **when it reaches 0 it calls `82CEA7D0(desc[+0], -1, 0)`
  and clears desc+12** (mapping dropped).
So the pointer stored in the shader object is only guaranteed to be *mapped* while the
block is locked; the game releases the lock right after XGRegister*. On real hardware
the GPU uses the physical address, which stays valid; the CPU mapping can go away.

## What to check (in order)
1. Log in 83FB7658/83FB67F0 (or right before): r4 and the first 16 bytes at r4, plus
   the first 16 bytes at info+8 (phys_src). Three outcomes:
   - src zero -> the container's physical section wasn't loaded yet (streaming) — look at
     who produced the container (caller of 82BD73F0);
   - src non-zero, r4 zero -> the memcpy went to a different view than r4 (compare the
     address memcpy wrote to in 82CEC738 with r4; see point 2);
   - r4 non-zero at register time but zero at bind -> content dropped/remapped after
     unlock (82CEA7D0 path) or path B reused a block that was never filled.
2. Physical-view aliasing: the pointer is in the 0xE0000000 view. In xenia that heap has
   a +0x1000 host offset; make sure your read of the backing uses the same
   TranslateVirtual path as the CPU memcpy, NOT (addr & 0x1FFFFFFF) into a physical base
   without the offset. A 0x1000 shift would read neighbouring (possibly zero) pages.
3. Hash at bind time only after confirming content is non-zero; never cache a zero section (agreed).

## Memexport VS A58EBA123EFB541C / BeginExport(8496AF20, format 0)
Not related to the shader fill — nothing in 82BD73F0 or the pool touches GPU export.
Treat it as a separate issue.

# Addendum 7 (2026-09-30): depth resolve flags=4 into the 960x3840 atlas; Resolve ABI

Static analysis of TU23 generated code + shader-identity-game.log. Not verified at runtime.
I could NOT pin down statically which caller produces the atlas copies — see "What to log".

## Resolve ABI (game side)
Game resolves go: caller -> `82BD6800` or `82BD69A0` -> `82BD5BE8` -> `82B64860` -> Resolve `83FBF0D8`
(ResolveEx `83FBDF80` is only reached from inside 83FBF0D8).
`82B64860` only shifts registers and inserts the device, so Resolve receives, XDK order:
`Resolve(device r3, Flags r4, pSourceRect r5, pDestTexture r6, pDestPoint r7, DestLevel r8,
DestSliceOrFace r9, pClearColor r10, ClearZ f1, ClearStencil [sp+0x5C], pParameters [sp+0x64])`.

Flags as the game builds them (82BD6800 / 82BD69A0):
- low bits = source: RT index 0..3, or **4 = depth/stencil** (when the group's slot type [slot+0] == 4);
- **0x10** added when [group+296] != 0 — per SDK this is D3DRESOLVE_FRAGMENT0 (resolve sample 0 only, MSAA);
- **0x100 = D3DRESOLVE_CLEARRENDERTARGET**, added ONLY on the colour path and ONLY when the caller
  passed a clear colour; pClearColor then points to a stack copy of **4 floats (D3DVECTOR4, RGBA)**.
  On the depth path 0x100 is never set and pClearColor = NULL;
- bits 31:26 = exponent bias added in 82BD5BE8 by object type (Addendum 3: 117 -> +11, 118 -> -5);
- ClearZ = float constant at 0x82005CD4 (likely 1.0 — read it), ClearStencil = 0, pParameters = NULL
  in both helpers.
DestSliceOrFace = u32 table at 0x8211377C indexed by the caller's face argument.

## The 82BD6800 helper (single resolve)
`82BD6800(slot r3, pClearColor r4, holder r5, level r6, face r7, pDestPoint r8, pSourceRect r9)`;
dst texture = holder[0][+44] -> the obj+120+52*idx header (Addendum 3). SourceRect and DestPoint are
**passed straight through from the caller** (D3DRECT {x1,y1,x2,y2} / D3DPOINT {x,y}, s32).
After a colour resolve it calls 82BD5C60 (advances the obj ring index); not after depth.
It also rebinds the slot before/after via 82B698A0 when [group+1042] is set.

## Callers that loop (candidates for the 4 transfers)
- `82B690D8`: loop over group tiles (+308 count, rects at [group+1036]+16*i), clamps each rect
  to the texture size (u16 at tex+40/+42), sets predication `3 << 2i` (82B64930) and calls
  82BD6800 with **srcRect = destPoint = the same rect** -> copies tile i to the same place.
- `82B69A78`: tiles x 5 slots (RT0..RT3 + depth at k==4), rects from group+792+16*k.
- `82B6A4C0`, `82B69CE0`: same pattern with explicit DestPoint built on the stack.
None of these obviously produces "960x960 src -> y = 960*i dst" — the atlas placement probably
comes from a caller of these (shadow code) passing a DestPoint. So:

## What to log (cheap, settles it)
In ResolveHook print lr and the ORIGINAL guest pointers' contents:
`*pSourceRect` (4 x s32), `*pDestPoint` (2 x s32), level, slice, and the two frames up
(82B64860's caller = 82BD5BE8, then its caller). Expected for a 4-cascade atlas:
srcRect (0,0,960,960), destPoint (0, 960*i) for i = 0..3, one depth render between each.
If destPoint is always (0,0) the atlas is filled differently (e.g. viewport offsets on a tall RT).

## Destination format / how the PS reads it
- Source DS: D3DFORMAT 0x2D200196 -> tf 22 = **k_24_8 (D24S8, unorm depth)**.
- Destination 0xAC585174: xenos_format 22 (k_24_8), tiled, num 0, sign 0, exp 0,
  swizzle 0xB48 = X, Y, 1, 1 (Xenos swizzle codes 0..3 = XYZW, 4 = 0, 5 = 1).
- Depth resolve copies the 24_8 bits as-is (no numeric conversion; xenia: depth resolve is raw).
- A tfetch of k_24_8 returns depth as unorm24 -> float in X; xenia loads it with
  kLoadShaderIndexDepthUnorm into **R32_FLOAT, swizzle RRRR** (d3d12/texture_cache.cpp:191).
  So the PS sees a plain float depth 0..1 and does its own compare (no hardware PCF on Xenos).
- For native: host D32_FLOAT_S8 source -> write the atlas region as float depth in an R32_FLOAT
  (or D32 SRV) texture at destPoint; if you keep guest-memory layout, convert float -> unorm24 << 8.
  Watch the clear value: ClearZ default 1.0 = far = "not in shadow"; an unwritten atlas (current
  skip) reads as 0 = everything in shadow -> black, which fits the symptom but is NOT proven.

# Addendum 8 (2026-09-30): the memexport pass is a GPU memmove

Static analysis of TU23 generated code. Not verified at runtime; not yet matched against the log.

## Call chain
- BeginExport `83FC53D0` / EndExport `83FC54C8` are called from game code in ONE place:
  **`82BD2868`** (the other caller, 83FC3438, is D3D-internal).
- `82BD2868` is only called by **`82BD2AA0(dst r3, src r4, size r5)`**.
- `82BD2AA0` is called by **`82B57E88`** and **`82B583C0`** — they sit next to the physical
  block pool code from Addendum 6 (82B58928, 82B58DD8, 82B590E8). Most likely the pool's
  compaction/relocation path. (Inference from address proximity, not proven.)

## 82BD2AA0 = GPU memmove(dst, src, size)
1. `83FC5A58(device, src, size, 0)` and `83FC5A58(device, dst, size, 0)` — probably
   D3D InvalidateGpuCache/flush for both ranges (not hooked by native).
2. Binds a fixed VS/PS pair from globals: VS = [0x84992E40], PS = [0x84992E44]
   (SetVertexShader 83FB6A30 / SetPixelShader 83FB6828), then 82B64708([0x84992E48]).
   The VS is presumably A58EBA123EFB541C — check that [0x84992E40] resolves to it.
3. size is rounded up to 64. Copy is done in chunks **from the end backwards**, each chunk at
   most (dst - src) bytes -> overlap-safe for dst > src, i.e. memmove semantics.
4. Final `83FC5A58(device, dst, size_rounded, 0)`.

## 82BD2868 = one chunk (dst r3, src r4, bytes r5)
- Takes the next slot from a ring of **2048** (index at global 0x84992E4C, wraps at 2048).
- Two 32-byte buffer headers per slot, built with `83F9AFD0` (header init, size = bytes)
  + `83F9B640` (set base address):
  - **dst header: 0x8496AE40 + 32*slot** — this is the "export resource" you see
    (8496AE40..8496AF20 = slots 0..7). Plain XG buffer header, not a D3DBuffer created
    through CreateVertexBuffer, so your buffer registry doesn't know it.
  - src header: 0x8497AE40 + 32*slot.
- Builds a 16-byte export stream constant at 0x8498AE40 + 16*slot and copies it into the
  device's VS float-constant shadow at **device+1920..1932** (that's the eA memexport
  address constant): word0 = 0x40000000 | (dst address bits), word1 = 0x4B000000,
  word2 = (old & mask) | 0x4B021A01 (export format/endian), word3 = 0x4B000000 | (size >> 3).
  Dirty bit 63 set in the 64-bit mask at device+0.
- SetStreamSource(0, src header, offset 0, stride 64) via 83FBA160.
- device+1936..1948 = second constant: x = float at 0x82005CD0, yzw = stack leftovers
  (uninitialised in the caller frame — the shader presumably ignores them).
- Loop in batches of <= 16384: BeginExport(0, dst header, 0) ->
  **DrawVertices(POINTLIST, start, count)** with count = bytes / 64 -> EndExport(0, dst header, 0).
- Unbinds stream 0, then `83FC5A58(device, src, bytes, 0)`.
So each point = one 64-byte record (four float4 fetches), exported unchanged to dst + 64*index.

## Readiness / ordering
It's a GPU copy in command order: everything submitted before it that writes src must finish
first, and every later draw that reads dst sees the new data. The CPU side does not wait
(apart from the cache ops). If this pass is skipped, the relocated memory at dst keeps stale
or zero contents while the game already uses the new address -> garbage vertex/index data
-> "huge triangles" is consistent with this, but NOT proven.

## CPU equivalent
`memmove(dst, src, round_up(size, 64))` in guest memory at the point in the command stream
where 82BD2AA0 runs, then invalidate any host mirrors (vertex/index/texture/shader caches)
covering dst. Since native executes draws immediately, doing it synchronously in a hook on
82BD2AA0 matches the GPU ordering, as long as no queued host work still has to read the old
src/dst contents. Byte order: export format 0x1A01 plus the vertex fetch are expected to be a
straight bit copy (same endian in and out) — confirm on one call by comparing src and dst
after a real run (e.g. xenia stock) before relying on it.

## Suggested check
Hook 82BD2AA0 (log dst/src/size and lr) and see whether the addresses match the vertex/index
buffers of the characters and menu background that break.

# Addendum 9 (2026-09-30): sampler state = the Xenos fetch constant shadow

Static analysis of TU23 generated code. Bit positions below are LSB-numbered (xenia
xe_gpu_texture_fetch_t, rexglue-sdk include/rex/graphics/xenos.h:1168).

## Key finding
TU23 D3D has NO separate sampler object. SetSamplerState edits the **real Xenos texture fetch
constant** in place: `F(s) = device + 0x480 + 24*s` (6 dwords d0..d5, s = D3D sampler index,
26 slots). SetTexture merges the texture header's fetch words into the same 6 dwords while
keeping the sampler bits. So the correct native solution is NOT to translate D3D sampler
states one by one: read the 6 dwords at F(s) at draw time and derive the host sampler exactly
like xenia does from a fetch constant (clamp_x/y/z, mag/min/mip filter, aniso, lod_bias,
mip_min/max_level, border_color). Unleashed does the same thing from its data0/3/5.

Dirty tracking: every setter ORs `(1<<63) >> (s+32)` into the u64 at device+24.

## Setup (device init 83FC9450, NOT hooked)
Table at **0x847F9FD8**: 20 entries x 12 bytes {+0 getter, +4 setter, +8 default}.
For k = 0..19: device+0x3B8+4k (952) = getter, **device+0x1D4+4k (468) = setter**; then for
every sampler s it calls setter_k(device, s, default_k); finally SetTexture(s, NULL).
The defaults are data — read the 20 u32 at 0x847F9FE0 + 12k from guest memory at runtime.
Setter k order follows D3DSAMPLERSTATETYPE; the addresses below identify each by the bits it writes.

## Setters (all `(device r3, sampler r4, value r5)`), what they write in F(s)
| Setter | Field | Bits | Rule |
|---|---|---|---|
| 83FB9DB8 | AddressU | d0 10..12 clamp_x | raw value (X360 D3DTADDRESS == Xenos ClampMode 0..7) |
| 83FB9E08 | AddressV | d0 13..15 clamp_y | raw |
| 83FB9E58 | AddressW | d0 16..18 clamp_z | raw |
| 83FB97E8 | MagFilter | d3 19..20 mag_filter, d3 25..27 aniso, d4 10 mag_aniso_walk | value&3 -> filter; (value>>2) = anisotropic flag; aniso from table 0x824CAF70[byte device+10864+s] |
| 83FB9640 | MinFilter | d3 21..22 min_filter, d3 25..27 aniso, d4 11 min_aniso_walk | same scheme |
| 83FB9988 | MipFilter | d3 23..24 mip_filter | raw |
| 83FB9A88 | MaxAnisotropy | d3 25..27 aniso_filter | table 0x824CAF70[value] (log2), only if an aniso walk bit is set; value kept in byte device+10864+s |
| 83FB9BA8 | MipMapLodBias | d4 12..21 lod_bias | float value * const @0x8202776C, truncated (s10) |
| 83FB9C48 | MaxMipLevel | d4 2..5 mip_min_level | max(value, bound texture's level); request kept in byte device+12356+s |
| 83FB9CC8 | MinMipLevel/"max level" | d4 6..9 mip_max_level | min(value, texture's max); request in byte device+12382+s |
| 83FB9B00 | AnisotropyBias | d5 5..8 aniso_bias | float * const @0x8204EA60 |
| 83FB9D48 | BorderColor | d5 0..1 border_color | (value != 0) -> 1 (Xenos only has black/white-ish presets) |
| 83FB9EA8 | TrilinearThreshold | d5 3..4 tri_clamp | raw |
| 83FB9FB0 | (force BC W to max) | d5 2 | raw |
| 83FB9F00 / 83FB9F58 | H / V gradient exp bias | d4 22..26 / 27..31 | raw (s5) |
| 83FBA008 | (clamp policy) | d1 11 nearest_clamp_policy | = (value == 0) |
| 83FB98E8 / 83FB9740 / 83FB99E0 | separate-Z / volume filter flags | byte device+10890+s bits 0/1/2, folded into d4 0..1 vol_mag/vol_min | |
Getters 83FB99C8, 83FB9B60, 83FB9C08, 83FB9D90, 83FB9DF0, 83FB9E40, 83FB9E90, 83FB9EE8,
83FB9F40, 83FB9F98, 83FB9FF0, 83FBA050 read the same fields back. (Names for the last few rows
are inferred from the bit they touch — confirm the k index against the table if it matters.)

## What SetTexture (83FB58A8, which native hooks) must preserve
`SetTexture(device, s, tex r5, dirty r6)`; texture header fetch words are tex+28..tex+48 (T0..T5).
Stores texture pointer at device+0x3278+4s (device+12920). Merge rules, bit-exact:
- d0 = T0 with **bits 10..21 kept from F** (clamp_x/y/z + pad).
- d1 = (T1 & 0x1FFFFFFF) + 0x1000-in-address if T1's address >= 0xE0000000
  (i.e. `((T1>>20)+0x200) & 0x1000`), with **bit 11 kept from F** (nearest_clamp_policy).
  -> D3D itself applies the 0xE0000000 physical +4 KB offset here (relevant to Addendum 6).
- d2 = T2 (size).
- d3 = T3 with **bits 19..30 kept from F** (mag/min/mip/aniso/arbitrary filters);
  texture supplies num_format, swizzle, exp_adjust (0..18) and border_size (31).
- d4 = T4 bits 2..9 (mip levels) + **bits 0..1 and 10..31 kept from F**; then
  mip_min_level = max(T4.min, byte 12356+s), mip_max_level = min(T4.max, byte 12382+s).
- d5 = T5 bits 9..28 (dimension, packed_mips, mip_address, same 0xE0000000 fixup)
  + **bits 0..8 kept from F** (border_color, force_bc_w, tri_clamp, aniso_bias).
- tex == NULL: F.d0 &= ~3 (fetch type 0 = invalid), nothing else changes.
- device+24 |= r6 (caller-supplied dirty bit).

## Recommended native fix
1. Keep the original setters running (they are not hooked) — the guest shadow at F(s) is then
   always correct for sampler bits.
2. In SetTextureHook either call the original merge or reproduce the rules above exactly;
   the current hook presumably overwrites/ignores F(s), losing sampler bits.
3. At draw: for each sampler the shader uses, read F(s), build a sampler key from
   clamp_x/y/z, mag/min/mip_filter, aniso_filter, lod_bias, mip_min/max_level, border_color
   and reuse xenia's fetch-constant -> host sampler mapping (D3D12 TextureCache::GetSamplerParameters
   in rexglue-sdk). Cache host samplers by key; there are few distinct ones.
4. VS vs PS: setters and SetTexture index F(s) by the D3D sampler index unchanged; which s
   values the VS uses (D3DVERTEXTEXTURESAMPLER*) and how s maps to the shader's tfetch slot
   was not determined here — take it from the shader's fetch-constant indices in your AOT data.

# Addendum 10 (2026-09-30): replaying sampler init without 83FC9450; VS sampler path

Static analysis of TU23 generated code. Not verified at runtime.

## Exact original order (83FC9450, after the render-state part)
```
for s in 0..25:                      // cmplwi r28,26
    for k in 0..19:                  // table 0x847F9FD8, 12-byte entries
        device[0x3B8 + 4k] = entry_k.getter   // written every s (harmless repeat)
        device[0x1D4 + 4k] = entry_k.setter
        entry_k.setter(device, s, entry_k.default)   // default = u32 at 0x847F9FE0 + 12k
    SetTexture(device, s, NULL, (1<<63) >> (s+32))   // 83FB58A8
```
So it's sampler-major (all 20 states for slot 0, then slot 1, ...), each slot ended by
SetTexture(NULL). Your plan matches; keep the SetTexture(NULL) step via your hook
(its only effect on a zeroed device: F(s).d0 &= ~3, store NULL at device+0x3278+4s,
device+24 |= dirty). Right after the loop the original continues with render-state
setup (12568 = 5, 12572 = 1, 83FAFC78(2), 83FB7840(2), ...) — you're skipping that, fine.

## Queue safety of the 20 setters with texture == NULL
All setters I identified (Addendum 9 table) are leaf functions: they only read/modify
F(s) = device+0x480+24s, the bytes device+10864+s / +10890+s / +12356+s / +12382+s,
the u64 dirty mask at device+24, and static data (0x824CAF70 table, float constants at
0x8202776C / 0x8204EA60). The only calls are the GPR save/restore helpers
(0x83E44F7C / 0x83E44FCC) in 83FB9640 and 83FB97E8. No command-buffer (13928), fence,
KickOff or queue access.
MaxMipLevel/MinMipLevel setters (83FB9C48 / 83FB9CC8) read the bound texture pointer at
device+0x3278+4s and **skip the write when it is NULL** — so on a zeroed device those two
only store the request byte (12356+s / 12382+s) and leave d4 mip fields 0 until SetTexture.
Caveat: I did not read the table itself (it's guest data). Before relying on this, check at
runtime that all 20 setter pointers are within the set listed in Addendum 9
(83FB9640..83FBA008 range). Any pointer outside it — inspect before calling.
Note the original SetTexture (not the setters) does touch the command buffer, but only when
the PREVIOUS texture is non-NULL (fence/reference path at device+11036/11040/13928).

## VS / PS sampler -> tfetch constant index
device+0x480 is the start of the **Xenos fetch constant file** itself, not a D3D-private copy:
SetVertexShader 83FB6A30 and SetPixelShader 83FB6828 both memcpy the shader's embedded
fetch-constant patches into `device+1152 + offset` (records {u16 byte offset, u16 dword count}
from the shader object, second loop merges masked words). That is how vertex-fetch constants
(vfetch streams) get written into the same file.
Consequences:
- Texture fetch slot n occupies bytes 1152 + 24n .. +23. D3D sampler index s == fetch
  constant index n for BOTH VS and PS. The tfetch "const" index in the microcode is the slot
  to read, no remapping.
- Slots 0..25 are D3D samplers (26 = what init touches); bytes of slots 26..31
  (device+1776..1919) are left for vertex fetch constants written by shader binds.
  (Memexport address constants start at device+1920 = the float constant file, Addendum 8.)
- For PS, 0..15 as you expect. For VS, take the tfetch const index straight from the VS
  microcode in your AOT data and read F(that index); by the rule above the D3D-side slot is
  the same number (on 360 the vertex texture samplers sit above the 16 PS slots — expect
  16..19 — but I did not find the constant in code, so trust the microcode index).
- Watch for the shader-bind memcpy: if native hooks SetVertexShader/SetPixelShader, those
  embedded fetch patches must still be applied to device+1152, or vfetch (and any
  texture fetch a shader pre-fills) will read stale words.

# Addendum 11 (2026-09-30): dynamic (CPU-written) textures rebuild their header on every lock

Static analysis of TU23 generated code. Not verified at runtime (compare headers as you planned).

## VS slots 16/17 come from the game's render-texture objects
`82BCE7F8(stage r4, slot r5, obj r6, sampler_desc r7)` is the game's "bind texture + sampler":
- stage 0 (vertex) -> **D3D slot = slot + 16** (confirms VS samplers 16..19, Addendum 10);
- it writes AddressU/V/W straight into F(s) and calls the Addendum 9 setters for the rest
  (MinFilter 83FB9640, MagFilter 83FB97E8, MipFilter bits, LOD bias, MaxAnisotropy,
  BorderColor, Min/MaxMipLevel);
- then SetTexture(s, **obj + 120 + 52*[obj+116]**) — the same header ring as in Addendum 3.
Callers: 82BCB7F0, 82BCBAA8, 82BCBB78.
So AB1AE148 / AB1AE238 are header slots inside two such objects, not CreateTexture textures.

## Header lifetime: rebuilt on every CPU lock — found the initializer missing in Addendum 5
`82BD6088(obj, flags)` = lock-for-CPU-write (callers 82BD4520, 82BD5498):
1. if obj+36 bit0 (double-buffered): **obj+116 = 1 - obj+116** (ping-pong between headers
   at obj+120 and obj+172);
2. BlockUntilNotBusy(header) 83FBF900 (waits for GPU to finish with that buffer);
3. **`82BD58D0(obj, idx, ...)` rebuilds the whole header** at obj+120+52*idx through XG:
   2D `83F9B490`, cube `83F9B508`, volume `83F9B578`; size from u16 obj+40/+42(/+44),
   levels byte obj+46, format obj+96; exp bias arg = -13 if type [obj+12] == 117, +5 if 118
   (matches the resolve exp-bias pairing in Addendum 3);
4. locks the backing pool block (handle obj+16, 82B58898 — the same pool family as Addendum 6/8)
   and gets the CPU address via 82BD5C80(handle, 4096) -> obj+20;
5. **`83F9B5D8(header, base, mip_base)`** sets base (and mip) address in the header;
   also stored at obj+224+4*idx;
6. optional 82BD5D20(handle, -1) afterwards.
Unlock/commit is `82BD5AA8` (records obj, size, idx and the written range in globals
0x849965D0..0x849965E4; also calls 82BD58D0 on some paths).

So between two binds of "the same" header address:
- the header fetch words T0..T5 (header+28..+48) are rewritten (base address can change
  when the pool relocates the block — see Addendum 8's GPU memmove), and
- the texel data at base is rewritten by the CPU every lock (bone/instance data for
  VS F5871392703E06C0, 1024x128 32_32_32_32_FLOAT, tiled).
With double buffering the two headers alternate each frame.

## What native should do
- Identity: key host textures on the decoded fetch words (T1 base, T5 mip base, T2 size,
  T3 format/swizzle/exp), **re-read at every SetTexture**, not on the header pointer.
  resource.common (header+0) is just type/refcount — not needed for identity.
- Content: these are CPU-written each frame; re-upload (or hash-check) on every bind, or at
  least after each 82BD5AA8 unlock for that obj. A first-bind cache will show frame-1 data
  forever -> skinned characters degrading over time fits, but confirm with the header compare.
- Same class of object also backs resolve targets (Addendum 3: GPU-written via Resolve).
  For those, invalidate the host copy on Resolve instead of re-uploading from guest memory.
- Likely relevant to the menu "lighting/blur" corruption as well: any render-texture obj
  sampled after a flip/resolve with a cached first-bind copy will show stale content.
  Not verified.

# Addendum 12 (2026-09-30): who writes animation / VB / IB data, and what changes over time

Static analysis of TU23 generated code + rexglue-sdk sources. Not verified at runtime;
logs/images named in the question were not inspected here.

## 1. Paths that write guest data (and whether the write watch sees them)
a) **Guest CPU stores** (recompiled code, incl. guest memcpy 0x83E45090 / memmove 0x83E467E0):
   ordinary host stores into the protected views -> the access-violation path fires the
   invalidation. This is how the vertex-texture data (Addendum 11, 82BD6088 lock -> CPU write
   -> 82BD5AA8 commit) and dynamic VB/IB contents are written. No DMA, no export for these.
b) **The game's pool changes page protection itself**: `82B55588` -> `82BD2808` ->
   `83F9F4D8` -> `MmSetAddressProtect` (the only game caller besides 82AB2800).
   Protect value 4 (READWRITE) or 0x404 (READWRITE|WRITECOMBINE), on block lock/unlock and
   during relocation. Callers of 82B55588: 82B556D8, 82B56480 (unlock path), 82B57880,
   82B57B00, 82B57E88, 82B583C0 (relocation), 82B590E8 (lock path).
   SDK: `MmSetAddressProtect_entry` -> `PhysicalHeap::Protect` (xmemory.cpp:2112) calls
   TriggerCallbacks for the range **when the new protection is writable**, then
   `BaseHeap::Protect` rewrites host page protection for that view. So the SDK notifies —
   but it also **overwrites your read-only watch protection** on that view. If your cache
   re-arms by setting protection itself and assumes it stays until the next fault, a later
   MmSetAddressProtect silently resets it; the SDK then only tracks via its own watch flags.
   Make sure your cache relies on the SDK's watch (EnablePhysicalMemoryAccessCallbacks),
   not on its own VirtualProtect, and re-arms after each callback.
c) **Host code using TranslatePhysical** (bypasses the A/C/E views entirely):
   in the SDK only `audio/xma_context.cpp` (XMA input/output, not geometry) and
   `system/xfile.cpp` (you said it notifies). In **native** code: `gpu_native/hooks_buffer.cpp`,
   `textures.cpp`, `memory_watch.h` — i.e. your own resolve writeback / memexport-CPU
   memmove / uploads. Any native write into guest memory must invalidate your own cache;
   the watch will not see it.
d) **Ordering race (most likely cause for a single bad animation line):** re-arming the
   watch AFTER copying guest data to the host loses any CPU write that lands in between
   (the game's worker threads write skinning/instance data concurrently with the render
   thread). xenia's order is: enable watch (protect) FIRST, then read the data; a fault during
   the copy just marks it dirty again. Check that your cache does it in that order, per page,
   and that a callback arriving during the copy isn't cleared by the "upload done" step.

## 2. Title screen after ~2 minutes (also with the cache off)
The pool's relocation/compaction runs over time: `82B592A8` calls `82B57E88` and `82B583C0`.
Each relocation:
- moves the block **either by CPU (memcpy 83E45090 / memmove 83E467E0) or by GPU
  memmove 82BD2AA0** (Addendum 8) — both paths exist in these two functions;
- changes page protection (82B55588, point 1b);
- calls **virtual callbacks on the owner (bctrl)** — the owner then rewrites its resource
  headers with the new address (for textures: 82BD58D0/83F9B5D8 as in Addendum 11; for
  buffers the same XG header setters 83F9AFD0/83F9B640 as in Addendum 8).
So after a relocation the **same header pointer holds a new base address**, and the old
address may be reused for something else. With the cache off, geometry still breaks if
any native object keeps the base address it read at creation/first bind (VB/IB registry,
vertex-declaration streams, adopted textures). Idle on the title screen is exactly when the
pool has time to compact.
Fix direction: for VB/IB/textures, re-read the header's base (VB/IB: the 32-byte XG buffer
header; textures: T1/T5) at every SetStreamSource/SetIndices/SetTexture and key host
resources on (base, size, format), not on the header pointer.
Quick confirmation: log calls to 82B57E88 / 82B583C0 (src, dst, size) and see whether the
title breakage starts right after the first one.
I found no separate "title idle" render pass; the attract/idle logic was not traced.

# Addendum 13 (2026-09-30): relocation callbacks and 83F9B640 (XGOffsetResourceAddress)

Static analysis of TU23 generated code. Not verified at runtime.

## Yes — 83F9B640 is called from the pool's relocation callbacks
83F9B640(header r3, delta r4) itself calls **GetResourceType 83FC49E8** and dispatches:
- type 1 -> header+24 += delta, **low 2 bits of +24 kept** (rlwimi 30..31)
- type 2, 7, 8 -> header+24 += delta (plain add)
- type 6 -> header+32 += delta
- type 3 and 17..20 (textures) -> 83F9B5D8(header, delta, mip_delta if header+48 has a mip address)
- anything else -> no-op
So a wrong GetResourceType answer for a borrowed VB (6 instead of 1) makes every relocation add
the delta to header+32 and leave the real base at +24 stale -> exactly "same header, old address"
(Addendum 12). Your fix (override only for native-owned simplified headers, borrowed ->
original leaf) matches this contract.

## How callbacks are registered
`82B56500(pool, handle, callback, userdata)`: stores callback at +40 and userdata at +44 of the
handle's 52-byte descriptor (max 9216 handles, under the lock at pool+944). The relocation code
(82B57E88 / 82B583C0, via bctrl) calls `callback(phase r3, old r4, size r5, new r6, userdata r7)`:
- phase 0 = before the move: switch buffer / BlockUntilNotBusy on the resource(s)
- phase 1 = cache flush (dcbf) over the old range (only 82B7A500 implements it)
- phase 2 = after the move: 83F9B640(header, new - old)

## Owners that go through this chain (found by who takes the callback's address)
| Callback | Registered by | Owner / headers relocated |
|---|---|---|
| **82CB9120** | **82CBCBE8** | double-buffered **vertex buffer** owner: two XG VB headers built with 83F9AFD0 at owner+24+32*idx; idx = s16 at owner+4; current address kept at owner+88+4*idx. Phase 0 flips idx and waits (83FBF900), phase 2 offsets header[idx] and updates the stored address |
| **82CB9070** | **82CBC9E0** | same pattern for **index buffers** (headers built with 83F9B068, idx at owner+12) |
| **82B7A500** | **82B7D0D0** | placement-shader groups (Addendum 6): walks a list at [userdata+12] (next at +60) and offsets the VS at +104 and the PS at +108 — **shaders move too**, so the shader backing pointer at obj+0x20/+0x18 changes |
| 82CE86D0 | not found by address scan (registered some other way) | a single header; offsets it only if header byte +3 is 6 or 7 |
Both buffer owners allocate their blocks with 82CEA680 + 82CBC020, i.e. the same 64 KB pool.

## Implications for native
- Borrowed VB/IB/shader/texture headers can change base at any phase-2 callback; re-read the
  base per bind (Addendum 12) — and your shader-by-backing lookup (Addendum 6) needs the same
  treatment: the physical section moves, and its content is copied by the relocation.
- 83F9B640 must see the true guest type, so GetResourceType must stay original for anything
  not created by native.

# Addendum 14 (2026-09-30): InsertCallback, CPU waits, and leads for hands / portal / blur

**Everything below is STATIC analysis of TU23 generated code. Nothing here was verified in
game.** The session-20260930 artifacts named in the question were not inspected.

## 1. InsertCallback 83FC0F38 — what the callbacks do

### 82BCDE68 and 82BCDF30 = GPU profiler timestamps (static)
- Both take the context in r3 as `(group << 16) | marker`, index a table through the pointer
  at global **0x849677B8** (`[0x849677B8] + 4*(1504 + 1122*group + marker)` = per-marker
  counter, capped at 32), and call **83F9EA20 = `mftb` -> store u64 timebase**.
  So each callback records "the CPU time at which the GPU reached this point".
- 82BCDE68 = begin marker (group 0 also clears the counters for all groups first).
- 82BCDF30 = end marker; for group 0 it then sums (end - begin) timestamp differences into
  per-group totals (fields around +6200/+6328/+6396 and +128/+320 of the table).
- They touch nothing else: no resource refcounts, no frees, no pool, no animation data.
- Other functions also read through the pointer at 0x849677B8 (82B3B278, 82B3EC68,
  82B79938, 82B79D00, 82B7A420, 82B7A488, 82B7ABF0, 82B9B3F8). I did not check whether any
  of them read the *timing* fields (e.g. for adaptive quality) or unrelated fields of the
  same block. If none reads the timings, these callbacks are profiler-only.

### Semantics to emulate
XDK InsertCallback = "call this when the GPU reaches this point in the command stream".
It never makes the CPU wait. The wrapper 83FC0F38 only kicks the command buffer if it's full
(83FC0C10) and records the callback (83FC02C0). Your current "wait for all host fences, then
call synchronously" turns every marker into a full GPU flush, which is the 29–32 ms.
Safe options, cheapest first:
a) For 82BCDE68 / 82BCDF30: call them immediately at insert time (CPU timestamp instead of
   GPU timestamp). Timings become meaningless but nothing depends on them — **conditional on
   the readers above not using the timing fields**. Log that one first.
b) General case: queue (callback, context) with the host fence of the current submission and
   run it when that fence completes (poll at the next submit/Present, or on a worker thread
   if the callback is thread-safe). Keep the order of callbacks.
c) Never block the game thread in InsertCallback itself.

### Other InsertCallback users (static)
- **83FBD2D8 (inside EndTiling 83FBD098)**: callback **83FCAA80**, context = physical address
  of the device buffer at device+13760. 83FCAA80 pushes the context into a per-core ring and
  signals it through import 0x841132FC — this is the D3D worker-queue path (next to
  StartWorkerQueue 83FCAB10). Native hooks EndTiling, so this is not reached; do not emulate.
- **83EB711C (inside 83EB6D68)**: after a 6-vertex TRIANGLELIST draw, InsertCallback with
  flags from a stack table indexed by the current hardware thread (83F9EEC8 = `lbz 268(r13)`),
  callback **83EB4480** = `this->vtable[23]()` (offset 92), context = the 83EB6D68 object.
  83EB6D68 is also one of the SetTexture callers (Addendum 11 list), so it's a helper renderer
  (likely UI/movie/XUI) whose virtual 23 runs when the GPU is done with its draw. I didn't
  resolve what vtable[23] does; to be safe, run it at the matching host fence (option b), not
  earlier.

## 4. BlockUntilNotBusy 83FBF900 (static)
`83FBF900(resource)` -> `83FBF7D0(device, fence = resource+8, reason 5, resource)`.
BlockOnFence 83FBF8A0 is the same with reason 6 and an explicit fence.
There is **no CPU-worker contract** in it: it waits purely on the GPU fence stored in
resource+8. D3D writes that field when a resource gets referenced by queued GPU work (e.g. the
end of the original SetTexture stores the current fence into the previously bound texture's
+8, device+11036/11040 path).
So for native:
- a resource whose data native already copied into an immutable host snapshot is "not busy"
  for the guest, and waiting can return immediately — **unless** some native path still reads
  guest memory for it later (lazy texture upload, resolve destination, memexport destination);
- for those, wait only for the host fence of the last submission that used that resource,
  not for all fences.
- 82BD6088 (Addendum 11) and phase 0 of 82CB9120/82CB9070 (Addendum 13) use it only to make
  sure the GPU is done with the buffer before the CPU rewrites it or the pool moves it.
  With snapshot semantics, that's satisfied once your snapshot exists.
Your measurement (removing these waits barely helped) fits: the cost is in InsertCallback.

## 2. Vortech's disappearing hands — where to look (static leads, not analysed in depth)
Per-part data sources that can differ between body and hands:
- vertex textures (VS slots 16..19, Addendum 11): written after lock 82BD6088, committed by 82BD5AA8,
  double-buffered (header index flips each lock);
- double-buffered dynamic VBs/IBs owned through 82CBCBE8 / 82CBC9E0 (Addendum 13): two
  headers, index flips in relocation phase 0 as well;
- GPU memmove relocations 82BD2AA0 (Addendum 8) and CPU relocations 82B57E88 / 82B583C0;
- placement shaders relocate too (82B7A500): a hands-only shader whose backing moved would
  fail the lookup or use stale code.
Suggested runtime check: for the draws of the hands, log per draw the VS hash, all stream
header addresses + their bases, vertex texture header addresses + T1 base, and compare the
same draw between a good and a bad frame (and with stock xenia if possible).
I did not trace the game's skinning code itself.

## 3. Portal layer, black/transparent objects, missing HUD icons, blur (static leads only)
- **Alpha test.** Xenos alpha test lives in RB_COLORCONTROL (function + reference in
  RB_ALPHA_REF), not in the shader. D3D12 has no fixed-function alpha test, so it has to be
  emitted in the PS (xenia does this with a system constant). HUD hearts/coins and portal
  energy layers are typical alpha-tested/alpha-blended draws. Check whether native applies
  it and the alpha-to-coverage bit.
- **Blend state.** RB_BLENDCONTROL0..3 per RT, plus RB_COLOR_MASK (device+10460, Addendum 4).
  A missing blend or a zero write mask gives "invisible" or "black" objects.
- **Blur / DoF:** the shipped xenia-based build **turns DoF off by shader hash
  CFEAC7ADB912F8A9** (release 0.1.17). The released game never showed that DoF pass, so
  "sharp in Xenia" is partly because of that. Compare native against a xenia run with the
  DoF cvar disabled before treating the blur as a native bug. Also check the exp bias
  (Addendum 3/4): a wrong ×32 or /32 scale in the half/HDR chain changes the CoC input.
- Tonemap PS 3A47E5DDE66B42C6, c12 and slot 3: not analysed (needs the microcode
  disassembly). Values to compare with xenia at the same frame: c12, the fetch constant of
  slot 3 (6 dwords at device+0x480+24*3), and the size/format of the texture bound there.

# Addendum 15 (2026-09-30): alpha-test leaves (correction) and the tonemap mix

**All of this is STATIC: raw TU23 words from `xexdump/dump/default.bin` plus the translated HLSL
`out/native-gpu/session-20260930/bank118-candidate/hlsl/ps-3A47E5DDE66B42C6.hlsl`. None of it was
run in game.** I also agree that alpha test should not be named as the cause of the portal/hands bug yet.

## 1. Correction: two of the three setters are not alpha test
The device shadows the register block starting at 0x2200 in order, so the offsets map like this:
`+10548` RB_DEPTHCONTROL (0x2200), `+10552` RB_BLENDCONTROL0 (0x2201), **`+10556` RB_COLORCONTROL
(0x2202)**, `+10560` RB_HIZCONTROL, `+10564` PA_CL_CLIP_CNTL, **`+10568` PA_SU_SC_MODE_CNTL (0x2205)**.
- `83FB7DA0` = `lwz r11,+10568; rlwimi r4,r11,0,0,28; stw r4,+10568; dirty[16] |= 0x40`
  -> PA_SU_SC_MODE_CNTL bits 0..2 = cull_front / cull_back / face. This is **CullMode**
  (d3d-hook-map.md already has this). Getter 83FB7DC0 = `&7`.
- `83FB7DD0` = `rlwimi r11,r4,3,21,28` -> bits 3..10 = poly_mode + front/back ptype. This is **FillMode**,
  not the alpha reference. Getter 83FB7DF0 = `(x>>3)&0xFF`.
Don't wire either of them into alpha test.

## 2. The real alpha-test leaves (RB_COLORCONTROL at +10556, dirty[16] |= 0x200)

**Historical correction (2026-10-07):** this section originally misidentified
`83FB92B8 / +10620` as ALPHA_REF. That was an unrelated float state (descriptor
index 59), and reading its default `1.0` discarded the native title-menu button
icons. The actual TU23 alpha reference is descriptor index 25,
`83FB8260 / +10500`. The mapped, patched TU23 image verifies its setter words,
descriptor and normalization constant; see
[`TU23-alpha-reference-byte-proof.json`](../../.local-testing/reports/TU23-alpha-reference-byte-proof.json).

| Setter / getter | Word | Field |
|---|---|---|
| **83FB82C0 / 83FB82E0** | `rlwimi r4,r11,0,0,28` -> bits 0..2 = r4 | **ALPHA_FUNC** |
| 83FB7E00 / 83FB7E28 | bit 3 = r4&1 (also dirty[16] \|= 0x40000) | **ALPHA_TEST_ENABLE** |
| 83FB93B8 / 83FB93D8 | bit 4 = r4&1 | ALPHA_TO_MASK_ENABLE |
| 83FB93E8 / (none found) | bits 24..31 = r4 | ALPHA_TO_MASK_OFFSET0..3 (2 bits each) |
| **83FB8260 / 83FB8298** | unsigned r4 -> float; multiply by float at `0x82005D78`; `stfs f0,+10500`; dirty[16] \|= `0x08000000` | **ALPHA_REF (integer / 255)** |
| 83FB92B8 / 83FB92E0 | `stw r4,tmp; lfs f0,tmp; stfs f0,+10620`; dirty[24] \|= 1<<48 | Unrelated float state, descriptor index 59 |

Leaf contract:
- The function value goes into bits 0..2 **raw**, so the enum is the hardware CompareFunction:
  0 NEVER, 1 LESS, 2 EQUAL, 3 LEQUAL, 4 GREATER, 5 NOTEQUAL, 6 GEQUAL, 7 ALWAYS
  (360 D3DCMP_* use these values, not PC's 1..8). Emit all 8, not only `>=`.
  Xenos semantics: the pixel passes if `alpha FUNC ref` holds, so GEQUAL keeps `oC0.w >= ref` (clip when
  `oC0.w < ref`, which matches your current code). NEVER/ALWAYS = discard all / no-op.
- The actual reference leaf `83FB8260` receives an **unsigned integer**, converts it to
  float and multiplies by the TU23 constant at `0x82005D78`: bytes `3B 80 80 81`,
  float32 `1/255`. The normalized float at `+10500` is ready for the shader comparison;
  do not normalize it again. The gameplay alpha helper `82BCF720` calls this setter
  with integer references. The previous float-bits/no-conversion claim applied to
  `83FB92B8`, which is not the alpha-reference setter.
- Compare in the shader against the value **before** the EDRAM/exp scale (`g_ColorOutputScale`).
  That's where your clip already sits. Xenos tests the shader's oC0.w against RB_ALPHA_REF before the RB format conversion.
- The alpha-to-mask bits are a separate feature (the A2C spec bit). If bit 4 is set and native ignores it,
  foliage/hair-style cutouts render as solid or vanish depending on blend.

Verified TU23 descriptor defaults: index 24 is enable=0, index 25 is integer ref=0,
and index 26 is func=ALWAYS(7). Native CreateDevice initializes these through the
actual leaf setters, including `83FB8260`, after validating the descriptor addresses.
This does not establish the separate alpha-to-mask state or repair other visual defects.

## 3. Tonemap PS 3A47E5DDE66B42C6: how sharp, "mip" and slot 3 combine
Slots from the descriptor table: 0 `fullColor_tex`, **3 `blur_tex`**, 4 `mipColor1_tex`, 6 `noiseTex`,
7 `cubeTex` (3D LUT).
```
uv    = TEXCOORD0.xy + noise-distortion * TEXCOORD4.x        // heat-haze style offset
full  = fullColor(uv).rgb          // sharp scene
mip   = mipColor1(uv)              // downsampled scene; mip.a = per-pixel CoC/blur factor
t     = 2 * (mip.a * c12.x + c12.y)
w     = saturate(t - 1)
col   = lerp(full, mip.rgb, w)     // <- the DoF mix: sharp vs the mipColor1 downsample
col  += blur(uv).rgb^2             // <- slot 3 is ADDED (squared), bloom-like, not a DoF lerp
col   = filmic curve(col * c11.y exposure, consts 0.5/5/0.4/0.12/0.03/0.004)
oC0.rgb = cubeTex LUT(saturate(col))
oC0.a = (saturate(t) > c12.z) ? saturate(t) : luma(col) - 0.0892
```
- **Sharp output: w = 0 <=> mip.a*c12.x + c12.y <= 0.5.** c12 = (0, 0, *, *) or anything with
  c12.x = 0 and c12.y <= 0.5 gives a pure `full` image. c12.w is unused.
- So the blur source is **mipColor1 (slot 4) plus its alpha**, not slot 3. Slot 3 only adds
  glow: if it's garbage or unresolved you get a haze/wash, not DoF. It should be black or dim where nothing glows.
- Consequence of filtering CFEAC7ADB912F8A9: whatever normally writes mipColor1's alpha (or the blur chain)
  no longer runs. In xenia that texture keeps its last or cleared contents. In native, check what
  mip.a actually holds. **Stale or uninitialised alpha with c12.x != 0 produces exactly "blurry
  everywhere".** Compare at the same frame: c12, c11.y, and the resource plus alpha content bound at slots 3 and 4, native vs xenia.
- ABI624 / the explicit-LOD752 candidate: this shader samples everything with plain tfetch2D and no
  LOD, so the candidate doesn't matter for this pass (static, from the HLSL above).

# Addendum 16 (2026-10-01): Vortech's arms, the TT Games logo, draw entry points, stencil

**Everything here is STATIC: the TU23 recompiled sources (`rexlego/generated/default`, asm comments),
GAME.DAT entries extracted with `modcli extract`, and container hashes computed the same way as
`HashShaderContainer` (XXH3_64 over `virtual_size + [4] + physical_size`). The game was not run.
Nothing below names alpha or stencil as the cause.** The hash rule is confirmed: several of the hashes
below appear as `adopted vertex shader ... hash=` in your own session logs.

## 1. Vortech's arms / hands are separate models
The large Vortech isn't one mesh. GAME.DAT has separate characters:
`chars\misc\vortech_body\vortech_body_nxg.ghg`, **`chars\misc\vortech_arms\vortech_armleft_nxg.ghg`**,
**`vortech_armright_nxg.ghg`**, plus `vortech_hand` (a boss character, `Addon "BossWWCVortechHandAddOn"`,
the same shader bank as the arms), `vortech_head`, `vortech_eagle`, `vortech_fort`. So the "missing arms"
are most likely a **separate object with its own instance/skinning constants and its own VB/IB**, drawn by
a separate draw call. (Static inference: I don't know which model your screenshot shows.)

Both arm banks contain the same 4 VS/PS pairs (order differs):

| VS | PS | VS constant groups (float regs) | PS |
|---|---|---|---|
| `35DB03916F21B74F` | `A71CC74B4EE251E5` | Camera c0-8, Fog c43-47, Instance c48-51, Instance2 c52-54, Material c16-28, MiscGroup1 c72-73, **Skinning c92-247** | Material c4-18, MiscGroup c40-45, s5/s6/s7 (layer0..2) |
| `A9FDB0A3AC4591FE` | `B2998D20AB498293` | Camera c0-7, Instance c48-51, Instance2 c52-54, Material c16-28, **Skinning c92-247** | same as above |
| `0406577EF00099BD` | `7CA67FB21C799FE4` | Camera, Fog, Instance, Instance2, MiscGroup1, **Skinning c92-247** | Lights c32-36, Material c4-15, MiscGroup c40-56, s3 = sceneEnvmap cube |
| `5EFE4DB4D125C51C` | `2E461045C523CE73` | Camera c0-7, Instance, Instance2, MiscGroup1, **Skinning c92-247** | Lights c32-36, Material c4-15, MiscGroup c40-45, s3 cube |

- **All four are skinned through constants:** `g_SkinningVS.vs_skinMatrix` = **c92..c247 (156 regs = 52 bones x 4x3)**.
  **None of them declares a vertex sampler**, so the arms do **not** use vertex texture fetch.
  (`vs_vtf_kOffset_kHeight` / `vs_vtf_direction` are just float members of g_MaterialVS.) Rule VS slots 16..19 out for the arms.
- `vortech_body` uses the **same four pairs plus 14 others** (it adds F8F4E648..., 386847F3..., C9DB3F6A..., E634DE29...,
  6E25E88B..., 5D70ED05..., 07ADC2A5...). If the body renders correctly with 35DB/A9FD/0406/5EFE, the arms'
  shaders aren't the problem in themselves. The difference has to be per-draw data:
  c48-54 (instance world / worldView), c92-247 (bones), the stream/IB, or culling.
- Fog vs no-fog and with/without g_MaterialVS are separate pairs. A9FD/B299 and 5EFE/2E46 (the no-fog
  pairs) don't appear in any of the 31 session logs, while 35DB and 0406 do. That may only be your log cap.
  It's cheap to check whether those two were ever adopted, and whether the arms are the draws using them.
- Cheapest runtime check: log per draw VS hash in {35DB, A9FD, 0406, 5EFE} -> c48..c54, c92..c95, vertex count,
  the first stream base and the cull/stencil/blend key. Compare body draws (same hashes) with the arm draws.

## 2. TT Games opening logo
`commonobjects\cut_openinglogos_nxg.gsc` (+ `.360_shaders`, `.nxg_textures` 4.5 MB). Its bank has only 2 pairs:

| VS | PS | VS groups | PS groups |
|---|---|---|---|
| `91007ACB3E640E3D` | `EEE573BE160E037D` | Camera c0-8, Fog c43-47, Instance c48-51, Material c16-24, MiscGroup1 c72-73 | Material c4, MiscGroup c40-56, s5 layer0 |
| `C3958E2D1B795ED9` | `0C1840BF35E84F2F` | Camera c0-3, Instance c48-51, Material c16-24 | Material c4, MiscGroup c40-45, s5 layer0 |

These are static (unskinned), single-texture shaders with no vertex samplers. 91007ACB/EEE573BE appear in 31 logs.
**C3958E2D/0C1840BF don't appear in any log, but they are in the microcode index.** If the logo is the part that's
missing, check whether this pair is ever adopted. (Another identical container elsewhere would produce the same
hash, so a hash hit doesn't prove it's the logo.) The loading-screen logo is a separate asset: `commonobjects\cut_loadingscreen_logo_nxg.*`.

## 3. Draw entry points (statically complete for the D3D range)
- Game meshes go through the **central dispatcher `82B74FB8`**. It calls `82BCF958` -> **`82BCE1F8` (the only
  caller of DrawIndexedVertices 83FC6A58)** or `82BCFA70` (10 calls to DrawVertices 83FC6640, one per primitive type).
  82B74FB8 is called from 82B3BFB8, 82B40890 (x5), 82B411D8 (x2, skinned path), 82B75468, 82B93070, 82BC8F10,
  82BD6C68/6C80/6CE8/6D00/6D28, 82BDA388 (x4). Native hooks both Draw functions, so every mesh draw is covered.
- PM4 draw packets emitted *outside* those two (DRAW_INDX 0x22 / DRAW_INDX_2 0x36):
  `83FC4EF0`/`83FC5048` (via `83FC53D0`/`83FC54C8`) <- **`82BD2868`, the game's GPU memmove (Addendum 8)**, which also
  calls DrawVertices; `83FC1B28` <- `83FC2C80`/`83FC3438` <- `83FC4338` <- **Swap 83FB33E0** (present blit);
  `83FB7310` <- `83FCA0A0` <- **CreateDevice 83FAF9A0**; ResolveEx 83FBDF80; `83FC38D0` (only reached through a function
  pointer, not identified). **None of these is a character or logo mesh draw.**
- No DrawVerticesUP / DrawIndexedVerticesUP entry (any D3D function emitting draw packets) was found besides the ones listed.
  **No hardware instancing:** Xenos has none. Here "instance" means the constant groups g_InstanceVS c48-51 /
  g_Instance2VS c52-54, written per draw.
- I did not trace the stream setup (SetStreamSource/vertex declaration) for the arms specifically.
  Addendum 13's double-buffered VB/IB owners (82CBCBE8 / 82CBC9E0) are still the candidates for skinned/dynamic geometry. Unverified.

## 4. Stencil: every setter, decoded (RB_DEPTHCONTROL = device+10548, RB_STENCILREFMASK = +10496, _BF = +10492)
Shadow block: +10444 = reg 0x2100, so +10492 = **0x210C RB_STENCILREFMASK_BF** and +10496 = **0x210D RB_STENCILREFMASK**.
(This is consistent with +10460 = RB_COLOR_MASK 0x2104 and the blend-factor floats at +10464..10476 written by 83FB82F0.)
All values are inserted **raw** into Xenos fields: CompareFunction 0 NEVER..7 ALWAYS; StencilOp 0 KEEP, 1 ZERO, 2 REPLACE,
3 INCR_SAT, 4 DECR_SAT, 5 INVERT, 6 INCR_WRAP, 7 DECR_WRAP. Your op table in draw.cpp:378 matches.

| Setter (getter) | Field (LSB bits of RB_DEPTHCONTROL) | Note |
|---|---|---|
| **83FB8570** (83FB85A8) | bit0 STENCIL_ENABLE | requested value kept at +12312; the HW bit = value AND (depth surface +12832 != 0) |
| 83FB84C8 (83FB8500) | bit1 Z_ENABLE | same gating, request at +12308 |
| 83FB8508 (83FB8528) | bit2 Z_WRITE_ENABLE | |
| 83FB8538 (83FB8560) | bits4-6 ZFUNC | |
| **83FB85B0** (83FB85D8) | bit7 BACKFACE_ENABLE (two-sided) | not a ref/mask |
| 83FB85E8 (83FB8608) | bits8-10 STENCILFUNC | |
| 83FB8618 (83FB8640) | bits11-13 STENCILFAIL | |
| 83FB8688 (83FB86A8) | bits14-16 STENCILZPASS | |
| **83FB8650** (83FB8678) | bits17-19 STENCILZFAIL | not ref |
| 83FB86B8 (83FB86D8) | bits20-22 STENCILFUNC_BF | |
| 83FB86E8 (83FB8710) | bits23-25 STENCILFAIL_BF | |
| 83FB8758 (83FB8778) | bits26-28 STENCILZPASS_BF | |
| 83FB8720 (83FB8748) | bits29-31 STENCILZFAIL_BF | |
| 83FB8788 / 87A8 / 87C8 | byte +10499 REF, +10498 MASK (read), +10497 WRITEMASK | dirty[16] bit 28 |
| 83FB87E8 / 8808 / 8828 | same for _BF (+10495 / +10494 / +10493) | dirty[16] bit 29 |
| 83FB9560 / 9590 / 95C0 / 95F0 | +10560 bits 3 / 2 / 5 and byte +10562 | Hi-Z / Hi-Stencil control (performance, not a correctness state; static guess) |
| 83FAF5D8 | emits event 0x46 + reg 0x5C8 = 0x20000 | HiZ/HiStencil flush |

SetDepthStencilSurface (**83FBAE38**) re-derives Z_ENABLE/STENCIL_ENABLE from +12308/+12312 whenever the DS changes.
So with no DS bound, both are forced off in the shadow, and they come back on rebind. Native should do the same
(or just read the HW bits from +10548, as it does now).

**Ref/mask facts:** only 83FB8788 (front REF) has callers, and every call passes **0**. **No code writes the read mask,
the write mask, or any _BF byte** (whole-word writes to +10492/+10496 in the D3D range: none). Those values come only from
device init / the 83FC9450 state replay, which native skips. **If native's shadow holds 0 there, write mask 0 = stencil never
written, and read mask 0 = NOTEQUAL-0 always fails / EQUAL-0 always passes.** Seed both bytes with 0xFF at CreateDevice
(D3D default). I did not read the actual default out of 83FC9450, so treat 0xFF as the D3D default, not a TU23 fact.

### The game's stencil modes: `82BCEAC8(state, mode 0..11)`
State object pointer at global 0x849677B8. Only 4 functions call the leaf setters at all: 82BCEAC8 (almost everything),
82B91C88, 82B69FB0, 82B685A8.

| mode | 2-sided | func F / BF | ops | HiZ |
|---|---|---|---|---|
| 0 | - | stencil OFF | - | off |
| 1 | no | **NOTEQUAL** ref 0 | keep all | 9560=1, flush |
| 2 | no | **EQUAL** ref 0 | keep all | 0 |
| 3, 9, 11 | yes | ALWAYS (9: NOTEQUAL) | 3/9: zpass INCR_WRAP both faces; 11: zfail INCR_WRAP both | 0 |
| 4, 10 | yes | ALWAYS (10: NOTEQUAL) | zfail DECR_WRAP front / INCR_WRAP back (z-fail volume, "Carmack's reverse") | 0 |
| 5 | yes | ALWAYS | zpass INCR_WRAP both | 0 |
| 6 | no | ALWAYS ref 0 | zpass ZERO (clears the stencil where drawn) | flush |
| 7 | yes | ALWAYS | zpass ZERO both | 0 |
| 8 | yes | ALWAYS | zfail INCR_WRAP front / DECR_SAT back | 0 |

Callers with constant modes: 82B97B68 / 82B8D648 / 82B8E508 (**4, then 1, then 0** = mark a volume with z-fail, then draw
where != 0), 82B94BC0 (8, 7, 1, 0), 82BCC270 (7, 3), 82B91C88 (5, 1), 82B95278 / 82BA75B0 (1, 0), 82B726C8 / 82BAF2B8 (6, 0).
**Data-driven:** `82BC3D00` takes the mode from the **material: (mat[+948] >> 11) & 7** (modes 1/2 kept; otherwise 3 or 7
from mat[+952] bit 12), and `82B3EC68` takes it from a byte at obj+116 minus 1, with a push/pop sentinel of 12.
**I couldn't link any of these callers to the portal statically.** The 4->1 and 5->1 sequences are the "mask then fill
inside" pattern a portal or volume effect would use, but that's an inference.
Runtime check: log the 82BCEAC8 mode per draw (or the +10548 / +10496 values your stencil candidate log already prints)
across a portal frame. Note that in draw.cpp native stencil is opt-in (`LEGO_NATIVE_STENCIL`). Without it, mode-1/2
consumers draw everywhere or nowhere, depending on the mode.

# Addendum 17 (2026-10-01): shader float constants — any path that bypasses device+0x780 / +0x1780?

**STATIC, TU23 only.** Short answer: **no game path writes VS/PS float constants straight into the command stream.**
One D3D path loads constants without going through the shadow: shader-embedded literals. XenosRecomp already bakes those.

1. **SetVertexShaderConstantF = `83FB6480`**: copies to device + (120+start)*16 = **+0x780 + 16*start**, then
   `dirty[+0] |= mask`. **SetPixelShaderConstantF = `83FB6558`**: **+0x1780** (376*16), `dirty[+8] |= mask`.
   Their only callers: **`82B7AB20`** (game constant upload, 85 call sites, args: stage r5 0=VS, start r7, count r8;
   it skips the call when the source pointer is unchanged and the param's dirty byte is 0) and **`82B7ABF0`** (the
   per-draw reflection loop over the shader's used groups, called from the draw dispatcher 82B74FB8), plus D3D-internal
   `83FD3A70/3AC0/3B50` (VS) and `83FD3D90/3E20/3EA8` (PS). Every material, skinning (c92-247), instance and camera
   group of the arms and the logo goes through these, so it lands in the shadow.
2. **Shadow -> GPU** happens inside the draw flush: DrawVertices / DrawIndexedVertices / 83FC6180 / 83FBDD20 call
   **`83FD1320`** with (dirty mask, reg base **0x4000** VS / **0x4400** PS, src +0x780 / +0x1780). It walks the 64-bit
   mask (1 bit = 4 float4) and emits **type-0 register writes**. That's consistent with the shadow.
3. **LOAD_ALU_CONSTANT (0x2F): exactly one emitter, `83FD1EC8`**, called 3x from the draw-state flush **`83FD20B8`**:
   from the VS object (device+13068: table at +40, base +24) and the PS object (device+13072: table at +872, base +32).
   Each entry is {u16 start, u16 count, u32 offset}, and the source is **the shader's own physical memory** (the
   definition table literals), not the shadow. So this does bypass device+0x780/+0x1780. But XenosRecomp materializes
   definition-table constants as locals (shader_recompiler.cpp:1609-1650), so native needs nothing extra **provided the
   AOT build saw the same container**. This applies to every shader, not specifically to the arms, the logo or portal particles.
4. **No SET_CONSTANT (0x2D) emitter** anywhere in 83FA0000-83FF0000 (I searched every `lis 0xC00x / ori` PM4 header).
   The 0x55 headers there (83FB7AB8, 83FBDF80, 83FCBF00, 83FD0BF8, 83FD20B8) write **registers** (0x2080, 0x2180,
   0x2001, 0x2208...), not ALU constants. **No GpuBeginShaderConstantF4 / GpuOwn-style function exists**: no D3D function
   other than 83FD1320 builds 0x4000/0x4400 headers. INDIRECT_BUFFER (0x3F) only appears in ring/segment management
   (83FBFCA0, 83FC72F8), reached through function pointers. I found no game-built command buffers, but this isn't proven.
5. Portal particles: I couldn't identify their shaders statically. Nothing found suggests a special constant path for them.
   They would use 82B7AB20/82B7ABF0 like everything else.

Caveat: native copies the whole 0x780/0x1780 banks every draw (draw.cpp:209), so even direct game stores into the shadow
without dirty bits would be seen. The bypass risk is limited to (3).

# Addendum 18 (2026-10-01): answers to the Oct 1 follow-up + VTE/clip finding

> **From the user: do NOT launch the game again until everything below (and the earlier open items) is
> implemented and passes offline tests.** Make one complete fix batch first. Then do a single test run, and
> only when the user asks for it.

All of this is STATIC (generated TU23 sources, GAME.DAT entries, the translated HLSL in `out/native-gpu/hlsl-depth`).

## 0. Corrections accepted
- Alpha ref default = 1.0f (descriptor 59 in table 0x847F9B18). That's your reading of the real table. It replaces my
  "D3D default 0.0" in Addendum 15.
- Stencil masks were already seeded from the TU23 descriptors (00FFFF00), so drop my Addendum 16 0xFF advice.

## 1. Do the traced 0406/35DB ranges belong to the arms? No, at least not to the arm files
GAME.DAT decompressed sizes: `vortech_armleft_nxg.ghg` = **59,200 bytes**, `vortech_armright_nxg.ghg` = **61,522 bytes**,
`vortech_body_nxg.ghg` = 527,760 bytes. Your samples index up to start 43644 + 1440 = **45,084 indices** (90 KB as u16)
in one IB, with several VBs (bases 0/610/1220/1243/1535). **That IB can't come from either arm file.** Together the two are
smaller than 90 KB of indices alone. The ranges fit the body (or another large model).
(Assumption: the IB is the model's own buffer from its GHG, not a shared pool. Your "VB length 36864" points to per-model buffers.)
**So in mesh1-3 no traced draw is provably an arm draw. The arm draws may not reach Draw at all**, which would point
to game-side visibility/animation/LOD, not the renderer. Combine that with the user's note that the torso and legs are
also damaged, and with rebind1-vortech.jpg, where almost the whole big Vortech is absent: the problem covers more than the arms.
I didn't parse GHG submesh tables (no parser for that format here). A cheaper identity test that needs no game run:
compare your traced first-index bytes and vertex bytes against the raw bytes inside `vortech_body_nxg.ghg`
(`modcli extract GAME.DAT "chars\misc\vortech_body\vortech_body_nxg.ghg" out.ghg`, then search for the 24/36-byte vertex
records you logged). A byte match identifies the asset with certainty.

## 2. Opening logo: which quad is TT
`cut_openinglogos_nxg.gsc` names 4 objects **WBLOGO, LEGOLOGO, PRESENTS, TTGAMESPRODUCTION** and 5 materials
`_TTShaderMaterial1..5`. `cut_openinglogos_nxg.nxg_textures` holds 5 textures, in this order:
`wblogo.nut, legologo.nut, presents.nut, ttgamesproduction.nut, ttlogo.nut` (plus 3 engine dummy lightmaps).
Your 5 quads (index starts 0/6/12/18/24) match 5 textures. **Most likely order: 0 = WB, 6 = LEGO, 12 = PRESENTS,
18 = TT GAMES PRODUCTION text, 24 = TT logo.** That's an inference from the string order, not from a parsed material table.
Confirm it by logging the texture fetch-constant base for each quad and matching it against the 5 DDS headers in the nxg_textures file.
The starts 18/24 that fade to c73.w = 0.50 / 0.99 are therefore probably the TT text and TT logo, fading in as expected.
If they reach ~1.0 and still don't show, look at the texture/format of those two entries (the first three DDS entries
are flagged `OPAQ`, the later ones aren't, so expect alpha-carrying formats there) and at blend. Opacity is not the issue.
C3958E2D/0C1840BF is the **no-fog variant** of the same material set (no g_FogVS, Camera c0-3 only). The game picks the
variant from the scene fog state. Only 9100/EEE5 being observed means fog-on is selected here, so C395/0C18 being absent is expected.

## 3. Shader audit of VS 0406577EF00099BD (HLSL `hlsl-depth/smaug_firework_nxg.6.hlsl`, same hash)
```
r8      = POSITION.xyz, w=1
r9      = NORMAL (decoded) * c255.x + c255.w        // c255 = (2.00787, 0, 0.001, -1): ubyte -> [-1,1]
r4      = BLENDINDICES * c254.x                     // c254.x = 3.0 (definition-table literal) -> index*3
for each of 4 influences: a0 = round(r4.i); rows c[92+a0], c[93+a0], c[94+a0] (4x3)
pos     = sum(weights * bone rows)                  // weights: all 4 (x,y explicit, z = 1-x-y style via c254.y = 1)
world   = c48..c51 (g_InstanceVS), normal via c52..c54 (g_Instance2VS)
clip    = c0..c3 (g_CameraVS viewProj)
```
- The math is as expected: the literals c254/c255 come from the definition table and are baked in, the bone stride is 3,
  and the relative load is c92+a0. So **if c92+ (bones), c48-51 and c0-3 are correct, this shader places the vertex
  correctly.** It does not read normal.w or material opacity for position. Alpha only goes through COLOR0, as you found for the logo.
- Check in the trace: **BLENDINDICES must reach the shader as integers 0..255** (`uint4` input). With UNORM the index
  becomes 0..1, so every vertex uses bone 0. That collapses the mesh to one bone but doesn't make it disappear.
  **The weights must be UNORM.** Your host bytes (00000100 indices / 000001FE weights) pair up correctly only if both use the same WZYX swap. They do.
- **The thing to compare at runtime is not the shader:** c48..c51 of the Vortech draws. A zero/garbage instance matrix or
  bone palette makes the whole model disappear or collapse. That matches "torso, legs, arms all missing".

## 4. Clip / viewport-transform state: a real gap in native
| Writer | What it writes |
|---|---|
| **83FB8FD0** `(dev, enable)` | **PA_CL_VTE_CNTL (+10572) = enable ? 0x43F : 0x400**, and **PA_CL_CLIP_CNTL (+10564) bit16 CLIP_DISABLE = !enable**; dirty \|= 0x20, 0x80 |
| 83FB8848 | PA_CL_CLIP_CNTL bits0-5 = user clip plane enables (D3DRS_CLIPPLANEENABLE); also +10420 = 0x1000 if any; no direct callers |
| 83FCA320 (device init) | PA_CL_CLIP_CNTL \|= 0x80000 (bit19 DX_CLIP_SPACE_DEF: z in [0,w]) |

Callers of 83FB8FD0(0): **83EB6D68** (the InsertCallback/virtual23 helper) and 83FB2898 (Swap's present blit). In 0x400
mode the viewport scale/offset is OFF: the VS outputs **window-space** coordinates, and clipping is disabled. Native has no
handling of +10572/+10564 at all (grep finds nothing in src/gpu_native). **Any 83EB6D68 draw then goes through the viewport
transform a second time and lands off-screen or tiny.** Fix: when VTE == 0x400, set the host viewport to identity for
window coordinates (or convert in the VS epilogue, as xenia does), and turn depth clip off when bit16 is set. Bit19 means
D3D-style z, which is what D3D12 already does. **No evidence ties the world meshes to this.** They never call 83FB8FD0, so leave
their VTE alone.

## 5. 83EB6D68 / 83EB4480 virtual23 (open question 3)
What 83EB6D68 does, statically: SetTexture slots **0, 1, 2** (83FB58A8 x3, i.e. three planes), sampler states for those 3 stages
(83FB9640 / 83FB97E8), VS/PS (83FB6A30 / 83FB6828), Z off (83FB84C8(0)), cull off (83FB7DA0(0)), **VTE off
(83FB8FD0(0))**, 83FBA160(0), then **DrawVertices(prim 4 = TRIANGLELIST, 6 verts)**, then InsertCallback 83FC0F38 ->
83EB4480 -> `this->vtable[23]`. It is only reached through a function pointer, so there's no static caller.
Three planes + screen space + a callback after the draw is the shape of a **video (YUV) frame presenter**: the callback releases
the frame's surfaces back to the decoder. The executable contains `.bik` / `.wmv` / `bink` strings. **No .bik/.wmv/.xmv exists
in GAME*.DAT or on the disc image**, so I don't expect this path to be active in the intro. The TT logo is the GSC scene
from section 2, not a video. If it does run, section 4 is why it would be invisible. I haven't resolved the vtable[23] target (it needs
the object's vtable from a runtime pointer), so lifetime and safety on the guest hook thread are still open.

## 6. Dynamic edits to shader fetch/definition tables
Not re-traced. From Addendum 17: the only consumer is 83FD20B8 -> 83FD1EC8, which reads the VS/PS object's tables at
+40/+872 each draw. I didn't search for writers to those offsets after creation. Next static step if needed: grep for stores at
+40..+60 / +872..+904 on objects stored to device+13068/+13072.

# Addendum 19 (2026-10-01): the big Vortech is a KRAWLIE swarm; corrections; TT depth

STATIC only. **Still: do not launch the game until the fix batch is complete; one run only when the user asks.**
Corrections accepted: my "arm files together < 90 KB" in Addendum 18 was wrong arithmetic (120,722 B). Your byte
matching supersedes it. Thanks for the exact offsets: all 8 skinned samples are vortech_body.

## 1. Why the arm meshes never reach Draw: they are emitters, not drawn meshes (data proof, code partly traced)
`cut\story\0001_prologue\0001_prologue.txt` (GAME1.DAT), cutscene **0001_PrologueC**:
```
// KRAWLIES!!!
draw_krawlies
addon WWCCutsceneKnight (Config='WWCCutsceneKnight', CutsceneChar='VORTECH_BODY',     NumberOfKrawlies=12000)
addon WWCCutsceneKnight (Config='WWCCutsceneKnight', CutsceneChar='VORTECH_ARMRIGHT', NumberOfKrawlies=1200)
addon WWCCutsceneKnight (Config='WWCCutsceneKnight', CutsceneChar='VORTECH_ARMLEFT',  NumberOfKrawlies=1200)
```
So the giant Vortech in this cutscene is **14,400 "krawlie" bricks** placed on the animated body/arm characters.
The character GHGs (body, armleft, armright) are the **skeleton/surface the bricks follow**. The visible arms are
**bricks**, not the arm mesh. **An arm mesh that never reaches Draw is therefore consistent with normal behaviour,
not a CPU gate you need to find.** The renderer-side question becomes: where do the krawlie bricks go?
- Brick asset: `commonobjects\krawlie_bricks_vortechblue_nxg.gsc` + `.360_shaders`. Its pairs:
  VS `5FE844BD78B0368C`/PS `DAB518E550952312`, VS `C0C579A9C7D3ACD9`/PS `BC67385D86BABED8`,
  VS `FFC80E0F8B28B228`/PS `1998CB74A7EDAFE1` and PS `336657153E47E0C3`, VS `362C416499E90A3B`/PS `0705E91ECC4BA3BB`.
  **None of these VS have skinning or vertex samplers.** Their groups are only Camera, Instance c48-51, Instance2 c52-54,
  Material c16-24, Lights c36-42, Fog, MiscGroup1 c72-73. So each brick is either one draw with its own c48-54, or
  (more plausible for 14,400) **a CPU-built batch in a dynamic VB with world-space vertices** and identity-like c48-51.
  Not proven which. (These hashes also occur in other banks, so log hits don't identify the swarm.)
- Code anchors (string xrefs, lis/addi pairs): `"WWCCutsceneKnight"` 0x820D6850 is referenced by **82AF3448**
  (a 1,352-instruction update function, probably the addon's per-frame update) and 84080A50. `"NumberOfKrawlies"`
  0x820453F0 is referenced by **82722AA8, 82743D88, 83C0D9F0**. `"KrawlieRenderCharInstAddOn"` 0x821F3E5C and
  `"draw_krawlies"` 0x823543C8 have no direct lis/addi xref (probably table-driven). I did not finish tracing from 82AF3448 to
  the draw. Next static step: follow 82AF3448's callees for the dynamic-VB lock (Addendum 12/13 owners
  82CBCBE8 / 82CBC9E0 / lock 82BD6088) or for 82B74FB8.
- **Prime suspect class for native:** the dynamic, double-buffered VB path. The CPU rewrites the brick vertices every frame
  and the buffer flips (Addendum 13). A stale, never-re-uploaded or wrong-parity copy gives you "missing or frozen bricks" while
  static meshes look fine. That's the same class as Addendum 11's VTF re-upload rule, but for vertex buffers.
  Check offline in your existing traces: do draws with VS in {5FE8, C0C5, FFC8, 362C} exist in the PrologueC window of mesh1-3,
  and is their VB a dynamic/double-buffered owner?
- Other krawlie uses exist (`chars\cutscene_krawlies\cut_crowd\*.krw`, `chars\krawlies\...\*.krw`: baked krawlie animations),
  so the same fix would also cover crowds and swarms elsewhere.

## 2. Body GHG bone/submesh tables
Not parsed. No GHG parser exists here, and the format isn't documented in our research folders. With section 1 the
priority drops: the body/arm meshes are the krawlie surfaces. Whether the body mesh itself is also meant to be visible
(your 8 submitted body draws) or only as a hidden emitter (with the bricks the real visual) is decided by the addon,
which I haven't traced. Torso/legs "missing" fits "bricks missing" as well.

## 3. TT quads 18/24 vs the galaxy under ZFUNC GREATEREQUAL
I can't derive per-quad depth/UV statically without parsing the GSC instance table. One concrete thing to check offline:
**ZFUNC GEQUAL means the game expects a depth buffer cleared to 0 (or reversed-Z).** Check that native's Clear
(83FBC9D8) honours the Z value and flags the game passes, and that a depth clear isn't skipped or replaced by 1.0
before the logo pass. If the clear is 1.0, every GEQUAL test fails except at exactly 1.0, and a quad "behind the galaxy"
is the visible symptom. The fact that your logo samples submit with alpha test off fits this. Opacity reaches 0.99, so it's not the cause.

## 4. Stage ownership: corrected
Confirmed from code: SetVertexShader 83FB6A30 stores to **device+13072**, SetPixelShader 83FB6828 to **device+13068**.
In 83FD20B8, r30 = +13072 = **VS object, literal table at +872** (with +896 / +904 sub-offsets), and r29 = +13068 = **PS
object, literal table at +40**, base at +24. Addendum 17 had VS/PS swapped. The mechanism is unchanged. When you trace
post-creation writes, look at **VS+872.. and PS+40..**.
