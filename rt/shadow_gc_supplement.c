/* ============================================================
 * shadow-0.4 rt/ — 编译器本体符号补充（shadow_gc_supplement.c）
 * ------------------------------------------------------------
 * 编译器本体（stage1/2/3）链接 runtime_for_selfhost.o（0.3 C++ runtime），
 * 其中缺少 0.4 新增的 GC 符号（如 shadow_gc_register_type）。
 * 本文件提供 no-op 定义，保证编译器本体可链接运行；
 * 用户产物不链接本文件（用 rt_gc.o 的真实现）。
 * ============================================================ */
#include <stdint.h>
#include <stddef.h>

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

/* ── LLVM-C 属性补充 ─────────────────────────────────────────────
 * alwaysinline：给热点运行时函数（shadow_schar/shadow_subscript 等）加
 * alwaysinline 属性，使 opt -O2 强制内联进热循环，消除调用开销。
 * 编译器本体（stage1/2/3）链接本文件；用户产物不链接本文件。
 * 手动声明 LLVM-C 符号（本文件编译无 LLVM include 路径）。 */
struct LLVMOpaqueContext;
struct LLVMOpaqueValue;
struct LLVMOpaqueAttributeRef;

extern unsigned LLVMGetEnumAttributeKindForName(const char* Name, size_t SLen);
extern struct LLVMOpaqueAttributeRef* LLVMCreateEnumAttribute(struct LLVMOpaqueContext* C, unsigned KindID, uint64_t Val);
extern void LLVMAddAttributeAtIndex(struct LLVMOpaqueValue* F, unsigned Idx, struct LLVMOpaqueAttributeRef* A);

extern void shadow_llvm_set_alwaysinline(void* ctx, void* fn) {
    unsigned kind = LLVMGetEnumAttributeKindForName("alwaysinline", 12);
    if (kind == 0) return;
    struct LLVMOpaqueAttributeRef* attr =
        LLVMCreateEnumAttribute((struct LLVMOpaqueContext*)ctx, kind, 0);
    LLVMAddAttributeAtIndex((struct LLVMOpaqueValue*)fn, (unsigned)-1, attr);
}
