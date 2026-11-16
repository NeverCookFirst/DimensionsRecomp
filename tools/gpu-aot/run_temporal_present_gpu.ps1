param(
  [Parameter(Mandatory=$true)][string]$OutputDirectory,
  [Parameter(Mandatory=$true)][string]$PlumeLibrary,
  [Parameter(Mandatory=$true)][string]$Dxc
)
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$taskOutput=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force (Join-Path $taskOutput 'include/rex') | Out-Null
# Log output is a fixture boundary; production rendering/ownership code is
# compiled unchanged. The host fixture retains resources until its GPU fence.
@'
#pragma once
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
#define REXLOG_ERROR(...) ((void)0)
'@ | Set-Content -LiteralPath (Join-Path $taskOutput 'include/rex/logging.h')
foreach($taskStage in @('vs','ps')) {
  $taskShader=if($taskStage -eq 'vs'){'copy_vs.hlsl'}else{'copy_color_ps.hlsl'}
  & $Dxc (Join-Path $taskRoot "rexlego/src/gpu_native/shaders/$taskShader") -T "${taskStage}_6_0" -E main -Fo (Join-Path $taskOutput "copy-$taskStage.dxil")
  if($LASTEXITCODE -ne 0){throw "Copy $taskStage shader failed"}
}
$taskSources=@(
  (Join-Path $PSScriptRoot 'test_temporal_present_gpu.cpp'),
  (Join-Path $taskRoot 'rexlego/src/gpu_native/temporal_present.cpp'),
  (Join-Path $taskRoot 'rexlego/src/gpu_native/fsr3_upscaler.cpp'))
$taskIncludes=@('rexlego/src','rexglue-sdk/include','thirdparty/plume',
  'thirdparty/plume/contrib/D3D12MemoryAllocator/include',
  'rexglue-sdk/thirdparty/FidelityFX-SDK/sdk/include',
  'rexglue-sdk/thirdparty/FidelityFX-SDK/ffx-api/include') | ForEach-Object {"/I$(Join-Path $taskRoot $_)"}
$taskLibraries=@('rexglue-sdk/bin/ffx_sdk/ffx_backend_dx12_x64.lib',
  'rexglue-sdk/bin/ffx_sdk/ffx_fsr3upscaler_x64.lib') | ForEach-Object {Join-Path $taskRoot $_}
$taskExecutable=Join-Path $taskOutput 'test-present.exe'
& clang-cl @taskSources /std:c++latest /O2 /MD /EHsc /UNDEBUG /DNOMINMAX "/I$(Join-Path $taskOutput 'include')" @taskIncludes "/Fo$taskOutput/" "/Fe$taskExecutable" /link $PlumeLibrary @taskLibraries d3d12.lib dxgi.lib dxguid.lib user32.lib *> (Join-Path $taskOutput 'compile.log')
if($LASTEXITCODE -ne 0){throw 'Presentation fixture compilation failed; see compile.log'}
$taskPreviousCache=$env:PLUME_D3D12_STATE_CACHE
try {
  $taskResults=foreach($taskCache in @('0','1')) {
    $env:PLUME_D3D12_STATE_CACHE=$taskCache
    foreach($taskMode in @(@(1920,1080),@(1280,720),@(1129,635),@(960,540),@(640,360))) {
      foreach($taskScene in @('flat','pattern','fallback','late-camera')) {
      $taskArguments=@($taskMode[0],$taskMode[1],(Join-Path $taskOutput 'copy-vs.dxil'),(Join-Path $taskOutput 'copy-ps.dxil'))
      if($taskScene -ne 'flat'){$taskArguments+=$taskScene}
      $taskLog=& $taskExecutable @taskArguments 2>&1
      $taskExit=$LASTEXITCODE
      $taskLog | Set-Content -LiteralPath (Join-Path $taskOutput "mode-$($taskMode[0])-cache-$taskCache-$taskScene.log")
      $taskLog | Write-Output | Out-Host
      @{width=$taskMode[0];height=$taskMode[1];state_cache=$taskCache;scene=$taskScene;exit_code=$taskExit;stdout=($taskLog -join "`n")}
      }
    }
  }
} finally {$env:PLUME_D3D12_STATE_CACHE=$taskPreviousCache}
$taskPreviousToggle=$env:LEGO_TEST_TOGGLE_MODE
$taskPreviousSwitch=$env:LEGO_TEST_SWITCH_PRESETS
try {
  $env:LEGO_TEST_TOGGLE_MODE='1';$env:LEGO_TEST_SWITCH_PRESETS='1'
  foreach($taskCache in @('0','1')) {
    $env:PLUME_D3D12_STATE_CACHE=$taskCache
    & $taskExecutable 1920 1080 (Join-Path $taskOutput 'copy-vs.dxil') (Join-Path $taskOutput 'copy-ps.dxil') pattern *> (Join-Path $taskOutput "live-toggle-cache-$taskCache.log")
    if($LASTEXITCODE){throw "Live FSR toggle/resize GPU fixture failed: $LASTEXITCODE"}
  }
} finally {
  $env:LEGO_TEST_TOGGLE_MODE=$taskPreviousToggle;$env:LEGO_TEST_SWITCH_PRESETS=$taskPreviousSwitch
  $env:PLUME_D3D12_STATE_CACHE=$taskPreviousCache
}
$taskMutantLog=& $taskExecutable 1280 720 (Join-Path $taskOutput 'copy-vs.dxil') (Join-Path $taskOutput 'copy-ps.dxil') invert-jitter 2>&1
$taskMutantExit=$LASTEXITCODE
$taskMutantLog | Set-Content -LiteralPath (Join-Path $taskOutput 'inverted-jitter.log')
if($taskMutantExit -ne 24){throw "Wrong jitter sign was not detected: $taskMutantExit"}
$taskShaderDirectory=Join-Path $taskRoot 'rexlego/src/gpu_native/shaders'
$taskFallbackMutant=(Get-Content -Raw (Join-Path $taskShaderDirectory 'copy_color_ps.hlsl')).Replace('tex_coord + sample_offset','tex_coord')
$taskFallbackMutant | Set-Content -LiteralPath (Join-Path $taskOutput 'copy-no-offset.hlsl')
& $Dxc (Join-Path $taskOutput 'copy-no-offset.hlsl') -I $taskShaderDirectory -T ps_6_0 -E main -Fo (Join-Path $taskOutput 'copy-no-offset.dxil')
if($LASTEXITCODE -ne 0){throw 'Fallback mutant compilation failed'}
$taskFallbackLog=& $taskExecutable 1280 720 (Join-Path $taskOutput 'copy-vs.dxil') (Join-Path $taskOutput 'copy-no-offset.dxil') fallback 2>&1
$taskFallbackExit=$LASTEXITCODE
$taskFallbackLog | Set-Content -LiteralPath (Join-Path $taskOutput 'fallback-no-offset.log')
if($taskFallbackExit -ne 24){throw "Uncorrected spatial fallback was not detected: $taskFallbackExit"}
$taskHashes=foreach($taskFile in $taskSources+@($PlumeLibrary)+$taskLibraries+@($taskExecutable)) {
  @{path=$taskFile;sha256=(Get-FileHash -LiteralPath $taskFile -Algorithm SHA256).Hash}
}
@{game_launched=$false;frame_generation=$false;production_presenter_tested=$true;
  game_integration_visually_tested=$false;host_ownership='fixture with real GPU fence';
  final_copy='Production presentation shader after production FSR dispatch';inverted_jitter_exit=$taskMutantExit;fallback_no_offset_exit=$taskFallbackExit;results=@($taskResults);files=@($taskHashes)} |
  ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $taskOutput 'verification.json')
if(@($taskResults | Where-Object {$_.exit_code -ne 0}).Count){throw 'Presentation mode failed'}
$global:LASTEXITCODE=0
