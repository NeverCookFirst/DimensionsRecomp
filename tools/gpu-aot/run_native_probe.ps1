param(
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9-]+$')][string]$Label,
    [ValidateSet('normal','timing','snapshot')][string]$Mode = 'normal',
    [switch]$AllTextures,
    [switch]$CaptureMissing,
    [switch]$LongProbe,
    [switch]$PortraitTrace,
    [switch]$NoMemoryWatch,
    [switch]$StaticTextureWatch,
    [switch]$AuditTextureWatch,
    [switch]$BufferWatch,
    [switch]$AuditBufferWatch,
    [switch]$NoDrawArena,
    [switch]$AsyncCpuResources,
    [switch]$Stencil,
    [switch]$MeshTrace,
    [switch]$LogoUploads,
    [switch]$Viewport,
    [switch]$BufferWindows,
    [switch]$DepthAlias,
    [switch]$DisableDepthAlias,
    [switch]$Pm4Reference,
    [switch]$ConfiguredRenderer,
    [string]$LogoCaptureRenderDocDll,
    [ValidateRange(1,8)][int]$Monitor = 3,
    [switch]$Close
)
$ErrorActionPreference = 'Stop'
if ($PortraitTrace -and !$LongProbe) { throw 'PortraitTrace requires LongProbe for correlated draw/constants capture' }
if ($ConfiguredRenderer -and $Pm4Reference) { throw 'Select configured renderer or explicit PM4 reference, not both' }
if ($DepthAlias -and $DisableDepthAlias) { throw 'DepthAlias and DisableDepthAlias are mutually exclusive' }
if (($Pm4Reference -or $ConfiguredRenderer) -and ($Stencil -or $MeshTrace -or $LogoUploads -or $Viewport -or $BufferWindows -or $BufferWatch -or $AuditBufferWatch -or $DepthAlias -or $DisableDepthAlias -or $LongProbe -or $LogoCaptureRenderDocDll)) {
    throw 'PM4 reference mode uses original XDK/SDK objects; native-only diagnostic/candidate switches must be disabled'
}
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$runRoot = Join-Path $projectRoot 'rexlego/out/native-gpu/session-20260930'
$exePath = Join-Path $projectRoot 'rexlego/out/build/win-amd64-release/legodimensions.exe'
$recordPath = Join-Path $runRoot "$Label-process.json"
if ($Close) {
    $record = Get-Content -LiteralPath $recordPath | ConvertFrom-Json
    $game = Get-Process -Id $record.id -ErrorAction SilentlyContinue
    if (!$game) { Write-Output 'Probe already closed'; return }
    if ($game.Path -ne $record.path -or
        $game.StartTime.ToUniversalTime().Ticks -ne ([datetime]$record.startedAt).ToUniversalTime().Ticks) {
        throw 'Process identity mismatch; refusing to close another process'
    }
    if (!$game.CloseMainWindow()) { throw 'Probe has no closeable main window' }
    Write-Output "Close requested for probe PID $($record.id)"
    return
}
if (Get-Process legodimensions -ErrorAction SilentlyContinue) { throw 'Game already running' }
if (Test-Path -LiteralPath $recordPath) { throw 'Use a fresh label to preserve earlier evidence' }
$keys = @('SDL_WINDOW_ACTIVATE_WHEN_SHOWN','LEGO_NATIVE_TIMING','LEGO_NATIVE_ALL_TEXTURES',
    'LEGO_GPU_SNAPSHOT_DIR','LEGO_GPU_SNAPSHOT_START_AT_TILING',
    'LEGO_GPU_SNAPSHOT_TIME_WINDOWS','LEGO_DUMP_MISSING_SHADERS',
    'LEGO_NATIVE_GPU_DEBUG','LEGO_XENOS_TILING_TRACE','LEGO_DUMP_TEXTURE_UPLOADS','LEGO_NATIVE_TRACE_DIR',
    'LEGO_NATIVE_NO_MEMORY_WATCH','LEGO_NATIVE_STATIC_TEXTURE_WATCH','LEGO_NATIVE_AUDIT_TEXTURE_WATCH','LEGO_NATIVE_BUFFER_WATCH','LEGO_NATIVE_AUDIT_BUFFER_WATCH','LEGO_NATIVE_NO_DRAW_ARENA',
    'LEGO_NATIVE_FRAME_METRICS','LEGO_NATIVE_ASYNC_CPU_RESOURCES','LEGO_NATIVE_STENCIL','LEGO_NATIVE_MESH_TRACE',
    'LEGO_NATIVE_VIEWPORT','LEGO_DUMP_TEXTURE_UPLOADS_LOGOS_ONLY','LEGO_NATIVE_BUFFER_WINDOWS','LEGO_NATIVE_PM4_REFERENCE',
    'LEGO_NATIVE_RENDERDOC_DLL','LEGO_NATIVE_RENDERDOC_CAPTURE','LEGO_NATIVE_DEPTH_ALIAS','LEGO_NATIVE_PORTRAIT_TRACE')
$saved = @{}
foreach ($key in $keys) {
    $saved[$key] = [Environment]::GetEnvironmentVariable($key,'Process')
    Remove-Item -LiteralPath "Env:\$key" -ErrorAction SilentlyContinue
}
try {
    $env:SDL_WINDOW_ACTIVATE_WHEN_SHOWN = '0'
    if ($Mode -eq 'timing') { $env:LEGO_NATIVE_TIMING = '1' }
    if ($NoMemoryWatch) { $env:LEGO_NATIVE_NO_MEMORY_WATCH = '1' }
    if (!$ConfiguredRenderer) {
        $env:LEGO_NATIVE_STATIC_TEXTURE_WATCH = $(if ($StaticTextureWatch -or $AuditTextureWatch) {'1'} else {'0'})
        $env:LEGO_NATIVE_BUFFER_WINDOWS = $(if ($BufferWindows) {'1'} else {'0'})
        $env:LEGO_NATIVE_BUFFER_WATCH = $(if ($BufferWatch -or $AuditBufferWatch) {'1'} else {'0'})
    }
    if ($StaticTextureWatch) { $env:LEGO_NATIVE_STATIC_TEXTURE_WATCH = '1' }
    if ($AuditTextureWatch) {
        $env:LEGO_NATIVE_STATIC_TEXTURE_WATCH = '1'
        $env:LEGO_NATIVE_AUDIT_TEXTURE_WATCH = '1'
    }
    if ($AuditBufferWatch) { $env:LEGO_NATIVE_AUDIT_BUFFER_WATCH = '1' }
    if ($NoDrawArena) { $env:LEGO_NATIVE_NO_DRAW_ARENA = '1' }
    if ($AsyncCpuResources) { $env:LEGO_NATIVE_ASYNC_CPU_RESOURCES = '1' }
    if ($Stencil) { $env:LEGO_NATIVE_STENCIL = '1' }
    if ($MeshTrace) { $env:LEGO_NATIVE_MESH_TRACE = '1' }
    if ($Viewport) { $env:LEGO_NATIVE_VIEWPORT = '1' }
    if ($BufferWindows) { $env:LEGO_NATIVE_BUFFER_WINDOWS = '1' }
    if ($DepthAlias) { $env:LEGO_NATIVE_DEPTH_ALIAS = '1' }
    if ($DisableDepthAlias) { $env:LEGO_NATIVE_DEPTH_ALIAS = '0' }
    if (!$ConfiguredRenderer) { $env:LEGO_NATIVE_PM4_REFERENCE = $(if ($Pm4Reference) {'1'} else {'0'}) }
    if ($LogoCaptureRenderDocDll) {
        $env:LEGO_NATIVE_RENDERDOC_DLL = (Resolve-Path -LiteralPath $LogoCaptureRenderDocDll).Path
        $captureDir = Join-Path $runRoot "renderdoc-$Label"
        New-Item -ItemType Directory -Path $captureDir | Out-Null
        $env:LEGO_NATIVE_RENDERDOC_CAPTURE = Join-Path $captureDir 'tt-logo'
    }
    if ($LogoUploads) {
        $env:LEGO_DUMP_TEXTURE_UPLOADS = Join-Path $runRoot "logo-uploads-$Label"
        $env:LEGO_DUMP_TEXTURE_UPLOADS_LOGOS_ONLY = '1'
    }
    if ($Mode -eq 'timing') { $env:LEGO_NATIVE_FRAME_METRICS = Join-Path $runRoot "$Label-frames.csv" }
    if ($LongProbe) {
        $env:LEGO_NATIVE_TIMING = '1'
        $env:LEGO_NATIVE_TRACE_DIR = Join-Path $runRoot "trace-$Label"
        New-Item -ItemType Directory -Path $env:LEGO_NATIVE_TRACE_DIR -Force | Out-Null
        $env:LEGO_GPU_SNAPSHOT_DIR = Join-Path $runRoot "gpu-stages-$Label"
        $env:LEGO_GPU_SNAPSHOT_START_AT_TILING = '1'
    }
    if ($AllTextures) { $env:LEGO_NATIVE_ALL_TEXTURES = '1' }
    if ($PortraitTrace) { $env:LEGO_NATIVE_PORTRAIT_TRACE = '1' }
    if ($CaptureMissing) {
        $env:LEGO_DUMP_MISSING_SHADERS = Join-Path $runRoot "missing-$Label"
    }
    if ($Mode -eq 'snapshot') {
        $env:LEGO_GPU_SNAPSHOT_DIR = Join-Path $runRoot "gpu-stages-$Label"
        $env:LEGO_GPU_SNAPSHOT_START_AT_TILING = '1'
        $env:LEGO_GPU_SNAPSHOT_TIME_WINDOWS = '1'
    }
    $logPath = Join-Path $runRoot "$Label-game.log"
    $configSnapshot = Join-Path $runRoot "$Label-config.toml"
    Copy-Item -LiteralPath (Join-Path (Split-Path $exePath) 'legodimensions.toml') -Destination $configSnapshot
    $nativeEnvironment = [ordered]@{}
    foreach ($key in $keys) {
        $value = [Environment]::GetEnvironmentVariable($key, 'Process')
        if ($null -ne $value) { $nativeEnvironment[$key] = $value }
    }
    $game = Start-Process -FilePath $exePath -WorkingDirectory (Split-Path $exePath) `
        -ArgumentList ("--monitor=$Monitor"),("--log_file=`"$logPath`"") -WindowStyle Normal `
        -RedirectStandardOutput (Join-Path $runRoot "$Label-stdout.log") `
        -RedirectStandardError (Join-Path $runRoot "$Label-stderr.log") -PassThru
    # Process.Path can briefly be empty immediately after Start-Process.
    # The executable was launched by this exact resolved path; later checks
    # still require the live process's actual path and start time to match.
    $record = [ordered]@{id=$game.Id;path=$exePath;startedAt=$game.StartTime.ToString('o');
        mode=$Mode;allTextures=[bool]$AllTextures;captureMissing=[bool]$CaptureMissing;monitor=$Monitor;
        longProbe=[bool]$LongProbe;noMemoryWatch=[bool]$NoMemoryWatch;staticTextureWatch=[bool]$StaticTextureWatch;
        portraitTrace=[bool]$PortraitTrace;
        auditTextureWatch=[bool]$AuditTextureWatch;
        bufferWatch=[bool]$BufferWatch; auditBufferWatch=[bool]$AuditBufferWatch;
        noDrawArena=[bool]$NoDrawArena;asyncCpuResources=[bool]$AsyncCpuResources;
        stencil=[bool]$Stencil;meshTrace=[bool]$MeshTrace;
        logoUploads=[bool]$LogoUploads;viewport=[bool]$Viewport;bufferWindows=[bool]$BufferWindows;
        nativeDepthAliasEnabled=![bool]$DisableDepthAlias;
        pm4Reference=[bool]$Pm4Reference;
        configuredRenderer=[bool]$ConfiguredRenderer;
        configSnapshot=$configSnapshot;environmentOverrides=$nativeEnvironment;
        logoCaptureRenderDocDll=$LogoCaptureRenderDocDll;
        exeSha256=(Get-FileHash -LiteralPath $exePath).Hash;
        configSha256=(Get-FileHash -LiteralPath (Join-Path (Split-Path $exePath) 'legodimensions.toml')).Hash}
    $record | ConvertTo-Json | Set-Content -LiteralPath $recordPath
    if ($LongProbe) {
        $probeShell=(Get-Process -Id $PID).Path
        $watchScript=Join-Path $PSScriptRoot 'watch_native_probe.ps1'
        $watcher=Start-Process -FilePath $probeShell -ArgumentList '-NoProfile','-File',("`"$watchScript`""),'-Label',$Label `
            -WindowStyle Hidden -RedirectStandardOutput (Join-Path $runRoot "$Label-watch-stdout.log") `
            -RedirectStandardError (Join-Path $runRoot "$Label-watch-stderr.log") -PassThru
        [ordered]@{id=$watcher.Id;path=$watcher.Path;startedAt=$watcher.StartTime.ToString('o');gamePid=$game.Id} |
            ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runRoot "$Label-watch-process.json")
    }
    $record
} finally {
    foreach ($key in $keys) {
        if ($null -eq $saved[$key]) { Remove-Item -LiteralPath "Env:\$key" -ErrorAction SilentlyContinue }
        else { Set-Item -LiteralPath "Env:\$key" -Value $saved[$key] }
    }
}
