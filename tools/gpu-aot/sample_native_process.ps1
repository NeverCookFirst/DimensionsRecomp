param(
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9-]+$')][string]$Label,
    [ValidateRange(1,30)][int]$Seconds = 5
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$runRoot = Join-Path $root 'rexlego/out/native-gpu/session-20260930'
$record = Get-Content -LiteralPath (Join-Path $runRoot "$Label-process.json") | ConvertFrom-Json
$game = Get-Process -Id $record.id
if ($game.Path -ne $record.path -or $game.StartTime.ToUniversalTime().Ticks -ne
    ([datetime]$record.startedAt).ToUniversalTime().Ticks) { throw 'Probe identity mismatch' }
$threadsBefore = @{}
foreach ($thread in $game.Threads) {
    try { $threadsBefore[$thread.Id] = $thread.TotalProcessorTime.TotalSeconds } catch {}
}
$started = Get-Date
$cpuBefore = $game.TotalProcessorTime.TotalSeconds
Start-Sleep -Seconds $Seconds
$game.Refresh()
$elapsed = ((Get-Date) - $started).TotalSeconds
$threads = foreach ($thread in $game.Threads) {
    try {
        if ($threadsBefore.ContainsKey($thread.Id)) {
            [pscustomobject]@{id=$thread.Id;oneCorePercent=
                [math]::Round(100*($thread.TotalProcessorTime.TotalSeconds-$threadsBefore[$thread.Id])/$elapsed,2)}
        }
    } catch {}
}
$memory = Get-CimInstance Win32_PerfFormattedData_PerfOS_Memory |
    Select-Object AvailableMBytes,PagesInputPersec,PageReadsPersec
$gpu = & nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total --format=csv,noheader 2>$null
$result = [ordered]@{
    label=$Label;startedAt=$started.ToString('o');sampleSeconds=$elapsed;pid=$record.id;
    workingSetMiB=[math]::Round($game.WorkingSet64/1MB,1);
    privateMiB=[math]::Round($game.PrivateMemorySize64/1MB,1);
    processCpuInCores=($game.TotalProcessorTime.TotalSeconds-$cpuBefore)/$elapsed;
    hottestThreads=@($threads | Sort-Object oneCorePercent -Descending | Select-Object -First 6);
    systemMemory=$memory;globalGpuNvidiaSmi=$gpu;
    note='GPU utilization and paging are system-wide; scene must be identified separately.'
}
$path=Join-Path $runRoot "$Label-resources-$($started.ToString('HHmmss')).json"
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $path
$result | ConvertTo-Json -Depth 5
