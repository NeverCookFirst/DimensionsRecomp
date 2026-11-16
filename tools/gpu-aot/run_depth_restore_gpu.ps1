param(
  [Parameter(Mandatory=$true)][string]$OutputDirectory,
  [Parameter(Mandatory=$true)][string]$PlumeLibrary,
  [Parameter(Mandatory=$true)][string]$Dxc
)
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$taskOutput=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force $taskOutput | Out-Null
$taskShader=Join-Path $taskRoot 'rexlego/src/gpu_native/shaders/depth_restore_ps.hlsl'
foreach($taskStage in @('vs','ps')) {
  $taskInput=if($taskStage -eq 'vs'){Join-Path $taskRoot 'rexlego/src/gpu_native/shaders/copy_vs.hlsl'}else{$taskShader}
  & $Dxc $taskInput -T "${taskStage}_6_0" -E main -Fo (Join-Path $taskOutput "$taskStage.dxil")
  if($LASTEXITCODE -ne 0){throw "Compile $taskStage failed"}
}
$taskSources=Join-Path $PSScriptRoot 'test_depth_restore_gpu.cpp'
$taskIncludes=@('rexlego/src','thirdparty/plume','thirdparty/plume/contrib/D3D12MemoryAllocator/include') | ForEach-Object {"/I$(Join-Path $taskRoot $_)"}
$taskExecutable=Join-Path $taskOutput 'test-depth.exe'
& clang-cl $taskSources /std:c++latest /O2 /MD /EHsc /UNDEBUG @taskIncludes "/Fo$taskOutput\" "/Fe$taskExecutable" /link $PlumeLibrary d3d12.lib dxgi.lib dxguid.lib user32.lib *> (Join-Path $taskOutput 'compile.log')
if($LASTEXITCODE -ne 0){throw 'GPU fixture compilation failed'}
$taskOldCache=$env:PLUME_D3D12_STATE_CACHE
try {
  $taskResults=foreach($taskCache in @('0','1')) {
    $env:PLUME_D3D12_STATE_CACHE=$taskCache
    $taskLog=& $taskExecutable (Join-Path $taskOutput 'vs.dxil') (Join-Path $taskOutput 'ps.dxil') 2>&1
    $taskCode=$LASTEXITCODE
    $taskLog | Set-Content (Join-Path $taskOutput "cache-$taskCache.log")
    if($taskCode -ne 0){throw "Depth GPU test failed, cache=$taskCache"}
    @{state_cache=$taskCache;exit_code=$taskCode;log=($taskLog -join "`n")}
  }
} finally {$env:PLUME_D3D12_STATE_CACHE=$taskOldCache}
$taskMutant=(Get-Content -Raw $taskShader).Replace(
  'float sourceX = position.x + ((halfIndex & 1) == 0 ? halfTile : -halfTile);',
  'float sourceX = float(pixel.x % (2 * uint(halfTile)) < uint(halfTile) ? pixel.x + uint(halfTile) : pixel.x - uint(halfTile));')
$taskMutant | Set-Content (Join-Path $taskOutput 'integer-tile.hlsl')
& $Dxc (Join-Path $taskOutput 'integer-tile.hlsl') -I (Split-Path $taskShader) -T ps_6_0 -E main -Fo (Join-Path $taskOutput 'integer-tile.dxil')
if($LASTEXITCODE -ne 0){throw 'Mutant compilation failed'}
$taskMutantLog=& $taskExecutable (Join-Path $taskOutput 'vs.dxil') (Join-Path $taskOutput 'integer-tile.dxil') 2>&1
$taskMutantCode=$LASTEXITCODE
$taskMutantLog | Set-Content (Join-Path $taskOutput 'integer-tile.log')
if($taskMutantCode -ne 1){throw "Integer tile regression was not detected: $taskMutantCode"}
@{game_launched=$false;production_shader=$taskShader;results=@($taskResults);
  integer_tile_mutant_exit=$taskMutantCode;comparisons=12;
  shader_sha256=(Get-FileHash $taskShader).Hash;fixture_sha256=(Get-FileHash $taskSources).Hash} |
  ConvertTo-Json -Depth 6 | Set-Content (Join-Path $taskOutput 'verification.json')
Write-Output 'PASS: 12 real GPU depth restoration cases; truncated tile size rejected'
