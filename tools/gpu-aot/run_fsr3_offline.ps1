param(
  [Parameter(Mandatory=$true)][string]$SdkRoot,
  [Parameter(Mandatory=$true)][string]$OutputDirectory,
  [string]$FrameGenerationProvider,
  [switch]$NativeModule
)
$ErrorActionPreference = 'Stop'
$taskSdk = (Resolve-Path -LiteralPath $SdkRoot).Path
$taskOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
$taskSource = Join-Path $PSScriptRoot 'test_fsr3_offline.cpp'
$taskExecutable = Join-Path $taskOutput 'test-fsr3-offline.exe'
$taskObject = Join-Path $taskOutput 'fsr3-smoke.obj'
$taskInclude = Join-Path $taskSdk 'thirdparty/FidelityFX-SDK/sdk/include'
$taskApiInclude = Join-Path $taskSdk 'thirdparty/FidelityFX-SDK/ffx-api/include'
$taskBackend = Join-Path $taskSdk 'bin/ffx_sdk/ffx_backend_dx12_x64.lib'
$taskUpscaler = Join-Path $taskSdk 'bin/ffx_sdk/ffx_fsr3upscaler_x64.lib'
$taskSources = @($taskSource)
$taskFlags = @("/Fo$taskObject")
if ($NativeModule) {
  if ($FrameGenerationProvider) { throw 'Native upscaler module has no frame generation' }
  $taskNativeRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../rexlego/src'))
  $taskSources += Join-Path $taskNativeRoot 'gpu_native/fsr3_upscaler.cpp'
  $taskFlags = @('/DLEGO_FSR_TEST_NATIVE_MODULE', '/DNOMINMAX', "/I$taskNativeRoot", "/Fo$taskOutput/")
}
& clang-cl @taskSources @taskFlags /std:c++20 /O2 /MD /EHsc "/I$taskInclude" "/I$taskApiInclude" "/Fe$taskExecutable" /link $taskBackend $taskUpscaler d3d12.lib dxgi.lib dxguid.lib
if ($LASTEXITCODE -ne 0) { throw 'FSR fixture compilation failed' }
$taskModes = @(
  @{name='Native AA';width=1920;height=1080},
  @{name='Quality';width=1280;height=720},
  @{name='Balanced';width=1129;height=635},
  @{name='Performance';width=960;height=540},
  @{name='Ultra Performance';width=640;height=360}
)
$taskResults = foreach ($taskMode in $taskModes) {
  $taskArguments = @($taskMode.width,$taskMode.height,1920,1080)
  if ($FrameGenerationProvider) { $taskArguments += (Resolve-Path -LiteralPath $FrameGenerationProvider).Path }
  $taskLog = & $taskExecutable @taskArguments
  $taskExit = $LASTEXITCODE
  $taskLog | Write-Output | Out-Host
  @{name=$taskMode.name;render_width=$taskMode.width;render_height=$taskMode.height;exit_code=$taskExit;stdout=($taskLog -join "`n")}
}
$taskHashes = foreach ($taskFile in @($taskSources) + @($taskBackend,$taskUpscaler,$taskExecutable)) {
  @{path=$taskFile;sha256=(Get-FileHash -LiteralPath $taskFile -Algorithm SHA256).Hash}
}
if ($FrameGenerationProvider) {
  $taskHashes += @{path=(Resolve-Path -LiteralPath $FrameGenerationProvider).Path;sha256=(Get-FileHash -LiteralPath $FrameGenerationProvider -Algorithm SHA256).Hash}
}
@{game_launched=$false;native_integration_proven=$false;production_dispatch_module_tested=[bool]$NativeModule;moving_object_test=[bool]$env:LEGO_FSR_TEST_MOVING;frame_generation_tested=[bool]$FrameGenerationProvider;frame_generation_provider=$FrameGenerationProvider;swapchain_pacing_tested=$false;results=@($taskResults);files=@($taskHashes)} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $taskOutput 'verification.json') -Encoding utf8
if (@($taskResults | Where-Object {$_.exit_code -ne 0}).Count) { throw 'At least one FSR mode failed' }
