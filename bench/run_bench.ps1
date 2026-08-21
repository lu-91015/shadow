# shadow vs Go 基准测试运行器
# 用法: powershell -ExecutionPolicy Bypass -File bench\run_bench.ps1
# 输出三列对比：shadow 默认(-O0)、shadow 优化(-O2)、Go
$ErrorActionPreference = "Stop"
$root = "d:\shadow\shadow-0.5"
$env:LLVM_HOME = "D:\llvm\clang+llvm-22.1.0-x86_64-pc-windows-msvc"
$runs = 3

$benches = @("sum_loop", "fib", "matmul", "str_concat", "array_push")
$rt_objs = @(
    "$root\build\rt\rt_core.o", "$root\build\rt\rt_fs.o", "$root\build\rt\rt_proc.o",
    "$root\build\rt\rt_time.o", "$root\build\rt\rt_err.o", "$root\build\rt\rt_extra.o",
    "$root\build\rt\rt_gc.o", "$root\build\rt\rt_thread.o", "$root\build\rt\rt_io.o",
    "$root\bootstrap\miniz.o"
)

function Measure-Best([string]$exe, [int]$n) {
    $best = [double]::MaxValue
    $lastOut = ""
    for ($i = 0; $i -lt $n; $i++) {
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $out = & $exe 2>$null
        $sw.Stop()
        $lastOut = ($out -join " ")
        if ($sw.Elapsed.TotalMilliseconds -lt $best) { $best = $sw.Elapsed.TotalMilliseconds }
    }
    return @($best, $lastOut)
}

# 编译 shadow 基准（默认 -O2，由 src/main.shadow 的 SHADOW_OPT 控制）
Write-Output "== 编译 shadow 基准 (默认 -O2) =="
foreach ($b in $benches) {
    & "$root\build\shadow.exe" "$root\bench\shadow\$b.shadow" --exe 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Output "shadow 编译失败: $b"; exit 1 }
}
Write-Output "shadow -O0 编译完成"

# 编译 shadow -O2 变体（opt -O2 + llc -O2，仅用于分析优化潜力）
Write-Output "== 编译 shadow 基准 (-O2) =="
foreach ($b in $benches) {
    $ll = "$root\bench\shadow\$b.ll"
    $bc = "$root\bench\shadow\$b.o2.bc"
    $obj = "$root\bench\shadow\$b.o2.obj"
    $exe = "$root\bench\shadow\$b.o2.exe"
    & "$root\build\shadow.exe" "$root\bench\shadow\$b.shadow" -o $ll 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Output "shadow -O2 IR 生成失败: $b"; exit 1 }
    & "$env:LLVM_HOME\bin\opt.exe" -O2 $ll -o $bc 2>&1 | Out-Null
    & "$env:LLVM_HOME\bin\llc.exe" -O2 -filetype=obj $bc -o $obj 2>&1 | Out-Null
    $link_args = @("-std=c++17", $obj) + $rt_objs + @("-Wl,/subsystem:console", "-lws2_32", "-lLLVM-C", "-L$env:LLVM_HOME\lib", "-o", $exe)
    & "$env:LLVM_HOME\bin\clang++.exe" @link_args 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Output "shadow -O2 链接失败: $b"; exit 1 }
}
Write-Output "shadow -O2 编译完成"

# 编译 Go 基准
Write-Output "== 编译 Go 基准 =="
foreach ($b in $benches) {
    Push-Location "$root\bench\go"
    go build -o "$b.exe" "$b.go" 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Output "go 编译失败: $b"; Pop-Location; exit 1 }
    Pop-Location
}
Write-Output "go 编译完成"

# 运行并对比
Write-Output ""
Write-Output ("{0,-14} {1,12} {2,12} {3,12} {4,8} {5,8} {6,10}" -f "bench", "shadow(def)", "shadowO2", "go(ms)", "def/go", "O2/go", "shadow_out")
Write-Output ("-" * 80)
foreach ($b in $benches) {
    $s0 = Measure-Best "$root\bench\shadow\$b.shadow.exe" $runs
    $s2 = Measure-Best "$root\bench\shadow\$b.o2.exe" $runs
    $g = Measure-Best "$root\bench\go\$b.exe" $runs
    $r0 = $s0[0] / $g[0]
    $r2 = $s2[0] / $g[0]
    Write-Output ("{0,-14} {1,12:F1} {2,12:F1} {3,12:F1} {4,8:F2}x {5,8:F2}x {6,10}" -f $b, $s0[0], $s2[0], $g[0], $r0, $r2, $s0[1])
}
