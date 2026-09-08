# Builds the "All Worlds" mod: every playable character gets every From<IP>
# hub-access tag, so placing any figure unlocks every adventure world.
#
# Mechanism (from the PS4 Lord Vortech mod): a world opens when a figure with
# the tag ability From<IP> is placed. Only the character's .txt needs the
# lines; the charcache copies are not required (verified in game 2026-09-08).
#
# A character's .txt must not grow inside the archive, so comments and blank
# lines are stripped and the file is space-padded back to its original size.
#
#   .\make-allworlds-mod.ps1 [-Archive <PATCH.DAT>] [-Out <mods\AllWorlds>]
param(
    # Default: the update's PATCH.DAT plus every DLC package archive.
    [string[]]$Archives = @(),
    [string]$Out = "$PSScriptRoot\..\DimensionsModManager\mods\AllWorlds",
    [string]$ModCli = "$PSScriptRoot\..\DimensionsModManager-CLI\bin\Release\net8.0\modcli.exe"
)
$ErrorActionPreference = 'Stop'
$Worlds = @("DC","LEGOMovie","LOTR","WizardOfOz","BTTF","Portal","DrWho","Ninjago",
            "Simpsons","ScoobyDoo","Chima","Jurassic","Ghostbusters","Retro","TeenTitans",
            "Ghostbusters2016",
            # Year 2 (tags defined in the DLC packs; exact spelling from the records)
            "AdventureTime","ET","FantasticBeasts","Gremlins","Sonic","KnightRider",
            "LegoBatman","Goonies","HarryPotter","LegoCity","BeetleJuice","PowerPuff",
            "ATeam","MissionImpossible")
$tmp = Join-Path $env:TEMP "allworlds-src"; New-Item -ItemType Directory -Force $tmp | Out-Null
if (Test-Path "$Out\datfiles") { Remove-Item -Recurse -Force "$Out\datfiles" }

if (-not $Archives) {
    $Archives = @("$PSScriptRoot\..\rexlego\tu23\PATCH.DAT") +
        (Get-ChildItem "$PSScriptRoot\..\rexlego\content\0000000000000000\5752084B\00000002" `
            -Recurse -Filter "DLC*.DAT2" | ForEach-Object { $_.FullName })
}
$done = @(); $skipped = @()
foreach ($Archive in $Archives) {
$archiveName = [IO.Path]::GetFileNameWithoutExtension($Archive).ToUpperInvariant()
$list = & $ModCli names $Archive 'chars\minifig\' | Where-Object { $_ -match '\.txt\s*$' }
foreach ($line in $list) {
    $parts = $line.Trim() -split '\s+', 5
    $path = $parts[-1].Trim()
    # Index -1 = a name-tree entry the CRC table cannot place; not extractable.
    if ([int]$parts[0] -lt 0) { $skipped += "$path (no entry)"; continue }
    $src = Join-Path $tmp ([IO.Path]::GetFileName($path))
    if (Test-Path $src) { Remove-Item $src }
    & $ModCli extract $Archive $path $src | Out-Null
    if (-not (Test-Path $src)) { $skipped += "$path (extract failed)"; continue }
    $orig = [IO.File]::ReadAllText($src)
    # Only playable figures carry a From tag; NPC/cutscene variants are left alone.
    if ($orig -notmatch 'Ability "From') { continue }
    $origLen = [Text.Encoding]::ASCII.GetByteCount($orig)
    $nl = if ($orig.Contains("`r`n")) { "`r`n" } else { "`n" }
    $keep = ($orig -split "`r?`n") |
        Where-Object { $_ -notmatch '^\s*//' -and $_.Trim() -ne '' } |
        ForEach-Object { ($_ -replace '\s*//.*$', '').Trim() -replace '[ \t]{2,}', ' ' }
    $anchor = $keep | Where-Object { $_ -match 'Ability "From' } | Select-Object -First 1
    $idx = [array]::IndexOf($keep, $anchor)
    $add = $Worlds | Where-Object { -not ($keep -match ('^\s*Ability "From' + $_ + '"')) } |
           ForEach-Object { 'Ability "From' + $_ + '"' }
    $new = @($keep[0..$idx]) + $add
    if ($idx + 1 -le $keep.Count - 1) { $new += $keep[($idx + 1)..($keep.Count - 1)] }
    $text = ($new -join $nl) + $nl
    $len = [Text.Encoding]::ASCII.GetByteCount($text)
    if ($len -gt $origLen -and $nl -eq "`r`n") {
        # Last resort: LF line endings (the game's own files use both styles).
        $nl = "`n"; $text = ($new -join $nl) + $nl
        $len = [Text.Encoding]::ASCII.GetByteCount($text)
    }
    if ($len -gt $origLen) { $skipped += "$path ($len > $origLen)"; continue }
    $text = $text.PadRight($origLen)
    $dst = Join-Path "$Out\datfiles\$archiveName" $path
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
    [IO.File]::WriteAllBytes($dst, [Text.Encoding]::ASCII.GetBytes($text))
    $done += "$archiveName\$path"
}
}
@"
{
  "name": "All Worlds",
  "author": "NeverCookFirst",
  "version": "1.0",
  "platform": "x360",
  "description": "Every character opens every adventure world. Each playable figure gets the hub-access tags Lord Vortech carries (FromDC, FromLOTR, FromPortal ... $($Worlds.Count) worlds), so placing any figure unlocks all of them. Only the character configs change; nothing else is touched. Covers $($done.Count) characters across the update and every DLC pack."
}
"@ | Set-Content "$Out\mod.json" -Encoding ASCII
"patched: $($done.Count)"; if ($skipped) { "SKIPPED (too long):"; $skipped }
