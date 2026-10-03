# Вопросы после возвращения пользователя

Пользователь вернулся 30 сентября. Ссылка и приоритетный вопрос переданы ему
через форму для пересылки агенту. Прямого сообщения другому агенту не было;
ответ получен в tiling-answer-20260930.md, Addendum 14. Ниже сохранены
исходные вопросы; продолжение для следующего разбора — в конце файла.

1. TU23 BlockUntilNotBusy83FBF900 сейчас в native вызывает полный drain всех
   host frame fences. Подготовлен opt-in LEGO_NATIVE_ASYNC_CPU_RESOURCES:
   известные native VB/IB используют неизменяемые host UPLOAD snapshots;
   известные AOT shaders читаются из встроенного архива; borrowed CPU textures
   (!surface, !resolved_on_host) копируются из guest в retained UPLOAD storage,
   затем в host texture строго в порядке одной graphics queue. Для них GPU
   никогда не читает саму guest allocation. Неизвестные ресурсы, surfaces,
   GPU-resolved textures, BlockOnFence и InsertCallback сохраняют полный wait.
   Проверь вызывающих83FBF900: нет ли у этих CPU ресурсов дополнительного
   CPU-worker readiness контракта, который был случайно обеспечен GPU drain?
   Нужны адреса вызывающих и смысл guest fence полей, особенно82BD6088,
   82CB9120/82CB9070 phase0. Первый runtime probe почти не пропускал wait:
   в конце 48 drains/кадр и 29.89мс, skips=0; стабильные30 не достигнуты.

2. После исправления borrowed GetResourceType пользователь выдержал5минут
   на Press START без прежней порчи (Addendum13 цепочка подтверждена логом),
   но в катсцене у Vortech по-прежнему исчезают руки. Банк118, все VS slots16/17
   формат38 1024x128 теперь full-hash при каждом draw, buffer contents также
   full-hash, metadata заголовков обновляется. Какая ветвь анимации/декларация
   отвечает за руки, и может ли часть геометрии использовать другие skinning
   textures/streams? Нужен список писателей/commit boundaries для сопоставления
   с runtime capture, не новая гипотеза 'все textures stale'.

3. В главном меню отсутствует содержимое портала и похожие эффекты; при Enter
   цвета кратко бледно-коричневые, затем нормализуются. Тонемап3A47E5DDE66B42C6
   после резкой HDR/half картинки сильно блюрит final. Нужны конкретные
   shader/pass/constant inputs для portal и blur/DoF, и роль PS c12/slot3;
   какие ожидаемые значения/LOD/alpha controls сравнить с эталонной Xenia?
   Существующий isolated explicit-LOD752 candidate не установлен, runtime ABI624.

4. Приоритет FPS: свежая разбивка fps-sync-reasons1 показывает в тяжёлом
   30-секундном участке 41.89 drains/кадр, 28.87мс суммарно. Из них
   InsertCallback83FC0F38: 39.28/кадр и 28.74мс; остальные причины вместе
   около0.13мс. Native выполняет callback синхронно после полного drain.
   В generated callers82BCE2C8/82BCE304 передают callback82BCDE68, а
   82BCE4F8/82BCE534 —82BCDF30, flags=1. Это статически найденные кандидаты,
   принадлежность всех runtime waits именно им ещё не подтверждена.
   Проверь обязанности этих callbacks и83FBD2D8/83EB711C: записывают ли они
   только bookkeeping/refcounts/releases CPU-snapshot ресурсов или читают
   GPU-produced данные? Какие guest wait/owner зависимости требуют точного
   завершения? Нужен контракт безопасного отложенного исполнения по host
   fence либо раннего CPU-only исполнения для конкретных адресов, включая
   последствия для pool relocation/анимации. Просто убирать все callbacks
   или объявлять их completed без доказательства не планируем.

Контекст: tools/gpu-aot/tiling-answer-20260930.md Addenda11–13;
session-20260930/placement-type1-game.log, placement-type1-title-start.png,
placement-type1-title-3min.png и placement-type1-title-5min.png
(последний уже показывает главное меню после пользовательского Enter).
FPS доказательства: session-20260930/fps-sync-reasons1-game.log,
fps-sync-reasons1-frames.csv(.summary.json), fps-sync-reasons1-cutscene.png.

## Следующие уточнения после Addendum 14

Подготовлены во время отсутствия пользователя. Пункты1–2 переданы ему через
форму для пересылки после возврата с обратной связью о новой сборке; ответ
получен в Addendum15. Пункт3 ещё не передан. Пользователь подтвердил более
стабильную катсцену/меню, но руки и vortex всё ещё отсутствуют.

**Коррекция после ответа:** +10568 — cull/fill, НЕ alpha func/ref.
Правильные alpha setters:83FB82C0 (+10556 low3),83FB7E00 (+10556 bit3),
83FB92B8 (raw FLOAT +10620). Пункт1 ниже оставлен как история отправленного
вопроса, не как достоверная карта alpha. Tonemap смешивает sharp slot0 с
downsampled slot4; slot3 — добавочный glow. См. Addendum15.

1. Alpha test: compiler уже генерирует `clip(oC0.w-g_AlphaThreshold)` перед
   EDRAM scale, spec bit1; host оставляет threshold=0 и никогда не включает
   этот bit (только normal bit0). Поэтому ветвь существует, но не применяется.
   В TU23 нашёл setter83FB7E00: enable в device+10556 bit3;
   setter83FB7DA0/getter83FB7DC0: function в device+10568 bits0..2;
   setter83FB7DD0/getter83FB7DF0: reference в +10568 bits3..10 (8 бит).
   Подтверди связь с D3DRS_ALPHATESTENABLE/ALPHAFUNC/ALPHAREF, перевод compare
   (сырое значение D3DCMP или Xenos enum), нормализацию reference и порядок
   относительно shader output/EDRAM exp. Почему ref упакован в +10568, а не
   отдельный float RB_ALPHA_REF? Нужен путь, который превращает этот shadow
   в настоящие hardware registers. Unleashed использует value/256 и только
   >= clip, но переносить его приближение без проверки TU23 не хочу.
   Нужно также выяснить defaults этих трёх состояний: native CreateDevice
   инициализирует sampler defaults, но не вызывает render-state init.
   Это найденный пробел, не доказательство причины невидимого портала/HUD.

2. Для следующего разбора blur: baseline уже пишет `pixel shader filter
   matches 1 AOT containers` (fps-sync-reasons1-game.log 19:33:27.041).
   Значит, одно отключение CFEAC7ADB912F8A9 уже применяется, но размытие
   осталось. Текущий runtime ABI624, isolated explicit-LOD candidate752
   не установлен. Нужен разбор tonemap3A47E5DDE66B42C6: как именно c12 и
   slot3 влияют на выбор/смешивание sharp и half/blur; какие значения в
   микрокоде дают чистый sharp output? Не просим объявлять DoF причиной
   пропавших рук. При необходимости предоставим disassembly контейнера.

3. Callback83EB4480 invokes vtable+92. Новая очередь сохраняет completion
   по fence всего содержащего submission и FIFO, без drain в InsertCallback.
   Уточни обязанности конкретных virtual23 в helper83EB6D68: только release
   CPU snapshot/renderer bookkeeping или game ждёт side effects до следующего
   draw/кадра? Нужны конкретные vtable/method addresses и lifetime объекта,
   чтобы проверить допустимость исполнения на guest hook thread после
   containing submission (вместо точного GPU marker).

## Обновление после офлайн-проверки alpha/defaults (ещё не отправлено)

Для следующего ответа агенту: исправление к Addendum15 — таблица TU23
0x847F9B18, descriptor59 содержит getter83FB92E0/setter83FB92B8/default3F800000.
Это float1.0, не0.0. Native теперь применяет15 проверенных leaf setters для
alpha/blend/масок цвета; результат проверен на оригинальных generated functions.
Все8 alpha compares реализованы до EDRAM scale; depth-only PS исключены.
9290 основных и118 runtime контейнеров пересобраны, ABI624 сохранён.

Гипотеза embedded fetch patches для рук не подтвердилась на captured corpus:
у всех118 контейнеров нет copy/masked records. Отдельно не проверено, меняет ли
игра эту таблицу позднее в уже созданном shader object. Native SetShader пока
эти таблицы не применяет. Нужен конкретный писатель, если такие динамические
изменения существуют, а не рекомендация снова расширить банк.

Остаётся ранее подготовленный вопрос3 о UI/video callback83EB4480 и
virtual23; добавочный контекст — TT Games logo отсутствует перед катсценой.
Нужно отделить путь видео/загрузки от geometry draws и назвать конкретные
методы/ресурсы. QuickStartup сокращает legal/loading, но TT-logo причинно
не подтверждён. Пока игру по просьбе пользователя не запускаем.

## Oct1 follow-up after Addenda16/17 — runtime evidence

User authorized today's tests, monitor3 only. All probes now closed. We read
both addenda and applied the DS rebind enable-bit tail missing from our native
SetDepthStencilSurface hook. 300 cases match the original TU23 PPC operations
(including requested bits surviving NULL then non-NULL binds). Stencil defaults
were already seeded using actual TU23 descriptor values, masks00FFFF00; this
was not a new fix. Candidate remains opt-in. No visual repair claimed.

Bounded target-mesh tracing now records before native rejection and after draw
submission, raw controls, c48..54/c73/c92..95/c247, PS c4/c45, stream metadata,
first actual index+base and converted host attribute bytes. No render overrides.
Artifacts: rexlego/out/native-gpu/session-20261001/*-summary.json; complete logs
in session-20260930/oct01-mesh1/2/3-game.log and oct01-rebind1-game.log.

mesh1: 43 distinct samples, ALL submitted. 0406577EF00099BD/7CA67FB21C799FE4
and35DB03916F21B74F/A71CC74B4EE251E5 each12 samples. Other no-fog pairs and
C3958E2D1B795ED9/0C1840BF35E84F2F never observed (trace is before rejection,
not the old capped stage log). Hash matches still do not identify an asset.

mesh2 sample9 (0406/7CA6): start42204,count1440,base1243, first_vertex1243,
VBstride24,length36864; POSITION half4, NORMAL UBYTE4N WZYX at8, COLOR at12,
BLENDINDICES UBYTE4 WZYX at16 host00000000, BLENDWEIGHT normalized WZYX at20
host000000FF (first weight1). Other samples use indices00000100 and weights
000001FE (indices0/1 and weights254/255,1/255). Bone c92..95 populated, c73.w1.
35DB sample13: start39402,count1350,base1220,first_vertex1220,VBstride36,
length69632; indices at28 host00000100, weights at32 host000003FC. Other ranges
start33954/base0,count2724;36678/base610,count2724;43644/base1535,count1440.
No packed-R11-normal flag in these samples. Native culling is NONE; raw
PA_SU_SC_MODE_CNTL=5 or6. Don't assume these samples are arms rather than body.

Logo pair9100/EEE5 uses five quads in the same VB/IB: index starts0,6,12,18,24,
bases0,4,8,12,16. POSITION float3, NORMAL UBYTE4N WZYX hostFF000000 -> w1,
UV half2. c73.w starts0 then fades up: rebind1 captures0.50081104 and0.99324435
for starts18/24. Alpha control00000007 (ALWAYS, enableOFF), ref1; PS c4 all1,
PS c45=(2,4,0.25,0). Initial zero opacity was normal fade behavior, NOT proof
of a bug. Shader multiplies normal.w*c73.w into COLOR0.x then PS output alpha.

Useful remaining static questions (no request to run game):
1. Can the exact VB/IB mesh ranges/counts above be matched to armleft/armright,
   body or a different character using extracted GHG? Need material/submesh
   identity and declared bone indices/count, not shader-hash identity alone.
2. Which of the five opening-logo quads is TT? Is C395/0C18 actually selected
   in this intro under fog=true, or merely an unused alternative in the bank?
   Which GSC object/material controls TT's draw, and which texture entry is it?
3. For the actual affected 0406/35DB draws, audit expected shader alpha and
   semantics against microcode (normal.w, material opacity, skin index*3,
   relative c92 load); don't infer shader correctness only because some body
   portions render. User clarified torso and legs are also missing/damaged.
4. Clip/depth state: does this title change PA_CL_CLIP_CNTL / PA_CL_VTE_CNTL
   for these meshes/effects? Our host always enables depth clip and uses cached
   viewport MinZ/MaxZ. Exact setters/fields only if relevant evidence exists.

## Ответ получен: Addendum 18 (tiling-answer-20260930.md)

**Указание пользователя: игру НЕ запускать, пока не исправлено всё из Addenda 16–18 и оставшихся пунктов и не пройдены офлайн-тесты. Потом один тестовый запуск — только по просьбе пользователя.**

## Oct1 offline follow-up after Addendum18

No new game launches. Exact uploaded records were inverted through both native
conversions (per-field WZYX, then DWORD endian swap) and searched in extracted
GHG files. ALL eight skinned samples in mesh2 belong uniquely to vortech_body:
0406 IDs9/10/11/12 -> file offsets398811/368979/392595/380787 (24 bytes);
35DB IDs13/14/15/16 ->450013/461353/428053/406093 (36 bytes).
No full-record matches in either arm file. Evidence:
rexlego/out/native-gpu/session-20261001/mesh2-asset-matches.json.
This proves stored vertex identity, not full geometry or scene-instance validity.
The two arm files total120722 bytes; the 'together smaller than90KB' sentence
in Addendum18 is arithmetically incorrect. Byte matching supersedes size inference.

Instance c48..51 in these body samples is finite/nonzero and shared across
the eight draws; c92..95 varies per submesh and is populated. c247=0 alone
does not prove invalid skinning. Full palette and camera were not captured.

Opening textures: the first THREE DDS entries are dummy DXT1/OPAQ lightmaps.
ALL FIVE real logo textures are DXT5, not just the last two. Real entries:
0xD71 1024x1024,0x100DF1 2048x1024,0x300E71 1024x256,
0x340EF1 1024x128,0x360F71 1024x1024. Raw DDS BC3 alpha decoder finds
37583 fully opaque +5432 intermediate alpha pixels in TT text and49965 fully
opaque +229595 intermediate in TT logo: neither source image is empty.
rebind1 quad18 textureAAD29740 is1024x128 and quad24 AB71BF40 is1024x1024;
both adopted as Xenosformat20/BC3,tiled1,swizzle688. Dimensions support object
order but are not an exact backing-address/payload identity proof.

The source shader banks of body and both arms do NOT contain the skipped
DoF pixel hashCFEAC7ADB912F8A9. No evidence to revert the confirmed blur fix.

Please continue STATIC analysis, without launching:
1. Trace scene object creation/visibility for vortech_arms (and body submeshes)
   into82B74FB8. Which CPU prerequisites can prevent arm Draw completely?
   Are they gated by occlusion-query results, shader bind return codes, or an
   animation/LOD condition? Need concrete callers/fields, not a guessed toggle.
2. Parse the body GHG bone/submesh tables if feasible. Which material/bone group
   occupies the eight offsets above; which remaining groups form torso/legs?
3. For TT quads18/24, identify expected depth/instance/camera and UV c23/24.
   Could scene material state place these behind the animated galaxy under
   the observed ZfuncGREATEREQUAL? Native alpha test is OFF on these samples.
4. Correct stage note: actual TU23SetVertexShader83FB6A30 usesdevice+13072;
   SetPixelShader83FB6828 uses+13068. Confirm table ownership on these objects
   before tracing post-creation writes (+40/+872 may refer to other structs).

## Ответ получен: Addendum 19

Главное: гигантский Vortech в 0001_PrologueC — рой krawlies (12000+1200+1200 кирпичей) на скелетах body/arms; меши рук — эмиттеры, их отсутствие в Draw ожидаемо. **Игру по-прежнему НЕ запускать до полного набора исправлений.**


Oct1 offline implementation update: verified TU23 viewport states43F/400 now
have an isolated opt-in candidate (LEGO_NATIVE_VIEWPORT). Shared ABI624 unchanged:
592/596 NDC pixel scale,604 mode,600 alpha compare,608 color output scale.
400 uses full-RT host viewport0..1 and VS conversion; CLIP_DISABLEbit16 controls
host depth clipping and separate PSO identity. Opt-in init seedsVTE43F/CLIPbit19.
No full arbitrary PA_CL_VTE_CNTL implementation claimed. This is not identified
as a cause of missing Vortech/world meshes. Matching9290+118banks installed,
18444+192specializations prelinked, hostlinked; alloffline tests pass, no game
launched. New diagnostics include fullcamera/UV/bonepalette and boundedBC3logo
upload capture; still OFF bydefault. Main visual/FPS issues remain open.

## Oct1 GPU correction after reading Addendum19 (no repeated questions)

The earlier GPU question about TT ZFUNC GREATEREQUAL was incorrect. Actual
oct01-rebind1 TT samples (IDs8/9,14/15,16/17,18/19) all log RB_DEPTHCONTROL
00700032. DecodeDepthState extracts (control>>4)&7 =3, and native's comparison
table maps3 to LESS_EQUAL. Depth writes are OFF (bit2 clear). Clear z=1 is
consistent with this, so the Addendum19 reversed-Z/clear0 hypothesis does not
apply to those samples. The mistaken GEQUAL premise came from our question,
not from new evidence. Also IDs18/24 are not stable between probe logs:
rebind1 ID18 is TT logo quad start24/base16; ID19 is TT text start18/base12;
rebind1 ID24 is a skinned body draw. Identify draws by source log +geometry,
never by a naked ID from another session.

New static anchor: the full class string is MechKrawlieRenderCharInstAddOn
at821F3E58 (not the substring821F3E5C). ppcref finds830E3954/830E395C in
830E3920 (lazy class-name hash initialization). Its factory830FDE30 creates
a24-byte addon with vtable821F86FC. Vtable render callbacks at+20/+28 point
to830EBA08/830EBA70 ->830E2708. The latter draws addon+16 through82D4F260;
a direct path also reaches82CBCBE8 through82B2BAD0/82AFCE90/82BCFEF8/82CBDAE8.
Whether this is the animated emitter drawing or the visible brick swarm is
still not established. No game launched.
