# Native buffer generation candidate, October 3

Previous title measurements attributed9.51ms/frame to buffer hashing; the
window candidate reduced bytes hashed but did not prove an overall FPS gain
(see `native-performance-20261001.md`). This candidate avoids full content
hashes when the SDK's physical page generations prove an upload unchanged.

Selection: `gpu_native_buffer_watch=true`, or `LEGO_NATIVE_BUFFER_WATCH=1`.
Default OFF. Probe switches `-BufferWatch` and `-AuditBufferWatch` explicitly
select it; audit also sets `LEGO_NATIVE_AUDIT_BUFFER_WATCH=1`. The global
`LEGO_NATIVE_NO_MEMORY_WATCH` disables both texture and buffer watches.
Texture and buffer selection remain independent. Vertex animation textures
continue to hash on every use.

Physical writes through guest aliases invalidate shared generations. A dirty
range arms its watch before hashing/conversion. Unknown virtual backing falls
back to hashing every use. Changed contents still create immutable upload
versions retained through their frame fence. A watch hit can reuse a different
already-cached declaration conversion, or create that variant without changing
older uploads. Host physical pool copies explicitly invalidate generations.
Device shutdown invalidates every stamp.

Audit hashes every otherwise-clean range, reports stale clean stamps as errors,
and repairs the conversion using the new data. Timing logs include watch hits,
audit checks and mismatches. Audit runs measure safety, not speed.

Offline verification passed:

- Actual production buffer conversion/adoption/window functions: existing10000
  fetched-byte equivalence and1000 rebasing cases, same-frame updates, immutable
  older uploads and bounded cache. Additional cases reuse100 watched draws with
  zero source hashing/allocation, re-invalidate on two same-frame writes, detect
  and repair a deliberately missed notification under audit, and hash every
  unknown-memory draw. Tests use a fake GPU driver and page-generation model.
- Production watcher with SDK callback model: alias mapping, races, neighbor
  protection, physical copies, fallback and global disable. Repeated with
  buffer-only selection while texture watches are disabled.
- Actual installed SDK: real protected-page CPU writes,100 rearm cycles,
  Protect, explicit host physical writes and Decommit. Passed environment and
  config selection for both texture and buffer watches.
- Native Release build and embedded build fingerprint verified.

No new gameplay or FPS measurement. The user prohibited launching the game
after the Enter capture. This remains opt-in until a monitor2 gameplay audit
and clean, comparable performance run are authorized. Async CPU-resource
wait bypass also remains OFF; this change does not alter its safety question.
