/* ============================================================
 * shadow-0.4 rt/ — 编译器本体符号补充（shadow_gc_supplement.c）
 * ------------------------------------------------------------
 * 编译器本体（stage1/2/3）链接 runtime_for_selfhost.o（0.3 C++ runtime），
 * 其中缺少 0.4 新增的 GC 符号（如 shadow_gc_register_type）。
 * 本文件提供 no-op 定义，保证编译器本体可链接运行；
 * 用户产物不链接本文件（用 rt_gc.o 的真实现）。
 * ============================================================ */
#include <stdint.h>

/* 0.4 GC 类型注册：编译器本体运行时 no-op（类型表仅在产物 rt 层生效） */
extern int32_t shadow_gc_register_type(int32_t id, int32_t size, int64_t bitmap) {
    (void)id; (void)size; (void)bitmap;
    return 1;
}

/* §5.2.4 range 根：编译器本体运行时不执行 GC 扫描，no-op 即可
 * （用户产物链接 rt_gc.o 的真实现）。 */
extern int32_t shadow_gc_root_range(void* base, uint32_t n) {
    (void)base; (void)n;
    return 0;
}

/* §5.2.4 协作式安全点 poll：编译器本体（自举编译器自身）没有 GC，no-op。
 * 用户产物链接 rt_gc.o 的真实现（检查 STW 请求并在安全点让出）。 */
extern void shadow_gc_poll(void) {
}
