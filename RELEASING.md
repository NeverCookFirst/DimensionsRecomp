# Cutting a release

Everything a release needs is produced by one script. The only manual part is
the GitHub release itself, because attaching the files is what makes the
updater see the new version.

## 0. Before anything

1. Build the game and **verify the artifact** - ninja can report success
   without relinking because of the space in the path:

   ```powershell
   . E:\devtools\env.ps1
   cmake --build "E:\Claude\LEGO Dimensions\rexlego\out\build\win-amd64-release" --target legodimensions
   Get-Item "E:\Claude\LEGO Dimensions\rexlego\out\build\win-amd64-release\legodimensions.exe" |
       Select-Object Length, LastWriteTime
   ```

   The timestamp must be from this build. If it is not, the build did nothing.

2. Bump `VERSION` in the workspace root (`1.0.0` -> `1.0.1`). The build script
   reads it, `-p:Version` stamps it into the installer, and the updater
   compares against it.

## 1. Build

```powershell
cd "E:\Claude\LEGO Dimensions\rexlego-installer"
.\build-installer.ps1                       # version from ..\VERSION
.\build-installer.ps1 -Notes "Fixes the X"  # notes shown in the updater
```

Out comes:

| File | What it is |
|---|---|
| `dist\DimensionsRecompiled-Setup.exe` | ~175 MB, full install, no dependencies |
| `dist\update-<version>.rxp` | only the files that changed since the previous release |
| `releases\<version>.json` | the manifest of this release - **commit it** |

`update-<version>.rxp` only appears when `releases\` already holds an earlier
manifest to diff against. The very first release (1.0.0) has no pack, which is
correct: there is nothing to update from yet.

`releases\<version>.json` is what makes the *next* build's pack small. If it is
not committed, the next release ships a pack the size of the installer.

## 2. Smoke-test

- Install `dist\DimensionsRecompiled-Setup.exe` into a throwaway folder.
- Start the game once. F4 -> **Updates** must list `updates_check`,
  `updates_repo`, `updater_path`.
- `tools\rexupdate\rexupdate.exe --check --console` must print either
  "No newer version" or the version it found, and exit cleanly.

## 3. The GitHub release

Repository: **NeverCookFirst/DimensionsRecomp**

```powershell
cd "E:\Claude\LEGO Dimensions"
git add -A ; git commit -m "Release 1.0.1" ; git push
git tag v1.0.1 ; git push origin v1.0.1
gh release create v1.0.1 `
    "rexlego-installer\dist\DimensionsRecompiled-Setup.exe" `
    "rexlego-installer\dist\update-1.0.1.rxp" `
    --title "Dimensions Recompiled 1.0.1" --notes-file notes.md
```

Rules the updater depends on:

- The tag is `v<version>`; the version inside it must match `VERSION`.
- The installer asset is named exactly `DimensionsRecompiled-Setup.exe`.
- The pack asset ends in `.rxp`.
- The release body is what the updater shows as the changelog.
- A pre-release is picked up too, so mark a test build as a pre-release only
  if you do want testers to be offered it.

## 4. The repository must be readable

The updater talks to the public GitHub API. **While
NeverCookFirst/DimensionsRecomp is private, nobody's updater can see or
download a release.** Either

- make the repository public before the release, or
- keep it private and drop a personal access token in
  `tools\rexupdate\token.txt` on each tester's machine (also honoured:
  `--token`, `GITHUB_TOKEN`).

Without one of those the updater fails with a clean "not found" and the game
carries on as normal - nothing breaks, updates simply never appear.

## 5. If a release turns out bad

Delete the release (or un-tag it). Anyone who already updated can run the
previous `Setup.exe` over the same folder - it repairs the install and keeps
saves - or restore by hand from `backup\<old version>\` inside the install.
