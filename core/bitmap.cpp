#include "ravenfs/ravenfs.h"

bool RavenFS::bitmap_test(uint64_t block) {
    (void)block;
    /* TODO */
    return false;
}

bool RavenFS::bitmap_set(uint64_t block, bool allocated) {
    (void)block; (void)allocated;
    /* TODO */
    return false;
}

bool RavenFS::bitmap_alloc(uint64_t* block) {
    (void)block;
    /* TODO */
    return false;
}
