# LEGO Dimensions native D3D hook map

Target: TU23 `Default.xex`, mapped image `xexdump/dump/default.bin` at
`0x82000000`. The embedded XDK shader compiler identifies the SDK family as
`xdksep13`; its shader compiler reports `2.0.21256.0`.

Do not infer a hook from function size alone. Several LEGO functions happen to
have the exact size of a named re:Blue XDK function while implementing unrelated
work.

## Confirmed by instruction semantics

| Guest address | Meaning | Evidence |
|---|---|---|
| `0x83FB7DA0` | `D3DDevice_SetRenderState_CullMode` | Replaces bits 0..2 of the device raster state and marks the raster group dirty. |
| `0x83FB7DD0` | `D3DDevice_SetRenderState_FillMode` | Replaces the adjacent fill-mode bits and marks the same group dirty. |
| `0x83FBA068` | `D3DDevice_SetScissorRect` | Reads a four-dword LTRB rectangle, clamps it to the current surface, and stores the scissor shadow. |
| `0x83FBA9F8` | `D3DDevice_SetViewport` | Loads X/Y/W/H/MinZ/MaxZ from the six-field viewport and tail-calls the viewport state writer. |
| `0x83FBA160` | texture state writer | Indexes the device texture slots and consumes the resource fetch address at offset `+0x1C`. The public entrypoint still needs naming. |
| `0x83FBAAA8` | render-target state writer | Indexes four surface slots, stores the bound surface and its fetch address, and marks the corresponding target dirty. The public entrypoint still needs naming. |

## High-confidence control-path anchors

| Guest address | Meaning / role | Evidence |
|---|---|---|
| `0x83FAF9A0` | `Direct3D_CreateDevice` | Six-argument ABI matches `(adapter, deviceType, focusWindow, behaviorFlags, presentParams, outDevice)`: clears `*outDevice`, allocates `24704` (`0x6080`) bytes aligned to 128, initializes the allocation, then stores it through the sixth argument. |
| `0x83FC9450` | reset/default-state initializer | Walks the render- and sampler-state descriptor tables, installs getter/setter dispatch pointers in the device, applies defaults, and clears all 26 fetch slots. Calling it from the native path is unsafe because applying the defaults also emits PM4. |
| `0x83FAF180` | ring-buffer blocker poll | Checks the device read pointer and timeout state; called from the blocking wait loop. |
| `0x83FBF7D0` | ring-buffer space wait | Compares requested and available ring space and loops through `0x83FAF180`. |
| `0x83FB33E0` | surface copy/resolve path | Consumes a 52-byte surface descriptor, rewrites Xenos formats and emits copy/resolve packets. It is not safe to name this `Swap` from position or size alone. |
| `0x83FB0DA8` | scanout-buffer programming helper | Takes `(device, buffer descriptor, flags)`, walks descriptor fields and emits display/ring packets. A caller at `0x83FC4994` passes one of the device's rotating 16-byte buffer records. This is **not** `Direct3D_CreateDevice`. |

## TU23 device layout anchors

The Sep'13 device is `0x6080` bytes, not the `0x5000`-byte device used by the
older re:Blue target. The reset loop at `0x83FC9450` proves these offsets:

| Device offset | Field |
|---|---|
| `0x040` | 101 render-state setter entrypoints |
| `0x1D4` | 20 sampler-state setter entrypoints |
| `0x224` | 101 render-state getter entrypoints |
| `0x3B8` | 20 sampler-state getter entrypoints |
| `0x480` | 26 six-dword fetch constants |
| `0x32E0` | cached viewport (`X/Y/W/H/MinZ/MaxZ`) |
| `0x32FC` | cached scissor rectangle (`left/top/right/bottom`) |

The render-state descriptor triples begin at `0x847F9B18`; the sampler-state
descriptor triples begin at `0x847F9FD8`. Each triple is
`{ getter, setter, default }`. The native create hook copies only the dispatch
pointers and seeds host-visible shadows directly, avoiding the original
initializer's ring traffic.

## Function-boundary source

The range beginning at image address `0x8260E400` is `.pdata`: pairs of a
function entrypoint and unwind metadata. It is useful for exact boundaries, but
it is not a D3D dispatch table and therefore does not provide names.

## Fast path to a complete map

The preferred input is the user's own `xdksep13` build 21256 static graphics
library (normally `d3d9.lib` plus its companion graphics libraries). Normalize
relocations in each PowerPC COFF member, then match instruction windows against
the decrypted image. Ambiguous matches must be resolved by call graph and
argument semantics before a hook is enabled.
