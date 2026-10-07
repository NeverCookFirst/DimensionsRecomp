# GPU shader extractor

Read-only inventory/extraction helper for the native renderer work. It reuses
the project's TT Games DAT/HDR and DFLT readers and never opens an archive for
writing.

```powershell
gpu-shader-extract inventory <game-directory>
gpu-shader-extract extract <game-directory> <output-directory>
```

`inventory` reports the file extensions present in every classic disc archive.
`extract` writes only likely shader-container files (`.vso`, `.pso`, related
extensions, and paths containing `shader`) beneath one directory per archive.
The extracted directory can be passed directly to XenosRecomp.
Names resolve through the shared reader's CRC-first `FindEntry` lookup. In new
archives, the name tree's sequential values can differ from DAT file-table
indices; using those values directly extracts the wrong payload. In particular,
TU23's named `*_nxg.360_shaders` can contain the source programs whose runtime
copies omit optional reflection. Keep the original metadata during extraction.

To audit every indexed entry, including unnamed entries and containers embedded
in other resources, use the opt-in streaming mode:

```powershell
gpu-shader-extract extract --embedded <archive-or-directory> <fresh-output-directory> --max-seconds 300
```

This scans a single `.DAT`/`.DAT2` or all such files directly beneath a directory,
using the actual archive reader and DFLT decoder. Raw and decoded data are read
in 2 MiB chunks; no entire archive entry is allocated. It searches every possible
container start, including nested/overlapping records, with the same metadata,
reflection and physical-range validator used by image mode. Exact complete
source containers share files by SHA256; reflection is never stripped or rebuilt.
Each entry's output remains provisional until its entire compression framing
and declared decoded size pass validation.
This checks DFLT chunk headers, decoder success and exact produced byte counts,
including short and overlong output. It retains the shared decoder's bitstream
semantics, including zero padding; it is not a new strict bitstream validator.

`embedded-index.json` reports completion, per-archive inventory, bounds and
unvisited ranges. `entries.jsonl` records every attempted file-table index,
CRC-first resolved name aliases, source range, decoded SHA256 and explicit
failures. `containers.jsonl` records each accepted source entry/decoded offset
and full/physical/instruction SHA256 identities. Names inherit the shared
reader's tree fallback when CRC lookup fails; the index states this policy.
Bounded header identities and decoded entry hashes are recorded; this mode
does **not** hash an entire multi-gigabyte archive.

The default deadline is 300 seconds, adjustable from 1 to 3600. Limits include
64 MiB headers, 1 GiB stored/decoded entries, 2 MiB DFLT chunks, 64 GiB total
decoded work, 100000 candidate starts, 4096 starts per entry, 1 GiB candidate
hash work and 512 MiB output. Unsupported name layouts, sparse archives,
oversized entries, decoder failures and deadline exhaustion are explicit;
an incomplete scan publishes its ledger with exit **3**, never success.
Exit 0 is a completed scan with structural candidates; 1 is a completed scan
without candidates; 2 rejects arguments/input/output. Successful extraction
still requires separate instruction decoding and shader compilation. It does
not establish rendered correctness or whole-campaign coverage.

Executable-resident shaders can also be extracted without starting the game:

```powershell
gpu-shader-extract extract --image mapped-image.bin executable-shaders --image-base 82000000 --image-provenance loader-provenance.json
```

The input must contain the **mapped, decoded executable image after applying
the intended title update**. Passing an encrypted/compressed `Default.xex` or
its patch file does not substitute for SDK loading. The load address must be
supplied explicitly. Producing this image with the SDK loader is a separate
step; this tool does not load or execute guest code.

Optional loader provenance must contain a matching record:

```json
{"mapped_image":{"base":"0x82000000","bytes":45613056,"sha256":"<actual image SHA256>"}}
```

Additional source XEX, update and loader identities are retained in the output
index. Matching the supplied record checks image identity, not the provenance
author's authenticity.

Image extraction scans all image bytes with bounded memory, validates container,
shader metadata, optional reflection and literal-definition ranges, and writes
complete containers beneath a **fresh** output directory. The four-byte archive
physical-size marker is retained when present. Identical full containers share
one file; every discovered image/guest address remains in `image-index.json`.
That index includes SHA256 and eight-byte prefixes for full containers, physical
sections and instruction streams. The existing `lego_gpu_microcode_index` tool
can produce the renderer's XXH3 placement lookup from these files.

Input is limited to 512 MiB, each section to 4 MiB and discovered records to 10000.
Every possible header start is searched, including nested and overlapping
containers. Total candidate bytes are capped at 512 MiB to bound repeated
hashing and output. These are bounded
**structural candidates**: shader compilation must succeed separately before a
bank is used. Neither extraction nor compilation establishes full campaign
coverage or rendered correctness. Exit 1 means no image candidates were found;
exit 2 means rejected arguments, bounds, provenance or output.
