<#
 Relight Anime プラグインのビルドスクリプト (Windows / VS2022 BuildTools / Ninja)

 使い方:
   .\plugin\build.ps1                      # Release ビルド
   .\plugin\build.ps1 -Install             # ビルド後に AE プラグインフォルダへコピー (管理者権限が必要)
   .\plugin\build.ps1 -SdkRoot C:\dev\AE_SDK
   .\plugin\build.ps1 -RemoteServer https://<深度サーバー>:8445   # Depth Engine = Remote の送り先を設定ファイルに書く
#>
param(
  [string]$SdkRoot = $(if ($env:AE_SDK_ROOT) { $env:AE_SDK_ROOT } else { "C:\dev\AE_SDK" }),
  [string]$Config = "Release",
  [string]$RemoteServer = "",
  [switch]$Install,
  [switch]$Clean
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$pluginDir = Join-Path $root "plugin"
$buildDir = Join-Path $pluginDir "build"

if (-not (Test-Path $SdkRoot)) {
  Write-Host "AE SDK が見つかりません: $SdkRoot" -ForegroundColor Red
  Write-Host "https://developer.adobe.com/after-effects/ から SDK を入手し、展開先を -SdkRoot で指定してください"
  exit 1
}

# MSVC 環境 (vcvars64) を取り込む
$vsBase = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022"
$vcvars = Get-ChildItem -Path $vsBase -Recurse -Filter vcvars64.bat -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $vcvars) { Write-Host "vcvars64.bat が見つかりません (VS2022 BuildTools の C++ ワークロードが必要)" -ForegroundColor Red; exit 1 }
$envDump = cmd /c "`"$($vcvars.FullName)`" >nul 2>&1 && set"
foreach ($line in $envDump) {
  if ($line -match '^([^=]+)=(.*)$') { [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process") }
}

# CMake / Ninja の所在
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
  $pipCmake = python -c "import cmake,os;print(cmake.CMAKE_BIN_DIR)" 2>$null
  if ($pipCmake -and (Test-Path $pipCmake)) { $env:PATH = "$pipCmake;$env:PATH" } else { Write-Host "cmake がありません (pip install cmake)" -ForegroundColor Red; exit 1 }
}
$ninja = Get-Command ninja -ErrorAction SilentlyContinue
if (-not $ninja) {
  $n = Get-ChildItem -Path $vsBase -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($n) { $env:PATH = "$($n.DirectoryName);$env:PATH" } else { Write-Host "ninja がありません" -ForegroundColor Red; exit 1 }
}

if ($Clean -and (Test-Path $buildDir)) { Remove-Item -Recurse -Force $buildDir }

cmake -S $pluginDir -B $buildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DAE_SDK_ROOT=$SdkRoot"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$aex = Join-Path $buildDir "RelightAnime.aex"
Write-Host "ビルド完了: $aex" -ForegroundColor Green

# 自動パス生成の設定ファイル (%APPDATA%\SRLM\RelightAnime.ini)。無い項目だけ追記する
$iniDir = Join-Path $env:APPDATA "SRLM"
$ini = Join-Path $iniDir "RelightAnime.ini"
New-Item -ItemType Directory -Force -Path $iniDir | Out-Null
$py = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $py) { $py = "python" }
# CUDA 版 ONNX Runtime と、それが使う CUDA / cuDNN の DLL の場所 (この PC の既定)
$ortCuda = "C:\ComfyUI_windows_portable\python_embeded\Lib\site-packages\onnxruntime\capi\onnxruntime.dll"
$cudaBin = if ($env:CUDA_PATH) { Join-Path $env:CUDA_PATH "bin" } else { "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin" }
$torchLib = ""
$tl = python -c "import torch,os;print(os.path.join(os.path.dirname(torch.__file__),'lib'))" 2>$null
if ($tl -and (Test-Path $tl)) { $torchLib = $tl }
$defaults = [ordered]@{
  "engine"    = "native"
  "depth_exe" = (Join-Path $root "depth\build\relight_depth.exe")
  "encoder"   = (Join-Path $root "models\vda_small_encoder.onnx")
  "head"      = (Join-Path $root "models\vda_small_head_t32.onnx")
  "provider"  = "cuda"
  "ort"       = $ortCuda
  "dll_dirs"  = (@($cudaBin, $torchLib) | Where-Object { $_ }) -join ";"
  "cache"     = (Join-Path $root "cache")
  "python"    = $py
  "script"    = (Join-Path $root "tools\gen_passes.py")
  "server"    = "http://127.0.0.1:8188"
  # Depth Engine = Remote のときの深度サーバー (tools/depth_server)。-RemoteServer か環境変数 RELIGHT_REMOTE_SERVER で渡す。空ならこの PC で作る
  "remote_server" = $(if ($RemoteServer) { $RemoteServer } elseif ($env:RELIGHT_REMOTE_SERVER) { $env:RELIGHT_REMOTE_SERVER } else { "" })
}
$existing = @()
if (Test-Path $ini) { $existing = [System.IO.File]::ReadAllLines($ini) }
$keys = $existing | ForEach-Object { if ($_ -match '^\s*([^#;=\s][^=]*?)\s*=') { $matches[1] } }
$add = @()
if (-not (Test-Path $ini)) { $add += "# Relight Anime 自動パス生成の設定 (key=value)。engine=native は relight_depth.exe (Python 不要)、comfyui は旧方式" }
foreach ($k in $defaults.Keys) { if ($keys -notcontains $k) { $add += "$k=$($defaults[$k])" } }
if ($add.Count -gt 0) {
  [System.IO.File]::WriteAllLines($ini, [string[]]($existing + $add), (New-Object System.Text.UTF8Encoding $true))
  Write-Host "設定ファイルに追記: $ini ($($add.Count) 行)"
}

if ($Install) {
  $dest = "C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\SRLM"
  try {
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    # エフェクト本体と補助 AEGP (アイドルフック担当) の 2 つを入れる
    Copy-Item $aex, (Join-Path $buildDir "RelightAnimeHelper.aex") -Destination $dest -Force
    Write-Host "インストール完了: $dest (AE を再起動してください)" -ForegroundColor Green
  } catch {
    Write-Host "コピーに失敗しました。管理者権限の PowerShell で再実行してください: $_" -ForegroundColor Yellow
  }
}
