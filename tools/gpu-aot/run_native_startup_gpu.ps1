param(
  [Parameter(Mandatory=$true)][string]$OutputDirectory,
  [Parameter(Mandatory=$true)][string]$PlumeLibrary,
  [Parameter(Mandatory=$true)][string]$Dxc
)
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$taskOutput=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force "$taskOutput/include/gpu_native/shaders" | Out-Null
$taskCmake=Get-Content -Raw "$taskRoot/rexlego/CMakeLists.txt"
$taskProfiles=@{}
foreach($taskStage in @('vs','ps')) {
  $taskMatch=[regex]::Match($taskCmake,"set\(LEGO_HOST_SHADER_PROFILE (${taskStage}_6_[0-9]+)\)")
  if(!$taskMatch.Success){throw "Missing production $taskStage profile"}
  $taskProfiles[$taskStage]=$taskMatch.Groups[1].Value
  $taskStem=if($taskStage -eq 'vs'){'copy_vs'}else{'copy_color_ps'}
  & $Dxc "$taskRoot/rexlego/src/gpu_native/shaders/$taskStem.hlsl" -T $taskProfiles[$taskStage] -E main -Fh "$taskOutput/include/gpu_native/shaders/$taskStem.h" -Vn "g_${taskStem}_dxil"
  if($LASTEXITCODE -ne 0){throw "Compile $taskStem failed"}
}
$taskCompiler=(Get-Command clang++).Source
$taskExecutable="$taskOutput/native-startup.exe"
$taskIncludes=@("-I$taskRoot/thirdparty/plume","-I$taskRoot/thirdparty/plume/contrib/D3D12MemoryAllocator/include","-I$taskOutput/include")
& $taskCompiler -std=c++20 -O2 -fms-runtime-lib=dll '-Wl,/nodefaultlib:libcmt' @taskIncludes "$PSScriptRoot/test_native_startup_gpu.cpp" $PlumeLibrary -ld3d12 -ldxgi -ldxguid -ld3dcompiler -luser32 -o $taskExecutable *> "$taskOutput/compile.log"
if($LASTEXITCODE -ne 0){throw 'Startup fixture compilation failed'}
$ErrorActionPreference='Continue'
$taskLog=& $taskExecutable 2>&1
$taskCode=$LASTEXITCODE
$ErrorActionPreference='Stop'
$taskLog | Set-Content "$taskOutput/startup.log"
@{game_launched=$false;profiles=$taskProfiles;exit_code=$taskCode;log=($taskLog -join "`n");
  fixture_sha256=(Get-FileHash "$PSScriptRoot/test_native_startup_gpu.cpp").Hash;
  scope='Production present shaders and CMake profiles; Plume device, native root layout and PSO creation; no gameplay or FPS claim'} |
  ConvertTo-Json -Depth 4 | Set-Content "$taskOutput/verification.json"
if($taskCode -ne 0){throw "Startup GPU fixture failed: $taskCode"}
Write-Output 'PASS: production present shaders create a pipeline on the selected GPU'
