/* ============================================================
 * shadow-0.4 rt/ — GC 桩（rt_gc_stub.c）
 * Phase 9-10 之前的 no-op/直通实现：保证产物可链接运行。
 * shadow_gc_alloc 直接走分配器（无 GC 语义）；写屏障/根集记录为空操作。
 * Phase 9 起由真正的 GC 实现替换（对象头 + 类型表 + 三色标记）。
 * ============================================================ */
#include <stdint.h>

extern void* __rt_shadow_malloc(int32_t n);

/* 帧标记：no-op（Phase 9 起插入安全点标记） */
extern int32_t shadow_gc_frame_enter(void) {
    return 0;
}
extern int32_t shadow_gc_frame_leave(int32_t marker) {
    (void)marker;
    return 0;
}

/* 根集记录：no-op（Phase 9 起追踪 slot 指针） */
extern int32_t shadow_gc_root_set(void* slot, void* val) {
    (void)slot; (void)val;
    return 0;
}
extern int32_t shadow_gc_root_range(void* base, uint32_t n) {
    (void)base; (void)n;
    return 0;
}
extern void shadow_gc_poll(void) {
}
extern int32_t shadow_gc_global_root_set(void* g, void* val) {
    (void)g; (void)val;
    return 0;
}
extern void shadow_gc_root_add(void* p) {
    (void)p;
}

/* 分配：直通分配器（Phase 9 起返回 GC 对象头） */
extern void* shadow_gc_alloc(int32_t size, int32_t kind) {
    (void)kind;
    return __rt_shadow_malloc(size);
}

/* finalizer：no-op */
extern void shadow_gc_set_finalizer(void* obj, void* fn) {
    (void)obj; (void)fn;
}

/* 写屏障：no-op（Phase 10 起插入混合写屏障） */
extern void shadow_gc_write_barrier(void* obj, void* val) {
    (void)obj; (void)val;
}

/* 触发收集：no-op（Phase 9 起 STW 标记-清扫；Phase 10 起并发） */
extern void shadow_gc_collect(void) {
}
extern void shadow_gc_minor_collect(void) {
}

/* 开关：no-op */
extern void shadow_gc_disable(void) {
}
extern void shadow_gc_enable(void) {
}

/* 注册：直通 */
extern void* shadow_gc_register(void* p) {
    return p;
}
