param([Parameter(Mandatory)][ValidatePattern('^[a-z0-9-]+$')][string]$Label)
$ErrorActionPreference='Stop'
$projectRoot=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$runRoot=Join-Path $projectRoot 'rexlego/out/native-gpu/session-20260930'
$record=Get-Content -LiteralPath (Join-Path $runRoot "$Label-process.json") | ConvertFrom-Json
$traceRoot=Join-Path $runRoot "trace-$Label"
$logPath=Join-Path $runRoot "$Label-game.log"
$captureRoot=Join-Path $runRoot "gpu-stages-$Label"
New-Item -ItemType Directory -Path $captureRoot -Force | Out-Null
$offset=0L; $pending=''; $lastGrowth=[datetime]::UtcNow; $stalled=$false
$menuStart=$null; $idleComplete=$false
$monitorLog=Join-Path $traceRoot 'monitor.jsonl'
function Write-MonitorEvent([string]$kind,$detail) {
    [ordered]@{utc=[datetime]::UtcNow.ToString('o');event=$kind;detail=$detail} |
        ConvertTo-Json -Compress -Depth 5 | Add-Content -LiteralPath $monitorLog
}
Write-MonitorEvent 'watch_started' @{gamePid=$record.id;log=$logPath}
while ($true) {
    $game=Get-Process -Id $record.id -ErrorAction SilentlyContinue
    if (!$game) { Write-MonitorEvent 'game_exited' @{}; break }
    if ($game.Path -ne $record.path -or
        $game.StartTime.ToUniversalTime().Ticks -ne ([datetime]$record.startedAt).ToUniversalTime().Ticks) {
        Write-MonitorEvent 'identity_changed_stopping_watch' @{}; break
    }
    if (Test-Path -LiteralPath $logPath) {
        $stream=[IO.File]::Open($logPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
        try {
            if ($stream.Length -lt $offset) { $offset=0; $pending='' }
            $length=$stream.Length-$offset
            if ($length -gt 0) {
                $lastGrowth=[datetime]::UtcNow; $stalled=$false
                $stream.Position=$offset
                $bytes=New-Object byte[] ([int][Math]::Min($length,4MB))
                $read=$stream.Read($bytes,0,$bytes.Length); $offset+=$read
                $text=$pending+[Text.Encoding]::UTF8.GetString($bytes,0,$read)
                $lines=$text -split "`n"; $pending=$lines[-1]
                for ($index=0;$index -lt $lines.Length-1;$index++) {
                    $line=$lines[$index].TrimEnd("`r")
                    if ($line -match '\[(error|critical)\]|Native GPU:.*(failed|mismatch|unsupported|unrecognized|exhausted)|Native D3D12') {
                        Write-MonitorEvent 'log_anomaly_candidate' @{line=$line;offset=$offset}
                        Set-Content -LiteralPath (Join-Path $captureRoot 'capture-next-frame') -Value 'log anomaly candidate'
                    }
                }
            }
        } finally { $stream.Dispose() }
    }
    if (!$stalled -and ([datetime]::UtcNow-$lastGrowth).TotalSeconds -gt 30) {
        $stalled=$true
        Write-MonitorEvent 'log_silent_30s_candidate' @{responding=$game.Responding}
        Set-Content -LiteralPath (Join-Path $captureRoot 'capture-next-frame') -Value 'log silent candidate'
    }
    $marker=Join-Path $traceRoot 'menu-start.json'
    if (!$menuStart -and (Test-Path -LiteralPath $marker)) {
        $menuStart=[datetime](Get-Content -LiteralPath $marker | ConvertFrom-Json).utc
        Write-MonitorEvent 'menu_idle_started' @{utc=$menuStart.ToUniversalTime().ToString('o')}
        Set-Content -LiteralPath (Join-Path $captureRoot 'capture-next-frame') -Value 'menu idle baseline'
    }
    $idleSeconds=if ($menuStart) { ([datetime]::UtcNow-$menuStart.ToUniversalTime()).TotalSeconds } else { $null }
    if ($menuStart -and !$idleComplete -and $idleSeconds -ge 600) {
        $idleComplete=$true
        Write-MonitorEvent 'menu_idle_600s_elapsed' @{idleSeconds=$idleSeconds}
        Set-Content -LiteralPath (Join-Path $captureRoot 'capture-next-frame') -Value 'menu idle 600 seconds'
    }
    [ordered]@{utc=[datetime]::UtcNow.ToString('o');gamePid=$game.Id;workingSet=$game.WorkingSet64;
        privateBytes=$game.PrivateMemorySize64;cpuSeconds=$game.CPU;responding=$game.Responding;
        logBytesRead=$offset;secondsSinceLogGrowth=([datetime]::UtcNow-$lastGrowth).TotalSeconds;
        menuIdleSeconds=$idleSeconds;menuIdle600sElapsed=$idleComplete} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $traceRoot 'heartbeat.json')
    Start-Sleep -Seconds 2
}
