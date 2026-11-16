param(
  [Parameter(Mandatory=$true)][string]$OutputDirectory,
  [Parameter(Mandatory=$true)][string]$PlumeLibrary,
  [Parameter(Mandatory=$true)][string]$Dxc
)
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$taskOutput=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force $taskOutput | Out-Null
$taskShader=Join-Path $taskRoot 'rexlego/src/gpu_native/shaders/resolve_scaled_ps.hlsl'
$taskSources=Join-Path $PSScriptRoot 'test_scaled_resolve_gpu.cpp'
foreach($taskStage in @('vs','ps')) {
  $taskInput=if($taskStage -eq 'vs'){Join-Path $taskRoot 'rexlego/src/gpu_native/shaders/copy_vs.hlsl'}else{$taskShader}
  & $Dxc $taskInput -T "${taskStage}_6_0" -E main -Fo (Join-Path $taskOutput "$taskStage.dxil")
  if($LASTEXITCODE -ne 0){throw "Compile $taskStage failed"}
}
$taskIncludes=@('rexlego/src','thirdparty/plume','thirdparty/plume/contrib/D3D12MemoryAllocator/include') | ForEach-Object {"/I$(Join-Path $taskRoot $_)"}
$taskExecutable=Join-Path $taskOutput 'test-scaled.exe'
& clang-cl $taskSources /std:c++latest /O2 /MD /EHsc /UNDEBUG @taskIncludes "/Fo$taskOutput/" "/Fe$taskExecutable" /link $PlumeLibrary d3d12.lib dxgi.lib dxguid.lib user32.lib *> (Join-Path $taskOutput 'compile.log')
if($LASTEXITCODE -ne 0){throw 'GPU fixture compilation failed'}
$taskOldCache=$env:PLUME_D3D12_STATE_CACHE
try {
  $taskResults=foreach($taskCache in @('0','1')) {
    $env:PLUME_D3D12_STATE_CACHE=$taskCache
    $ErrorActionPreference='Continue'
    $taskLog=& $taskExecutable (Join-Path $taskOutput 'vs.dxil') (Join-Path $taskOutput 'ps.dxil') 2>&1
    $taskCode=$LASTEXITCODE
    $ErrorActionPreference='Stop'
    $taskLog | Set-Content (Join-Path $taskOutput "cache-$taskCache.log")
    if($taskCode -ne 0){throw "Resampling GPU test failed, cache=$taskCache"}
    @{state_cache=$taskCache;exit_code=$taskCode;log=($taskLog -join "`n")}
  }
} finally {$env:PLUME_D3D12_STATE_CACHE=$taskOldCache}
# The old uniform-axis assumption must fail the exact Balanced dimensions.
$taskMutant=(Get-Content -Raw $taskShader).Replace('position.xy * g_ResolveRatio','position.xy * g_ResolveRatio.xx')
$taskMutant | Set-Content (Join-Path $taskOutput 'single-axis.hlsl')
& $Dxc (Join-Path $taskOutput 'single-axis.hlsl') -I (Split-Path $taskShader) -T ps_6_0 -E main -Fo (Join-Path $taskOutput 'single-axis.dxil')
if($LASTEXITCODE -ne 0){throw 'Mutant compilation failed'}
$ErrorActionPreference='Continue'
$taskMutantLog=& $taskExecutable (Join-Path $taskOutput 'vs.dxil') (Join-Path $taskOutput 'single-axis.dxil') 2>&1
$taskMutantCode=$LASTEXITCODE
$ErrorActionPreference='Stop'
$taskMutantLog | Set-Content (Join-Path $taskOutput 'single-axis.log')
if($taskMutantCode -ne 1){throw "Single-axis regression was not detected: $taskMutantCode"}
@{game_launched=$false;production_shader=$taskShader;production_mapping='scaled_resolve.h';
  results=@($taskResults);single_axis_mutant_exit=$taskMutantCode;comparisons=20;
  shader_sha256=(Get-FileHash $taskShader).Hash;fixture_sha256=(Get-FileHash $taskSources).Hash} |
  ConvertTo-Json -Depth 6 | Set-Content (Join-Path $taskOutput 'verification.json')
Write-Output 'PASS: 20 real GPU resampling cases through 4K; old single-axis assumption rejected'
