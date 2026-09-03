/* ═══════════════════════════════════════════════════════════════════
 * cpu_id_stub.cpp — 用户态 get_processor_id() 宿主实现（外部合约）
 *
 * 合约背景：ravenfs_juvenile.cpp 顶部 `extern "C" uint32_t get_processor_id();`
 * 是核心留出的平台身份缝——核心零隐式平台依赖，per-CPU 分片路由按此返回值
 * 索引 preferred 数组。各宿主必须提供实现：
 *   - SparrowOS 内核：读 GS per-cpu slot（同 fast_get_processor_id 家族），
 *     移植时**必须**提供强定义（缺了 link 直接报错，不会静默错核）；
 *   - 本工程（纯用户态测试/bench 宿主）：本文件提供 **weak** 默认桩。
 *
 * weak 而非强定义的理由：单线程/单片测试（logical_processor_count=1）只需
 * 恒 0；将来多线程分片压测要在每个线程塞假冒 CPU id（arch_prctl(ARCH_SET_GS)
 * + %gs:offset 读，逐位对齐内核 per-cpu 语义），届时压测文件自带强定义
 * 覆盖本桩即可，不必动 CMake。核心库本身不含任何定义——硬合约留给内核侧。
 * ═══════════════════════════════════════════════════════════════════ */
#include <cstdint>

extern "C" __attribute__((weak)) uint32_t get_processor_id()
{
    return 0;   /* 单槽默认：所有分配都落在 CPU 0 的 preferred 片上 */
}
