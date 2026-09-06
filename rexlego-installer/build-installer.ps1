# Builds a release: the one-file installer, and the small update pack that goes
# next to it.
#
#   dist\DimensionsRecompiled-Setup.exe   everything, for a fresh install
#   dist\update-<version>.rxp             only the files that changed, for the updater
#   releases\<version>.json               the manifest, committed, so the NEXT build
#                                         knows what changed
#
#   .\build-installer.ps1                          # version from ..\VERSION
#   .\build-installer.ps1 -Version 1.0.1           # override it
#   .\build-installer.ps1 -Previous releases\1.0.0.json
#
# The game binaries are taken from rexlego\out\build\win-amd64-release. They are
# NOT rebuilt here - build the game first and make sure the exe/dlls are the ones
# you mean to ship (see the "verify the artifact" note in memory: ninja can
# report success without relinking because of the space in the path).

param(
    [string]$GameBuild = "$PSScriptRoot\..\rexlego\out\build\win-amd64-release",
    [string]$Version = "",
    [string]$Previous = "",
    [string]$Notes = "",
    # Keys this release adds to legodimensions.toml. Needed whenever the plan in
    # TomlConfig.cs grows a key: the updater that applies this pack is still the
    # PREVIOUS release's, so it knows nothing about the new key unless the
    # manifest carries it. -TomlForced for compatibility switches that must be
    # rewritten even if the user changed them, -TomlDefaults for the rest.
    [hashtable]$TomlDefaults = @{},
    [hashtable]$TomlForced = @{},
    [string[]]$TomlRemoved = @()
)

$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."
$dist = "$PSScriptRoot\dist"
$releases = "$PSScriptRoot\releases"
# Deliberately NOT under $dist: PayloadSource falls back to a "payload" folder
# next to the exe, and a stale staging copy there would mask a broken append.
$payload = "$PSScriptRoot\staging"
$packStage = "$PSScriptRoot\packstage"

if ($Version -eq "") {
    $versionFile = "$root\VERSION"
    if (-not (Test-Path $versionFile)) { throw "no -Version and no $versionFile" }
    $Version = (Get-Content $versionFile -Raw).Trim()
}
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "version must look like 1.2.3, got '$Version'" }

# The previous release's manifest is what makes the update pack small. Without
# one every file goes in, which is correct but pointless - that is Setup.exe.
if ($Previous -eq "" -and (Test-Path $releases)) {
    $candidate = Get-ChildItem $releases -Filter *.json |
        Where-Object { $_.BaseName -ne $Version } |
        Sort-Object { [version]$_.BaseName } -Descending |
        Select-Object -First 1
    if ($candidate) { $Previous = $candidate.FullName }
}

Write-Host "== Dimensions Recompiled $Version"
if ($Previous -ne "") { Write-Host "   diffing against $(Split-Path $Previous -Leaf)" }
else { Write-Host "   no previous release recorded: installer only, no update pack" }

foreach ($path in @($dist, $payload, $packStage)) {
    if (Test-Path $path) { Remove-Item -Recurse -Force $path }
}
New-Item -ItemType Directory -Force "$payload\game", "$payload\mods", "$payload\modcli", `
    "$payload\toypad", "$payload\saveconverter", $releases | Out-Null

# 1. The installer itself: one self-contained exe, no .NET needed on the tester's PC.
Write-Host "== Publishing the installer"
# EnableCompressionInSingleFile squeezes the bundled .NET runtime from ~139 MB to
# ~60 MB. It costs a second of startup while the host decompresses it, which an
# installer can afford, and it does not touch the payload appended afterwards.
dotnet publish "$PSScriptRoot\Setup.csproj" -c Release -r win-x64 --self-contained `
    -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true `
    -p:EnableCompressionInSingleFile=true -p:DebugType=none -p:Version=$Version `
    -o $dist -nologo -v q
if ($LASTEXITCODE -ne 0) { throw "installer publish failed" }
Get-ChildItem $dist -Filter *.pdb | Remove-Item

$exe = "$dist\DimensionsRecompiled-Setup.exe"
# This binary, before anything is appended to it, IS the updater: the installer
# copies itself minus the payload into tools\rexupdate. So its hash is taken now,
# while the file still looks the way it will on the user's disk.
$updaterHost = "$PSScriptRoot\rexupdate-host.exe"
Copy-Item $exe $updaterHost -Force

# 2. The game. Verify each file exists rather than copying whatever is there.
Write-Host "== Game binaries from $GameBuild"
$gameFiles = "legodimensions.exe", "rexruntime.dll", "rexgpu-xenos.dll", "FiraSans-Regular.ttf", "achievement_unlocked.wav"
foreach ($f in $gameFiles) {
    $src = Join-Path $GameBuild $f
    if (-not (Test-Path $src)) { throw "missing game file: $src" }
    Copy-Item $src "$payload\game\$f"
    Write-Host ("   {0,-28} {1,12:N0} bytes  {2}" -f $f, (Get-Item $src).Length, (Get-Item $src).LastWriteTime)
}
# SDL gamepad mappings (community gamecontrollerdb, kept in rexlego\res). Optional.
if (Test-Path "$root\rexlego\res\gamecontrollerdb.txt") {
    Copy-Item "$root\rexlego\res\gamecontrollerdb.txt" "$payload\game\"
} else {
    Write-Warning "rexlego\res\gamecontrollerdb.txt not found - shipping without gamepad mappings"
}

# 3. Mods + modcli (self-contained so the F8 menu works without a .NET runtime).
Write-Host "== Mods"
# The mods folder is shared with the RPCS3 build; ship only what this build can
# apply (mod.json platform "any" or "x360"), so testers do not see PS3-only mods.
foreach ($mod in Get-ChildItem "$root\DimensionsModManager\mods" -Directory) {
    $json = Join-Path $mod.FullName "mod.json"
    if (-not (Test-Path $json)) { continue }
    $platform = (Get-Content $json -Raw | ConvertFrom-Json).platform
    if ($platform -eq "any" -or $platform -eq "x360") {
        Copy-Item -Recurse $mod.FullName "$payload\mods\$($mod.Name)"
        Write-Host "   $($mod.Name)  [$platform]"
    }
}
dotnet publish "$root\DimensionsModManager-CLI\ModCli.csproj" -c Release -r win-x64 --self-contained `
    -p:PublishSingleFile=true -p:DebugType=none -o "$payload\modcli" -nologo -v q
if ($LASTEXITCODE -ne 0) { throw "modcli publish failed" }
if (-not (Test-Path "$payload\modcli\modcli.exe")) { throw "modcli.exe missing from payload" }

# 4. Toypad app (latest release exe kept in the project root) and the save converter.
Write-Host "== Tools"
$toypad = Get-ChildItem "$root\LegoToypad_*.exe" | Sort-Object Name -Descending | Select-Object -First 1
if (-not $toypad) { throw "no LegoToypad_*.exe in $root" }
Copy-Item $toypad.FullName "$payload\toypad\LegoToypad.exe"
Copy-Item "$root\DimensionsSaveConverter\DimensionsSaveConverter.exe" "$payload\saveconverter\"
Copy-Item "$root\DimensionsSaveConverter\READ ME FIRST.txt" "$payload\saveconverter\"

# 5. The manifest. Every file in the release with its SHA-256, which is what the
#    updater compares against the user's disk to decide what to replace.
Write-Host "== Hashing"
$files = @()
foreach ($file in Get-ChildItem $payload -Recurse -File) {
    $relative = $file.FullName.Substring($payload.Length + 1).Replace('\', '/')
    $files += [ordered]@{
        p   = $relative
        sha = (Get-FileHash $file.FullName -Algorithm SHA256).Hash.ToLower()
        len = $file.Length
    }
}
# Listed like any other file, but never packed into Setup.exe's payload: the
# installer makes it by stripping the payload back off itself.
$files += [ordered]@{
    p   = "updater/rexupdate.exe"
    sha = (Get-FileHash $updaterHost -Algorithm SHA256).Hash.ToLower()
    len = (Get-Item $updaterHost).Length
}
$totalBytes = 0
foreach ($entry in $files) { $totalBytes += $entry.len }
Write-Host ("   {0} files, {1:N1} MB" -f $files.Count, ($totalBytes / 1MB))

function New-Manifest([bool]$full) {
    return [ordered]@{
        product     = "DimensionsRecompiled"
        version     = $Version
        releasedUtc = (Get-Date).ToUniversalTime().ToString("o")
        notes       = $Notes
        minVersion  = ""
        full        = $full
        files       = $files
        removed     = @()
        toml        = [ordered]@{
            forced   = $TomlForced
            defaults = $TomlDefaults
            removed  = $TomlRemoved
        }
    }
}
(New-Manifest $true) | ConvertTo-Json -Depth 6 | Set-Content "$payload\manifest.json" -Encoding UTF8

# 6. Fold the payload into the exe: zip it, append it, add the footer that
#    PayloadSource looks for. Appending after a single-file apphost is safe -
#    the bundle offset is patched into the host, not searched from the end.
Write-Host "== Folding the payload into the exe"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$payloadZip = "$PSScriptRoot\payload.zip"
if (Test-Path $payloadZip) { Remove-Item $payloadZip }
# ZipFile rather than Compress-Archive, to avoid its empty-folder and path
# quirks. Both still write entry names with backslashes here (Windows
# PowerShell runs on .NET Framework, which does that against the spec), so
# ZipPayload normalises separators when it reads them back.
[IO.Compression.ZipFile]::CreateFromDirectory($payload, $payloadZip, [IO.Compression.CompressionLevel]::Optimal, $false)

$hostSize = (Get-Item $exe).Length
$zipSize = (Get-Item $payloadZip).Length
$out = [System.IO.File]::Open($exe, 'Append', 'Write')
try {
    $in = [System.IO.File]::OpenRead($payloadZip)
    try { $in.CopyTo($out, 1MB) } finally { $in.Dispose() }
    $out.Write([System.Text.Encoding]::ASCII.GetBytes("RXPAYLD1"), 0, 8)
    $out.Write([System.BitConverter]::GetBytes([long]$zipSize), 0, 8)
} finally { $out.Dispose() }
Remove-Item $payloadZip
Write-Host ("   host {0:N0} + payload {1:N0} = {2:N0} bytes" -f $hostSize, $zipSize, (Get-Item $exe).Length)

# 7. The update pack: the same layout, carrying only what changed since the
#    previous release. Its manifest still lists every file, so the updater can
#    tell "nothing else to do" from "this pack cannot finish the job".
Write-Host "== Update pack"
$previousShas = @{}
$packed = 0
if ($Previous -eq "" -or -not (Test-Path $Previous)) {
    # Nothing to diff against, so a pack would just be Setup.exe's payload under
    # another name. The first release ships the installer alone.
    Write-Host "   skipped: no previous release to diff against"
} else {
    foreach ($entry in (Get-Content $Previous -Raw | ConvertFrom-Json).files) {
        $previousShas[$entry.p] = $entry.sha
    }
}
New-Item -ItemType Directory -Force $packStage | Out-Null
(New-Manifest $false) | ConvertTo-Json -Depth 6 | Set-Content "$packStage\manifest.json" -Encoding UTF8

if ($previousShas.Count -gt 0) {
    foreach ($entry in $files) {
        if ($previousShas.ContainsKey($entry.p) -and $previousShas[$entry.p] -eq $entry.sha) { continue }
        if ($entry.p -eq "updater/rexupdate.exe") { $source = $updaterHost }
        else { $source = Join-Path $payload $entry.p.Replace('/', '\') }
        $target = Join-Path $packStage $entry.p.Replace('/', '\')
        New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
        Copy-Item $source $target
        $packed++
        Write-Host ("   + {0}" -f $entry.p)
    }
}

$pack = "$dist\update-$Version.rxp"
if ($packed -eq 0) {
    if ($previousShas.Count -gt 0) { Write-Warning "nothing changed since the previous release - no pack written" }
} else {
    [IO.Compression.ZipFile]::CreateFromDirectory($packStage, $pack, [IO.Compression.CompressionLevel]::Optimal, $false)
    Write-Host ("   {0} file(s) -> {1} ({2:N1} MB)" -f $packed, (Split-Path $pack -Leaf), ((Get-Item $pack).Length / 1MB))
}
Remove-Item -Recurse -Force $packStage
Remove-Item $updaterHost

# 8. Record this release so the next build can diff against it. Commit it.
(New-Manifest $true) | ConvertTo-Json -Depth 6 | Set-Content "$releases\$Version.json" -Encoding UTF8

Write-Host ""
Write-Host ("== Done: {0} ({1:N0} MB)" -f (Split-Path $exe -Leaf), ((Get-Item $exe).Length / 1MB))
if ($packed -gt 0) { Write-Host ("           {0} ({1:N0} MB)" -f (Split-Path $pack -Leaf), ((Get-Item $pack).Length / 1MB)) }
if ($packed -gt 0) { Write-Host "   Attach BOTH to the GitHub release, and commit releases\$Version.json." }
else { Write-Host "   Attach the installer to the GitHub release, and commit releases\$Version.json." }
