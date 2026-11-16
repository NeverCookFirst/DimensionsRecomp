# LOTR offline geometry checkpoint

The user rejected shader-lifetime candidate 082d13f: no visible change. Live
comparison with FSR enabled and disabled showed white terrain, mottled foliage
and stretched black geometry in both runs. The latest user request was to close
the game and work offline. Closed the selected game with Alt+F4 and verified
that its process exited. No subsequent launch, network access or frame generation.

## Investigation

The older LOTR draw capture contains 192 draws, all triangle lists. Its recorded
position samples are finite and modest in magnitude (largest absolute FLOAT
sample about 56.84). This excludes neither bad indices elsewhere nor bad shader
transforms: only four corners and six indices per draw were recorded. Half-float
positions are also present. No current affected-frame RenderDoc capture exists
in the inspected evidence.

Inspected native buffer adoption, immutable conversion ownership, range rebasing,
declaration formats, pair swaps, alpha test and MRT write-mask selection. No
evidence-supported LOTR rendering change emerged from those checks. A local
instruction audit of the saved containers found no vector/scalar source overlap
of the suspected kind; do not deploy a speculative ALU ordering patch. The new
live log contains no reported texture-binding failure, though bounded warnings
and incomplete capture coverage cannot prove every binding correct.

## Implemented diagnostic prerequisite

The CPU draw diagnostic can now optionally save complete raw guest index/vertex
resources, all 256 VS and 224 runtime PS floating-point constants, and per-target
attachment/write masks. Every used vertex stream is eligible, including blend
indices/weights. Source data remains unchanged; no GPU upload memory is read.
Blobs are content-named and retained independently when the same address changes.

Opt-in TOML settings:

```
gpu_native_draw_trace_trigger = 'absolute path to capture-draws'
gpu_native_draw_trace_geometry = true
```

The existing environment trigger takes precedence. With no trigger file,
capture is idle. Existing `skip=700`, `ps=963f001993939275` and
`pair=0,963f001993939275` commands select the next frame, at most 64 draws.
Up to four requests per process retain numbered outputs. A pre-created trigger
at startup would capture the wrong scene; create it only at the affected scene.

Bounds per request: 16 MiB per geometry resource, 64 MiB unique blob storage,
128 MiB attempted source reads, 4 MiB text. Protected reads remain at most 768
bytes each. Invalid, unreadable, oversize, failed and budget-exhausted resources
are explicitly labeled. This is a geometry diagnostic, not a complete GPU replay:
textures, integer/boolean constants and concurrent guest-write coherence still
need separate evidence. Capture frames must not be used for FPS measurements.

## Validation and delivery

Production extracted recorder/reader test passes, including byte preservation,
same-address mutation, deduplication, protected partial reads, overflow/size/read
budgets and expanded record size. Targeted CTest `draw_diagnostic` passed in
2.68 seconds. Release executable built successfully; no gameplay test.

Package: `../session-20261012-offline/native-lotr-geometry-diagnostic/`.
Fingerprint: `6640e13d301f07c4`.
Executable SHA256: `11df3c36dd70accf16ac485e917b4a5016820f153ad1edd1eabc612019ce0f3a`.
FSR off, frame generation off, monitor 1, borderless fullscreen. Previous
package preserved. **No LOTR visual fix is claimed.** A synchronized affected
draw capture remains necessary to attribute the stretched geometry and avoid
another unproven rendering patch.
