# Monitor3 format/HUD/LOTR diagnostic, October4

Owner reached Vorton and LOTR in diagnostic PID1100, started15:21:49 Moscow.
Build27794e2f1412daae, exe89d48797bd6944c1724bd6c167952cd8dc93792ecbfadf2cf89dd21692f8ca09.
The prior manual PID24428 was captured and closed before launching diagnostics.
No SDK/plugin/config changes; native D3D12, actual720p/output1080p. The saved
configuration has depth_of_field=false and both runs log the native DoF-disable
path. Remaining softness is not established to be DoF.

The owner-assisted screenshots still show blank portrait circles, texture
stripes and tall LOTR artifacts. These are failed visual checks, not repaired
results. Diagnostic timings are not clean gameplay performance benchmarks.
PID1100 was closed after capture. No further launch is authorized by this run.

Local evidence under session-20261004/format-forensics1:

- hub.png / lotr.png; saved log and exact process record.
- hub-events.tsv / lotr-events.tsv; live mask owner/material/backend/header
  snapshots. One portrait_setup with all fields readable; no matching
  portrait_bound_draw in the captured trace. That establishes no matched mask
  binding, not absence of every possible portrait draw or the cause of failure.
- Automatic full-mip upload dump:32 textures at hub frame2009, with exact
  guest spans. Its per-process32-texture limit exhausted in the hub, so it did
  not collect subsequent LOTR textures. Future multi-scene captures need a
  separate budget/arming design; do not present these as LOTR mip evidence.
- LOTR:15 recent texture headers, six large BC1/BC3 base memory spans. All six
  headers remained stable before/after reading. The external reader originally
  omitted the Windows E/F guest alias+4096 host offset. These raw spans contain
  a preceding page and lack the last4096 source bytes. Initial decoded-bases
  PNGs are diagnostic-reader artifacts, not evidence of renderer corruption.
  The manifest explicitly marks these limitations. Object headers at A/B
  addresses are unaffected. Do not infer intact whole mips from partial spans.
- Recursive missing-shader review found five new complete containers under
  placement-containers, in addition to the known A58 memexport container. The
  original top-level-only count missed these nested files. The captured PS
  identities are35F76DC3372D5773/DBFA1692650B44C6; VS identities
  1AFAC761791379C0/43DF3B3B6D90016E/45C2959A30C6DCAE. All five compile,
  with exact captured identities added to the runtime archive/index. Incomplete
  placement-raw spans are excluded; no shader substitutes are guessed.

The SDK's actual mapping code confirmed the reader offset issue. The reader now
matches Windows E/F translation, supports explicitly bounded spans up to8MiB,
at most32 addresses/128MiB total and retains executable identity verification.
test_read_guest_memory.py executes real ReadProcessMemory against an isolated
synthetic child: ordinary/alias pages,1024/4096 byte reads, invalid limits,
address overflow and wrong executable all pass. No game is launched by this test.

## Gradient query correlation

Two of the four hub_lotr_05 gradient shaders are actually referenced in this
run's texture binding trace:

- FBD34779A28C36BE:14 recorded references.
- D70DC053F574C4C6:13 recorded references.

The other two have no recorded reference. Rows are preserved in
active-gradient-events.tsv. The isolated GetTextureGradients correction is
therefore relevant to active LOTR materials; it is not a proven fix for the
entire screenshot. It preserves the624-byte ABI and does not include rejected
explicit-LOD changes. Matching main archives are now installed in the offline build; image
validation remains pending.

## Offline capture rearming repair

The diagnostic capture now accepts up to four fresh scene names in the trigger
file, with32 textures per scene and the same128MiB cumulative payload ceiling.
Scene output directories preserve earlier captures. A reused scene, malformed
name/path or fifth scene is refused. Trigger checks precede the per-scene quota
check, so exhausting hub quota cannot prevent later rearming. Legacy logo
captures retain their existing base-only behavior. Capture remains opt-in.

For a future authorized probe, use `run_native_probe.ps1 -Label LABEL
-CaptureStage hub`, then a fresh stage such as lotr only after owner readiness.
This command validates the exact running process and settings, writes bounded
ASCII stage text through atomic file replacement, and does not launch a game.
The actual capture-body tests pass scene rearming after quota exhaustion,
preserved DDS/raw bytes, scene/path/count refusal and retained global budget.
