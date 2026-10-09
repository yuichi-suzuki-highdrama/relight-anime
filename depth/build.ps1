<#
 relight_depth.exe (深度推定プログラム) をビルドする
   .\depth\build.ps1
#>
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$root = Split-Path -Parent $here

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
  cl /nologo /O2 /EHsc /utf-8 /std:c++17 /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS `
    "/I$root\third_party\onnxruntime\include" `
    (Join-Path $here "relight_depth.cpp") /Fe:relight_depth.exe
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  Write-Host "ビルド完了: $out\relight_depth.exe" -ForegroundColor Green
} finally { Pop-Location }
