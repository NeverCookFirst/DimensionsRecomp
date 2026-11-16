param([Parameter(Mandatory=$true)][string]$OutputDirectory,[Parameter(Mandatory=$true)][string]$PlumeLibrary,[Parameter(Mandatory=$true)][string]$Dxc)
$ErrorActionPreference='Stop'
$taskOutput=[IO.Path]::GetFullPath($OutputDirectory)
$taskRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
New-Item -ItemType Directory -Force $taskOutput | Out-Null
@'
void main(uint id:SV_VertexID,in float a:TEXCOORD0,in float b:TEXCOORD1,out float4 position:SV_Position,out float2 value:TEXCOORD0){
 position=float4(id==0?float2(-1,-1):id==1?float2(3,-1):float2(-1,3),.5,1);value=float2(a,b);
}
'@ | Set-Content -LiteralPath (Join-Path $taskOutput 'vs.hlsl')
@'
float4 main(in float4 position:SV_Position,in float2 value:TEXCOORD0):SV_Target0{return float4(value,0,1);}
'@ | Set-Content -LiteralPath (Join-Path $taskOutput 'ps.hlsl')
foreach($taskStage in @('vs','ps')){
 & $Dxc (Join-Path $taskOutput "$taskStage.hlsl") -T "${taskStage}_6_0" -E main -Fo (Join-Path $taskOutput "$taskStage.dxil")
 if($LASTEXITCODE -ne 0){throw 'Shader compilation failed'}
}
@'
void main(uint id:SV_VertexID,in float a:TEXCOORD0,in float b:TEXCOORD1,out float4 position:SV_Position,out float2 value:TEXCOORD0){
 position=float4(id==0?float2(-1,-1):id==1?float2(3,-1):float2(-1,3),.5,1);value=float2(a,9);
}
'@ | Set-Content -LiteralPath (Join-Path $taskOutput 'unused-vs.hlsl')
& $Dxc (Join-Path $taskOutput 'unused-vs.hlsl') -T vs_6_0 -E main -Fo (Join-Path $taskOutput 'unused-vs.dxil')
if($LASTEXITCODE -ne 0){throw 'Unused-input shader compilation failed'}
$taskExe=Join-Path $taskOutput 'test-bindings.exe'
& clang-cl (Join-Path $PSScriptRoot 'test_vertex_binding_cache_gpu.cpp') /std:c++20 /O2 /MD /EHsc /UNDEBUG /DNOMINMAX "/I$(Join-Path $taskRoot 'rexlego/src')" "/I$(Join-Path $taskRoot 'thirdparty/plume')" "/I$(Join-Path $taskRoot 'thirdparty/plume/contrib/D3D12MemoryAllocator/include')" "/Fo$taskOutput/fixture.obj" "/Fe$taskExe" /link $PlumeLibrary d3d12.lib dxgi.lib dxguid.lib user32.lib *> (Join-Path $taskOutput 'compile.log')
if($LASTEXITCODE -ne 0){throw 'Binding GPU fixture compilation failed'}
$taskPreviousCache=$env:PLUME_D3D12_STATE_CACHE
try{
 $taskResults=foreach($taskCache in @('0','1')){
  $env:PLUME_D3D12_STATE_CACHE=$taskCache
  $taskLog=& $taskExe (Join-Path $taskOutput 'vs.dxil') (Join-Path $taskOutput 'ps.dxil') 2>&1
  $taskExit=$LASTEXITCODE;$taskLog | Set-Content -LiteralPath (Join-Path $taskOutput "cache-$taskCache.log")
  if($taskExit -ne 0){throw "Binding GPU oracle failed: $taskExit"}
  @{state_cache=$taskCache;exit_code=$taskExit;stdout=($taskLog -join "`n")}
 }
 $taskUnusedResults=foreach($taskCache in @('0','1')){
  $env:PLUME_D3D12_STATE_CACHE=$taskCache
  $taskLog=& $taskExe (Join-Path $taskOutput 'unused-vs.dxil') (Join-Path $taskOutput 'ps.dxil') unused-stream 2>&1
  $taskExit=$LASTEXITCODE;$taskLog | Set-Content -LiteralPath (Join-Path $taskOutput "unused-cache-$taskCache.log")
  if($taskExit -ne 0){throw "Unused-stream GPU oracle failed: $taskExit"}
  @{state_cache=$taskCache;exit_code=$taskExit;stdout=($taskLog -join "`n")}
 }
 $taskNegative=& $taskExe (Join-Path $taskOutput 'vs.dxil') (Join-Path $taskOutput 'ps.dxil') omit-offset 2>&1
 $taskNegativeExit=$LASTEXITCODE;$taskNegative | Set-Content -LiteralPath (Join-Path $taskOutput 'negative.log')
 if($taskNegativeExit -ne 23){throw 'Offset mutation was not detected'}
 @{game_launched=$false;window_opened=$false;production_cache=$true;actual_plume_bindings=$true;cases=$taskResults;unused_stream_cases=$taskUnusedResults;negative_exit=$taskNegativeExit} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $taskOutput 'verification.json')
 Write-Output 'PASS cached/uncached IA GPU oracle and omitted-offset negative control'
}finally{if($null -eq $taskPreviousCache){Remove-Item Env:PLUME_D3D12_STATE_CACHE -ErrorAction SilentlyContinue}else{$env:PLUME_D3D12_STATE_CACHE=$taskPreviousCache}}
exit 0
