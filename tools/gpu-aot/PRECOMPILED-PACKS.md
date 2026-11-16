# Additive precompiled shader packs

The native renderer can load missing shaders compiled offline without restarting
the game. This development feature is disabled by default. It never compiles
shaders at runtime or replaces shaders already present in the executable.

Set both `LEGO_NATIVE_SHADER_PACK` and `LEGO_NATIVE_SHADER_PACK_TRIGGER` before
launching. Each must name a path readable by the running Windows process. Under
Wine, use the corresponding Windows drive paths. The first names the pack file;
the second names a trigger file that the renderer consumes once.

Build a strict candidate with `prepare_native_compiler.py`, then export its
additions against the candidate actually compiled into the running executable:

```sh
python3 tools/gpu-aot/export_precompiled_pack.py \
  --candidate /path/to/new-candidate \
  --baseline /path/to/running-build-candidate \
  --build-info /path/to/build/generated/native_gpu_build_info.h \
  --output /path/to/additions.pack
```

For subsequent packs, repeat `--previous-pack /path/to/accepted.pack` for every
pack accepted by this process. Keep those files immutable. Check the export
report, publish the completed pack by an atomic rename, and only then create the
trigger. A rejected pack consumes its trigger; inspect the log before retrying.
Look for `precompiled_shader_pack_loaded` and its generation in the trace, then
verify that the affected draws recover. Acceptance alone does not prove coverage.

The exporter checks candidate provenance and output hashes. It does not install
the pack, write a trigger, or change the active shader bank. A renderer rebuild
changes the required fingerprint, so packs must be exported again for that build.

The runtime checks the exact build fingerprint, constants ABI, common shader
header and compiler revision, checksum, complete specialization variants, DXIL
stage and chunk bounds, and physical microcode identities. Publication is atomic;
failed validation leaves the archive unchanged. Published bytecode and lookup
pointers remain valid until process exit, including across resource resets.
Missing bindings can recover; intentionally null pixel bindings remain null.

Each pack is limited to 64 MiB and 256 shaders. A process accepts at most 64 packs
with a combined input size of 256 MiB. Restart with a rebuilt bank when those
limits are reached.

Run the production archive and binding fixture without game assets:

```sh
python3 tools/gpu-aot/test_precompiled_pack.py /tmp/lego-pack-check \
  --compiler clang++ --verify-regression
```

The fixture covers malformed and truncated packs, publication rollback,
specialization lookup, stable pointers, binding recovery, and a negative control
that removes the provenance rejection. It does not validate an entire campaign.
