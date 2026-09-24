# 快速重建：应用 rt/linux/runtime_for_selfhost.cpp 与 src/codegen/codegen.shadow 改动。
# 1) 从源码重编 runtime .o 并派生 wk.o / user.o（含新增 shadow_hashmap_insert_float/ptr）
# 2) 用当前 build/shadow.exe 编 src/main.shadow -> stage1 -> 链接为新 shadow.exe.new
$ErrorActionPreference = 'Stop'
$LH = 'D:\llvm\clang+llvm-22.1.0-x86_64-pc-windows-msvc'
$BIN = "$LH\bin"
$RT = 'build\rt'
Set-Location 'd:\shadow\shadow-0.5'

Write-Output '[1] runtime .o from cpp'
& "$BIN\clang++.exe" @('-O2','-c','rt\linux\runtime_for_selfhost.cpp','-o',"$RT\runtime_for_selfhost.o",'-I','rt',"-I","$LH\include",'-std=c++17','-D_CRT_SECURE_NO_WARNINGS','-w')
if ($LASTEXITCODE -ne 0) { throw 'clang++ runtime failed' }

$wkSyms = 'shadow_any_box_ptr','shadow_any_box_string','shadow_any_print','shadow_any_to_string','shadow_any_unbox_ptr',`
 'shadow_array_get_int','shadow_array_get_ptr','shadow_array_len','shadow_array_pop','shadow_array_push_float',`
 'shadow_array_push_int','shadow_array_push_long','shadow_array_push_ptr','shadow_array_set_int','shadow_array_set_ptr',`
 'shadow_content_hash','shadow_gc_poll','shadow_gc_register_type','shadow_gc_root_range','shadow_int_to_str',`
 'shadow_llvm_set_alwaysinline','shadow_println_int','shadow_println_str','shadow_set_cli_args','shadow_string_concat',`
 'shadow_string_len','shadow_sys_args','shadow_zip_list','shadow_zip_pack','shadow_zip_unpack'

Write-Output '[2] wk.o'
$wargs = @('--redefine-sym=shadow_sys_exec=shadow_sys_exec_blob')
foreach ($s in $wkSyms) { $wargs += "--redefine-sym=$s=__cpp_$s" }
$wargs += "$RT\runtime_for_selfhost.o","$RT\runtime_for_selfhost_wk.o"
& "$BIN\llvm-objcopy.exe" $wargs
if ($LASTEXITCODE -ne 0) { throw 'objcopy wk failed' }

Write-Output '[3] user.o'
$uargs = @('--redefine-sym=shadow_sys_exec=__cpp_shadow_sys_exec')
foreach ($s in $wkSyms) {
  if ($s -ne 'shadow_gc_poll' -and $s -ne 'shadow_gc_root_range' -and $s -ne 'shadow_gc_register_type') { $uargs += "--redefine-sym=$s=__cpp_$s" }
}
$uargs += "$RT\runtime_for_selfhost.o","$RT\runtime_for_selfhost_user.o"
& "$BIN\llvm-objcopy.exe" $uargs
if ($LASTEXITCODE -ne 0) { throw 'objcopy user failed' }

Write-Output '[4] compile src/main.shadow -> stage1'
& 'build\shadow.exe' @('src\main.shadow','-o','build\stage1.ll')
if ($LASTEXITCODE -ne 0) { throw 'compile main failed' }
& "$BIN\llc.exe" @('-O2','-filetype=obj','build\stage1.ll','-o','build\stage1.o')
if ($LASTEXITCODE -ne 0) { throw 'llc failed' }

Write-Output '[5] link new shadow.exe.new'
$lnk = @('-std=c++17','build\stage1.o',"$RT\runtime_for_selfhost_wk.o","$RT\miniz.o","$RT\shadow_gc_supplement.o","$RT\rt_zip.o","$RT\shadow_index.o","$RT\rt_proc_spawn.o","$RT\sys_exec_cpa.o","$RT\cxa_atexit_shim.o",'-o','build\shadow.exe.new','-Wl,/subsystem:console','-Wl,/Brepro','-Wl,/stack:8388608','-lws2_32','-lLLVM-C',"-L$LH\lib","-Lpkg\shadow\llvm\crt")
& "$BIN\clang++.exe" $lnk
if ($LASTEXITCODE -ne 0) { throw 'link shadow failed' }
Write-Output 'OK -> build\shadow.exe.new'