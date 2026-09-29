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
