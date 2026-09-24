# 编译单个 exe 用例：.shadow -> .ll -> .o -> .exe -> 运行
# 用法: powershell -File tools\run_case.ps1 -Src test\cases\exe\ex_trait_dyn.shadow
param(
    [Parameter(Mandatory=$true)][string]$Src
)
$ErrorActionPreference = 'Stop'
$bin = "D:\llvm\clang+llvm-22.1.0-x86_64-pc-windows-msvc\bin"
$rtlib = "D:\llvm\clang+llvm-22.1.0-x86_64-pc-windows-msvc\lib"
$base = [System.IO.Path]::GetFileNameWithoutExtension($Src)
Set-Location d:\shadow\shadow-0.5

Write-Output "[1] compile -> build\$base.ll"
& 'build\shadow.exe' $Src -o "build\$base.ll" 2>&1 | Out-Null
if (-not (Test-Path "build\$base.ll")) { Write-Output "COMPILE_FAIL"; exit 1 }

Write-Output "[2] llc -> build\$base.o"
& "$bin\llc.exe" -O2 -filetype=obj "build\$base.ll" -o "build\$base.o"
if ($LASTEXITCODE -ne 0) { Write-Output "LLC_FAIL"; exit 1 }

Write-Output "[3] link -> build\$base.exe"
& "$bin\clang++.exe" "-std=c++17" "build\$base.o" `
  "build\rt\runtime_for_selfhost_user.o" "build\rt\miniz.o" `
  "build\rt\rt_zip.o" "build\rt\shadow_index.o" `
  "build\rt\rt_proc_spawn.o" "build\rt\rt_test_par.o" `
  "build\rt\sys_exec_cpa.o" "build\rt\cxa_atexit_shim.o" `
  "-o" "build\$base.exe" "-Wl,/subsystem:console" `
  "-lws2_32" "-lLLVM-C" "-L$rtlib" "-Lpkg\shadow\llvm\crt"
if ($LASTEXITCODE -ne 0) { Write-Output "LINK_FAIL"; exit 1 }

Write-Output "[4] run"
& "build\$base.exe"
Write-Output "RUN_EXIT=$LASTEXITCODE"