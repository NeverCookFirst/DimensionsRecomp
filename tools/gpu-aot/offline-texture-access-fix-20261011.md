# Offline texture access fix

October 11, 2026. A reproduced lost-update bug is fixed: borrowed guest
textures now keep their original XDK LockRect and description paths after
entering the native cache. The native executable builds successfully and
all 76 offline renderer tests pass. No game was launched or network used.
The visual cause of every black surface and blinking world remains unproved.

## Reproduced failure and fix

`FindTexture` can adopt a guest resource into the native registry with
`owns_guest_memory=false`. Registry membership previously made LockRect
allocate and return a native CPU mirror. The borrowed texture uploader,
however, consumes the original Xenos base/mip allocations. CPU writes into
the mirror therefore never reach that uploader's source. A later update
can repeat the same failure. The cached description also substitutes
generic native format, dimension and usage metadata for the guest header.

LockTextureResource, DescribeTextureResource and DescribeSurfaceResource
now handle only resources whose guest memory the native renderer owns.
Borrowed resources fall through to the original XDK bodies. A separate
OwnsNativeTextureHeader predicate protects native-created zero-fetch
headers from those bodies, including invalid lock or description requests.
Registry membership remains available for existing cache/reference hooks.
This change does not implement GPU-resolve readback or alter reference
count/destruction semantics.

The production-body regression fails on the old route, passes on the fix,
and fails again when the old route is restored by mutation. It checks two
writes reaching the uploader's allocation, preservation of mip/rectangle/
flags, original descriptions, native invalid-header guards, and routing
after registry removal/address reuse. Its original XDK boundary is a stub;
it does not emulate the complete XDK layout, GPU synchronization or game.
Evidence is under `../session-20261011-offline/guest-access-before` and
`guest-access-after` relative to this source checkout.

## Fetch state and lifetime investigation

The original material binder writes sampler address/filter/mip fields.
It does not account for the captured changes in texture size/dimension.
The original stream-source setter writes the vertex fetch bank after the
26 texture slots, so that path does not overlap the texture fetch bank.
Saved cross-frame headers alone cannot distinguish corruption from legal
allocation reuse. Existing residency, authority, pool invalidation,
descriptor retirement, framebuffer ownership and submission tests pass;
they do not prove there is no runtime lifetime bug.

SetTexture publication now takes the same recording lock as native draws.
The optional existing draw trace records the last SetTexture caller,
ordinal, guest pointer and six fetch words. At draw time it reports both
all changed words and changes to resource fields, excluding legal sampler
updates. The audit tool attaches these records to suspicious bindings.
The last setter is a baseline, not attribution to a subsequent corrupting
writer. No guest fetch state is repaired from historical values.

History is enabled with the existing nonempty
`LEGO_NATIVE_DRAW_TRACE_TRIGGER` environment variable. Old captures lack
this history; identifying the remaining writer needs a future authorized
capture or additional offline evidence. None was launched in this task.

## Validation and candidate

- Full CTest renderer suite: 76/76 passed, 59.52 seconds.
- Material capture audit: three Python unit tests passed.
- Production LockRect/description regression and old-route mutation passed.
- Production draw-diagnostic serialization and fetch-history tests passed.
- Native Release target `legodimensions` compiled and linked using the
  existing prebuilt SDK. There was no SDK or shader-bank rebuild.

The candidate executable is saved privately at
`../session-20261011-offline/native-candidate/legodimensions-texture-access-fix.exe`.
SHA256: `921B02C972814AF264FF4F361893C066E9F572379A75980C8B4AB27D5DD467EC`.
The previous executable is preserved beside it as `legodimensions-before.exe`,
SHA256: `8D1E0F744D5CEF3CC374F6CC323EBCE9629C5BD7F564E354EDF1A5E29322E072`.
The validation build directory contains the newly linked candidate; the
root game installation was not deployed or started. Shader ABI remains 624.
Independently dirty dependency trees were not edited or staged.

This is a concrete fix for lost CPU texture updates and incorrect resource
descriptions. Game-world visual validation, remaining fetch corruption,
GPU-authoritative pool relocation and wrong blending remain open. FSR work
is unchanged from the preceding offline research checkpoint.
