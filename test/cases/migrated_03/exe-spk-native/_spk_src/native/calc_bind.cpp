// native/calc_bind.cpp — rt_calc_create / rt_calc_add 的 C++ 实现
// 由 shadow-0.3 编译器在链接期自动编译为 .o 并链接到目标 exe
//
// opaque 类型在 LLVM 层是 i8*（64 位指针），对应 C++ 的 void*
// 句柄语义：rt_calc_create 在堆上分配一个 int32，返回其地址作为 opaque 句柄
//           rt_calc_add 解引用句柄取出 int32，加上 addend 后返回

#include <cstdint>
#include <cstdlib>

extern "C" {

// 创建句柄：在堆上分配 int32 存储 value，返回地址作为 opaque 句柄
void* rt_calc_create(int32_t value) {
    int32_t* p = (int32_t*)std::malloc(sizeof(int32_t));
    *p = value;
    return (void*)p;
}

// 读取句柄存储的值并加上 addend
int32_t rt_calc_add(void* handle, int32_t addend) {
    int32_t* p = (int32_t*)handle;
    return *p + addend;
}

} // extern "C"
