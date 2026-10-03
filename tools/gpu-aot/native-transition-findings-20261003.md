# Enter transition color investigation, October 3

The user's title screenshot shows a blue LEGO tile and yellow lights, where
the normal title has a red tile and cyan lights. Native trace
`session-20260930/trace-oct03-enter-burst/events.tsv` caught a 1280x720
host-resolved RGBA8 texture at frame2334 using guest swizzle0xA0A (ZYX1),
raw descriptor515, with no logical-RGBA descriptor. The previous correction
created a separate resolved view only for0x60A (ZYXW). Thus this transition
variant exchanged red and blue a second time when sampling native RGBA data.

`NativeColorResolveSwizzle` removes the BGRA storage conversion for all ZYX
RGB layouts while retaining alpha selection, including constant ONE. CPU
uploads keep their original view. `test_color_resolve_swizzle.cpp` checks the
observed red/cyan case, all eight alpha selections by differential comparison
against physical BGRA storage, and unchanged behavior for other RGB layouts.
Core tests and native Release build pass. Post-fix runtime pixels are NOT
verified: the user prohibited further game launches after this test.

Opt-in tracing now records texture color authority and supports a trigger
file `capture-present-frames` containing a frame count (maximum240). This
captures consecutive final textures within the existing probe byte budget;
regular rendering does not enable these readbacks. The diagnostic probe
`oct03-enter-burst` captured240 frames and was closed. Heavy captures are not
valid performance measurements.

The earlier aspect-ratio correction was confirmed in normal title window
captures: the portrait monitor window now has black letterbox bars. This does
not establish the remaining HUD portrait or general gameplay correctness.
