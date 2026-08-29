#pragma once
#include "stdint.h"
enum class cache_work_mode_t{
    no_cache,//不缓存，相应的session自己分配内存，调用管理器的回调函数接口，折构时session自己释放
    self_reliat_cache,//自力更生用户态缓存
    offload_cache //背后有mmap的内核文件页缓存
};