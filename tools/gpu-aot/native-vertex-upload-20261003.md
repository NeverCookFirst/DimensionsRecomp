# Fused packed-vertex upload, October 3

The old packed-vertex path allocates and zeroes a temporary CPU buffer, swaps
all DWORDs, reverses UBYTE4 fields, then copies the entire result into the D3D12
UPLOAD mapping. These reads/writes are expensive even though the final mapped
copy is write-only. The measured title vertex cost was10.84ms/frame on October1.

`WriteAlignedVertexUpload` combines the two permutations into a single
sequential write from guest bytes to the upload mapping. On the game's x86 ISA,
SSSE3 shuffles use a bounded repeating mask table. The helper reads no mapped
destination bytes and allocates no temporary full buffer. It accepts aligned,
sorted, distinct packed fields in strides4..256; unusual declarations retain
the original conversion. Upload versioning, hashes, fences and guest RAM are
unchanged. This exact-byte optimization is active without selecting watches.

Verification:

- `test_vertex_upload.cpp`:30000 differential cases against the old two-step
  conversion, varied strides/phases/fields, misaligned source/destination
  pointers, partial records and tails, guarded output bounds, and untouched
  output on rejected layouts. SSSE3 build passed.
- Actual buffer resolve/adoption/window body tests pass with the new helper:
  immutable old uploads, same-frame edits,1000 rebasings, buffer watches and
  audits. Native Release build passed.
- `benchmark_vertex_upload.cpp` creates a D3D12 device and committed UPLOAD
  buffer on RTX4060, with no game, window, swap chain or input. One6MiB synthetic
  stride24 layout with packed offsets8/12/16/20 is converted identically by both
  paths and compared byte-for-byte on the real mapping. Each trial performs40
  writes; trial order alternates. Clang20.1.8,-O2,-march=x86-64-v3:
  old7.67321/7.59433/8.03351ms, fused0.624520/0.576703/0.585677ms.
  Raw log: `session-20261003/vertex-upload-benchmark-v3.log`.
  Repeated at the actual native build's-O3 ISA flags: old6.56941/6.61659/
  6.52649ms, fused0.548512/0.405378/0.413095ms; raw log
  `session-20261003/vertex-upload-benchmark-native.log`.

Reproduce after loading `E:\devtools\env.ps1`:

```
clang++ -std=c++20 -O2 -mssse3 -Irexlego/src tools/gpu-aot/test_vertex_upload.cpp -o <test.exe>
clang++ -std=c++20 -O2 -march=x86-64-v3 -DNOMINMAX -Irexlego/src tools/gpu-aot/benchmark_vertex_upload.cpp -ld3d12 -ldxgi -o <benchmark.exe>
```

These are CPU conversion measurements on a synthetic layout, not gameplay
FPS, frame-time or visual correctness measurements. No post-change game launch
was performed because the user prohibited it. A clean comparable hub/level run
must establish the actual benefit and check the full scene once authorized.
