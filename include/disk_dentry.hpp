#pragma once

#include "stdint.h"
union dentry_node_meta_t {
    struct {
        uint64_t entry_count : 6;   /* 有效 entry 数 - 1 */
        uint64_t reserved0:2;
        uint64_t is_internal_node   : 1;   /* 0=leaf, 1=internal */
        uint64_t is_root_node   : 1;
        uint64_t reserved           : 2;
        uint64_t parent_ptr         : 52;  /* 非根→父节点块号 | 根→所属 inode 编号 */
    } fields;
    uint64_t raw;
}__attribute__((packed));
static_assert(sizeof(dentry_node_meta_t) == 8, "");
struct Dentry_head_t{
    dentry_node_meta_t head;
    uint64_t ptr1;
    uint64_t ptr2;
    uint64_t reserved0;
    uint64_t magic1;
    uint64_t magic2;
    uint64_t magic3;
    uint64_t magic4;
    uint64_t reserved2[8];
}__attribute__((packed));
static_assert(sizeof(Dentry_head_t) == 128, "");
struct Dentry_common_t{
    uint64_t ptr;
    char name[120];
}__attribute__((packed));
static_assert(sizeof(Dentry_common_t) == 128, "");
struct Dentry_Bptree_node_t{
    Dentry_head_t head;
    Dentry_common_t entries[63];
};