`xenosrecomp-empty-reflection.patch` targets XenosRecomp revision
`339af41df2c23dbe3256c1c377716b81a0e0fe6b`. It also applies after the existing
LEGO compatibility and ABI624/viewport/gradient preparation steps.
`prepare_native_compiler.py` applies it only to the freshly prepared compiler
source and records its SHA-256 in `compiler-inputs.json`.

Valid runtime shader containers may have `constantTableOffset == 0`. CTAB names
are optional reflection: the compiler already resolves unnamed float constants,
samplers and boolean bits through their register indices, and decodes vertex
inputs/interpolators from the shader metadata. The patch selects an explicit
static empty reflection table for those containers. A present CTAB retains its
original input pointer and all three reflection loops. Literal definitions and
microcode translation are unchanged, including rejection of unsupported
memexport shaders.

On October 6, 2026, the rebuilt Linux native compiler produced DXIL for the actual
captured shaders `ps-20B156FC6D4EF62D`, `ps-31CEA43B9FE57F6F`,
`vs-34EB25B494B1B971` and `vs-71CEE6F3353CE415`, all without CTAB, plus the
present-CTAB control `ps-025D4B4261D8A4D1`. All five generated HLSL files and
their complete compressed bank were byte-identical to the previous Release
compiler's output. The previous compiler happened to read the zero CTAB offset
in the container header as a zero reflection count when assertions were
disabled; an assertion-enabled build rejected the valid inputs. The new path
does neither. This check used `XENOS_RECOMP_DXIL_ONLY=1`; it makes no SPIR-V
coverage claim.

Local captured-input hashes, compiler/source/patch hashes and equivalence
results are recorded in
`.local-testing/game-launch/native-compiler/empty-reflection-test/verification.json`.
The supplemental patch was also checked against the exact pinned source and
the prepared source. No native application build or game launch was part of
these checks.
