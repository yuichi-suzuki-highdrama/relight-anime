<#
 relight_cli.exe (プラグインの照明コードを AE なしで動かす検証ツール) をビルドする
   .\tools\harness\build_cli.ps1 [-SdkRoot C:\dev\AE_SDK\AfterEffectsSDK_26.5_win]
#>
param(
  [string]$SdkRoot = $(if ($env:AE_SDK_ROOT) { $env:AE_SDK_ROOT } else { "C:\dev\AE_SDK\AfterEffectsSDK_26.5_win" })
)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$root = Split-Path -Parent (Split-Path -Parent $here)
$ex = Join-Path $SdkRoot "Examples"

$vsBase = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022"
$vcvars = Get-ChildItem -Path $vsBase -Recurse -Filter vcvars64.bat -ErrorAction SilentlyContinue | Select-Object -First 1
$envDump = cmd /c "`"$($vcvars.FullName)`" >nul 2>&1 && set"
foreach ($line in $envDump) {
  if ($line -match '^([^=]+)=(.*)$') { [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process") }
}

$out = Join-Path $here "build"
New-Item -ItemType Directory -Force -Path $out | Out-Null
Push-Location $out
try {
  # GPU 版 (CUDA) も入れる。CUDA Toolkit が無ければ CPU 版だけ
  $cuda = if ($env:CUDA_PATH) { $env:CUDA_PATH } else { "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8" }
  $nvcc = Join-Path $cuda "bin\nvcc.exe"
  $cudaArgs = @()
  if (Test-Path $nvcc) {
    & $nvcc -c -O3 -arch=sm_120 -Xcompiler=/utf-8 -Xcompiler=/EHsc -DNOMINMAX "-I$root\plugin\src" (Join-Path $root "plugin\src\RelightGPU.cu") -o RelightGPU.obj
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $cudaArgs = @("/DRELIGHT_HAS_CUDA", "/I$cuda\include")
  }
  cl /nologo /O2 /EHsc /utf-8 /fp:fast /std:c++17 /DWIN32 /D_WINDOWS /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS `
    "/I$root\plugin\src" "/I$ex\Headers" "/I$ex\Headers\SP" "/I$ex\Util" `
    (Join-Path $here "relight_cli.cpp") `
    (Join-Path $root "plugin\src\RelightAuto.cpp") `
    (Join-Path $ex "Util\AEGP_SuiteHandler.cpp") `
    (Join-Path $ex "Util\MissingSuiteError.cpp") `
    $cudaArgs $(if ($cudaArgs) { @("RelightGPU.obj", "$cuda\lib\x64\cudart_static.lib") }) `
    shell32.lib user32.lib gdi32.lib /Fe:relight_cli.exe
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  Write-Host "ビルド完了: $out\relight_cli.exe" -ForegroundColor Green
} finally { Pop-Location }
