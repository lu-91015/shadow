param(
    [string]$Src,
    [string]$Out = ""
)
# 编译 shadow 源码 -> exe 并运行
$llvm = 'D:\llvm\clang+llvm-22.1.0-x86_64-pc-windows-msvc'
$bin = "$llvm\bin"
Set-Location d:\shadow\shadow-0.5
if ($Out -eq "") { $Out = "build\rt_" + [System.IO.Path]::GetFileNameWithoutExtension($Src) + ".exe" }
$base = [System.IO.Path]::GetFileNameWithoutExtension($Src)
$ll = "build\rt_$base.ll"
$bc = "build\rt_$base.o2.bc"
$obj = "build\rt_$base.o2.o"

& build\shadow-stage1-dbg2.exe $Src -o $ll 2>&1 | Out-Null
if (-not (Test-Path $ll)) { Write-Error "compile failed: $Src"; exit 1 }
& "$bin\opt.exe" -O2 $ll -o $bc 2>&1 | Out-Null
& "$bin\llc.exe" -O2 -filetype=obj $bc -o $obj 2>&1 | Out-Null
$largs = @($obj,"build\rt\rt_core.o","build\rt\rt_fs.o","build\rt\rt_proc.o","build\rt\rt_proc_spawn.o","build\rt\rt_time.o","build\rt\rt_err.o","build\rt\rt_extra.o","build\rt\rt_gc.o","build\rt\rt_thread.o","build\rt\rt_io.o","-o",$Out,"-Wl,/subsystem:console")
& "$bin\clang++.exe" $largs 2>&1 | Out-Null
if (-not (Test-Path $Out)) { Write-Error "link failed: $Src"; exit 1 }
Write-Output "=== $Src ==="
& $Out
