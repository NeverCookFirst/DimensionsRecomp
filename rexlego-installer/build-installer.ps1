# Builds the tester release: ONE file, dist\DimensionsRecompiled-Setup.exe, with
# everything a tester needs appended to it (see PayloadSource.cs for the format).
# staging\ holds the loose files on the way there and is not part of the release.
#
#   .\build-installer.ps1                 # uses the release build of the game
#
# The game binaries are taken from rexlego\out\build\win-amd64-release. They
# are NOT rebuilt here - build the game first and make sure the exe/dlls are
# the ones you mean to ship (see the "verify the artifact" note in memory:
# ninja can report success without relinking because of the space in the path).

param(
    [string]$GameBuild = "$PSScriptRoot\..\rexlego\out\build\win-amd64-release"
)

$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."
$dist = "$PSScriptRoot\dist"
# Deliberately NOT under $dist: PayloadSource falls back to a "payload" folder
# next to the exe, and a stale staging copy there would mask a broken append.
$payload = "$PSScriptRoot\staging"

if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
if (Test-Path $payload) { Remove-Item -Recurse -Force $payload }
New-Item -ItemType Directory -Force "$payload\game", "$payload\mods", "$payload\modcli", "$payload\toypad", "$payload\saveconverter" | Out-Null

# 1. The installer itself: one self-contained exe, no .NET needed on the tester's PC.
Write-Host "== Publishing the installer"
# EnableCompressionInSingleFile squeezes the bundled .NET runtime from ~139 MB to
# ~60 MB. It costs a second of startup while the host decompresses it, which an
# installer can afford, and it does not touch the payload appended afterwards.
dotnet publish "$PSScriptRoot\Setup.csproj" -c Release -r win-x64 --self-contained `
    -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true `
    -p:EnableCompressionInSingleFile=true -p:DebugType=none `
    -o $dist -nologo -v q
if ($LASTEXITCODE -ne 0) { throw "installer publish failed" }
Get-ChildItem $dist -Filter *.pdb | Remove-Item

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

# 5. Fold the payload into the exe: zip it, append it, add the footer that
#    PayloadSource looks for. Appending after a single-file apphost is safe -
#    the bundle offset is patched into the host, not searched from the end.
Write-Host "== Folding the payload into the exe"
$exe = "$dist\DimensionsRecompiled-Setup.exe"
$payloadZip = "$PSScriptRoot\payload.zip"
if (Test-Path $payloadZip) { Remove-Item $payloadZip }
# ZipFile rather than Compress-Archive, to avoid its empty-folder and path
# quirks. Both still write entry names with backslashes here (Windows
# PowerShell runs on .NET Framework, which does that against the spec), so
# ZipPayload normalises separators when it reads them back.
Add-Type -AssemblyName System.IO.Compression.FileSystem
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
Write-Host ("== Done: {0} ({1:N0} MB) - this single file is what testers get" -f $exe, ((Get-Item $exe).Length / 1MB))
