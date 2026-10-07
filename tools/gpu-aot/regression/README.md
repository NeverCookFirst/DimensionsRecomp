# Private renderer regression runs

These Linux/Proton helpers prepare a real saved-game checkpoint for repeatable
renderer experiments. Preparation and reporting never launch, send Toypad
packets, capture the desktop, or press keys. `execute_run.py` launches only with
explicit `--launch`; it refuses another game and locks execution within the
chosen workspace. Use one workspace for all concurrent operators.

Python3.11+ and Linux reflink support are required for large inputs. Unsupported
reflinks fall back only for individual files ≤1MiB, with an aggregate8MiB copy
budget. Copies have independent inodes. Failed preparation is preserved for
inspection and cannot launch. There are no included game assets or machine paths.

## Inputs

Supply these independently recorded inputs:

- A baseline directory containing `legodimensions.exe` and
  `baseline-manifest.json` with `build_fingerprint` (16 lowercase hex digits)
  and `executable_sha256`. Record the source/dependency provenance here too.
  `frozen_dependencies.rexruntime.dll.sha256` pins the exact compatible runtime;
  a matching imported symbol set cannot replace that ABI identity.
- A staged installation with `legodimensions.toml`, the four primary binaries,
  support files, and `native-staging-manifest.json`. Its `binaries` maps names
  to `size`/`sha256`; `imports` lists each primary PE's actual imported DLL names;
  `graphics` maps `dxgi.dll`, `d3d12.dll`, and `d3d12core.dll` to their source paths.
- A legitimate checkpoint directory with flat `savegame_1` files and
  `save-metadata.json`: each filename maps to its `bytes` and `sha256`.
  `GAME01`, `V2GAME01`, and `OPTS01` are required. Supply its original profile XUID.
- A tag directory with your persistent180-byte `batman.bin`, `gandalf.bin`,
  `wyldstyle.bin`, and `batmobile.bin`. Prepared LOAD packets refer to private
  backing files; this tool does not transmit them. The starter layout is Batman
  pad2/index4, Gandalf2/0, Wyldstyle3/6, and Batmobile1/1. Only place figures the
  scene recipe calls for; preparing the car does not load it.
- Proton, its data prefix, Steam's client directory, and a distinct Toypad port.

The tool freezes private profile/install/DLC supporting files, checkpoint,
update, mod-update, mods, cache, metadata and tag paths. Game disc data remains a
shared asset reference with game-relative writes disabled. The Proton prefix is
explicitly selected and may be shared across sequential runs. Large supporting
files use per-file COW snapshots and are not rehashed: this is not an atomic
whole-machine save state. Close source writers before preparing twins.

```sh
python3 tools/gpu-aot/regression/prepare_run.py prepare slots3-a \
  --workspace "$WORKSPACE" --runs "$WORKSPACE/runs" \
  --baseline "$BASELINE" --install "$STAGING" \
  --checkpoint "$CHECKPOINT" --xuid "$XUID" --tags "$TAGS" \
  --proton "$PROTON" --proton-prefix "$PREFIX" --steam-client "$STEAM" \
  --toypad-port 19207 --buffer-windows --cadence-only --command-slots 3 --timer-wait spin
python3 tools/gpu-aot/regression/prepare_run.py report "$WORKSPACE/runs/slots3-a" \
  --workspace "$WORKSPACE"
python3 tools/gpu-aot/regression/execute_run.py "$WORKSPACE/runs/slots3-a" \
  --workspace "$WORKSPACE"                  # verify and print only
python3 tools/gpu-aot/regression/execute_run.py "$WORKSPACE/runs/slots3-a" \
  --workspace "$WORKSPACE" --launch         # explicit separate action
```

Preparation rejects customized mod/modcli configurations. Cadence mode removes
`LEGO_NATIVE_TIMING` completely, enables frame CSV metrics, and omits LongProbe,
draw triggers and snapshots. A value of `"0"` still enables presence-based
switches, so it is not equivalent to absence. Normal performance mode retains
detailed timing; the two modes have different workloads.

`--timer-wait spin` preserves the default by omitting `REX_TIMER_WAIT_BLOCKING`.
`--timer-wait blocking` explicitly sets it to `1`; this restart-only SDK adapter
changes timer wake scheduling and remains an experiment. Matching imports and
lower background CPU usage do not prove equivalent deadline precision or FPS.
For a timer comparison keep command slots fixed and pass
`--vary timer-wait` to `compare_samples.py`. Its default `--vary command-slots`
requires timer mode to match. Varying both settings rejects comparability.

For a build supporting native GPU timestamps, add `--gpu-timestamps` to write
`logs/gpu-timestamps.csv` separately from CPU cadence metrics. Query writes and
readback add diagnostic overhead. Command-slot and timer comparisons require
the same probe presence. To measure probe overhead explicitly, keep slots,
timer mode and all other settings fixed and pass `--vary gpu-timestamps` to
`compare_samples.py`; it groups every accepted window as `off` or `on`, retains
rejected windows, and reports `on` relative to `off`. Only probe presence and
its verified private output path may differ; this mode changes no defaults.
The spans measure elapsed queue execution within individual command submissions,
not GPU utilization, CPU command recording or DXGI presentation. A descriptor
only requests the probe; startup evidence and actual completed rows must confirm
availability on the selected device. PM4 cannot request this native probe.

For a PM4 reference use `--renderer pm4`, optionally
`--render-target-path-d3d12 rtv`, and provide `--pm4-plugin`,
`--pm4-plugin-sha256`, `--pm4-runtime-sha256`, and
`--pm4-compatibility-proof`. The proof is JSON with those two SHA256 fields and
a nonempty `evidence` string recording why that exact ABI pair is compatible.
The plugin's runtime imports are checked against actual exports, but symbol
closure alone does not establish internal ABI compatibility. PM4 cannot use
native diagnostic flags. Actual runtime route logs and visible rendering are
still required; a launch descriptor does not prove either.

## Fixed cadence intervals

Use normal controls to load the checkpoint and required figures, without moving
the camera or player. Publish any additive packs separately with the production
exporter/parser; build fingerprint gates remain strict. When packs are used,
record `hotload/publications.json` as an ordered list of
`{"generation":1,"source":"/path/to/frozen.pack","sha256":"..."}` entries.
The sampler verifies the source SHA/header build identity and every generation's
actual acceptance log. It never publishes packs itself.

Capture the intended scene before sampling. Create a scene-confirmation JSON:

```json
{
  "run_root": "/absolute/private/run",
  "build_fingerprint": "0123456789abcdef",
  "renderer": "native",
  "scene_id": "powered-portal-default-vorton",
  "camera_id": "untouched-load-camera",
  "stationary": true,
  "camera_unchanged": true,
  "inputs_released": true,
  "workers_held": true,
  "capture_path": "/path/to/before.png",
  "capture_sha256": "64 lowercase hex digits",
  "figures": [{"name":"batman","pad":2,"index":4,"uid":"recorded UID"}]
}
```

Include every actually loaded figure with its prepared manifest UID/placement.
Scene identity and input ownership are explicit operator evidence; the scripts
do not infer them from pixels. Hold compiler/scanner/profiler workers throughout
warmup and sampling. The sampler records endpoint process observations as a
supplement, not continuous absence proof. It requires the actual native route
and initialized slot count in the game log.

```sh
python3 tools/gpu-aot/regression/sample_cadence.py "$RUN" \
  --workspace "$WORKSPACE" --output "$WORKSPACE/samples/slots3-a" \
  --scene-confirmation "$SCENE_JSON"
```

Each attempt uses the same fixed settling gate: ten seconds with no new texture
adoption, buffer relocation, texture refresh, missing shader or pack load; frame
IDs must progress and the last60 rows' half-window medians must differ by at
most25%. Warmup is bounded at120seconds, then one fixed30-second interval is
recorded. No screenshots or input belong inside that interval. Streaming,
observed workers, or renderer failure reject the window while preserving its
CSV/log/outcome. Interrupted attempts also retain rejection evidence. Outputs
cannot be overwritten; give retries distinct names.
The sampler reads at most a4MiB CSV tail and retains at most10,000 rows.
Context logs are capped at16MiB and interval evidence at8MiB; excessive logging
fails closed instead of growing memory without bound. Execution evidence uses
atomic JSON publication and bounded reads.

Capture the scene again afterward and pause normally. Prepare separate3/12/3
runs from the same inputs, keeping every outcome. Feed all outcomes, including
rejected ones, to the comparison:

```sh
python3 tools/gpu-aot/regression/compare_samples.py \
  "$WORKSPACE/samples/slots3-a" "$WORKSPACE/samples/slots12-b" \
  "$WORKSPACE/samples/slots3-a2" --output "$WORKSPACE/comparison.json"
```

Comparison checks binary/runtime/DLL hashes, initial checkpoint, normalized
configuration, flags, packs, criteria and confirmed scene/figures. It validates
metrics against frozen CSVs, aggregates all accepted windows, and never chooses
the fastest window or changes the source default. NPC/particle phases and
ordinary desktop activity remain limitations. CSV flushing shifts the recorded
duration relative to wall endpoints. Disabled stage-time/hash/conversion columns
measure no timing/work quantity. Native present intervals and PM4's XE_SWAP
request intervals have different boundaries and draw-counter scopes.

## Asset-free verification

```sh
python3 tools/gpu-aot/regression/test_regression.py
```

Tests exercise actual independent copies, private writable paths, real packet
construction, bounded fallback, corruption/alias rejection, synthetic static
PE imports, environment sanitization, cadence descriptors and fake-clock fixed
sampling/comparison. Synthetic PE files are never executed. These checks do not
validate game pixels, actual driver timing or campaign completion.
