/* ═══════════════════════════════════════════════════════════════════════════
 * mkcontent.cpp — 仅供测试的最小内容填充器（写**副本**；auditor 自身永不写盘）
 *
 * 用途：把 mkfs 造出的空盘副本填成「有内容」的镜像，供 audit_v1.2 递归校验。
 *   root/ 含 → { sub/、inline.txt(REG inline)、link(LNK)、fifo、sock、chardev(CHR rdev)、
 *               direct.bin(REG direct)、extent.bin(REG extent) }
 *   sub/  含 → { nested.txt(REG inline) }
 *   inline.txt 带一个 xattr 块（测 xattr 路径）
 * 条目名全部 ≤40B（内联，不进名字堆）。
 *
 * 用法： ./mkcontent <in.img> <out.img>
 * ═══════════════════════════════════════════════════════════════════════════ */
#include <disk_struct_v1.2.h>
#include <magics.h>
#include <xxhash.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static constexpr uint64_t BLK  = 4096;
static constexpr uint64_t NODE = 8192;
static constexpr uint64_t PTR52 = (1ULL << 52) - 1;
static constexpr uint64_t PTR_INV = PTR_INVALID;

/* FS_block b 的设备字节偏移 */
static inline uint64_t blkoff(uint64_t b) { return BLK * (b + 1); }

struct Img {
    int fd = -1;
    uint64_t total_blocks = 0, inode_count = 0, root_inode = 0;
    uint64_t bitmap_blocks=0, bitmap_start=0, inode_blocks=0, inode_array_base=0;
    uint64_t inode_bm_blocks=0, inode_bm_base=0;
    std::vector<uint8_t> bm;    /* block bitmap */
    std::vector<uint8_t> ibm;   /* inode bitmap */

    bool rd(void *buf, size_t n, uint64_t off) {
        uint8_t *p=(uint8_t*)buf; size_t d=0;
        while (d<n){ ssize_t r=pread(fd,p+d,n-d,(off_t)(off+d)); if(r<=0) return false; d+=(size_t)r; }
        return true;
    }
    bool wr(const void *buf, size_t n, uint64_t off) {
        const uint8_t *p=(const uint8_t*)buf; size_t d=0;
        while (d<n){ ssize_t r=pwrite(fd,p+d,n-d,(off_t)(off+d)); if(r<=0) return false; d+=(size_t)r; }
        return true;
    }
    bool blk_lit(uint64_t b) const { return (bm[b>>3]>>(b&7))&1; }
    bool ino_free(uint64_t i) const { return !((ibm[i>>3]>>(i&7))&1); }

    void mark_blk(uint64_t b) { bm[b>>3] |= (uint8_t)(1u<<(b&7)); }
    void mark_ino(uint64_t i) { ibm[i>>3] |= (uint8_t)(1u<<(i&7)); }

    uint64_t alloc_blocks(uint64_t n) {
        for (uint64_t b = 2; b + n <= total_blocks; b++) {
            bool ok = true;
            for (uint64_t k = 0; k < n; k++) if (blk_lit(b+k)) { ok = false; b += k; break; }
            if (ok) { for (uint64_t k = 0; k < n; k++) mark_blk(b+k); return b; }
        }
        fprintf(stderr, "mkcontent: no free %llu contiguous blocks\n", (unsigned long long)n);
        exit(1);
    }
    /* 节点块：偶数对齐、2 连续块 */
    uint64_t alloc_pair() { return alloc_blocks(2); }
    uint64_t alloc_inode() {
        for (uint64_t i = 17; i < inode_count; i++) if (ino_free(i)) { mark_ino(i); return i; }
        fprintf(stderr, "mkcontent: no free inode\n"); exit(1);
    }
    void write_inode(uint64_t idx, const void *ino, size_t n) {
        wr(ino, n, blkoff(inode_array_base) + idx * 256);
    }
};

struct Ent { std::string name; uint64_t inode; };
static void sort_ents(std::vector<Ent> &v) {
    std::sort(v.begin(), v.end(), [](const Ent&a, const Ent&b){
        uint64_t ha = (uint64_t)XXH64(a.name.data(), a.name.size(), 0);
        uint64_t hb = (uint64_t)XXH64(b.name.data(), b.name.size(), 0);
        if (ha != hb) return ha < hb;
        return a.name < b.name;
    });
}

static void build_dentry_node(std::vector<uint8_t> &node, uint64_t owner_inum,
                              uint64_t dotdot_inum, const std::vector<Ent> &ents,
                              uint64_t heap_data_base)
{
    node.assign(NODE, 0);
    auto *meta = (Bptree_meta_t *)node.data();
    meta->magics[0]=DENTRY_MAGIC1; meta->magics[1]=DENTRY_MAGIC2;
    meta->magics[2]=DENTRY_MAGIC3; meta->magics[3]=DENTRY_MAGIC4;
    meta->parent_ptr.v = RFS_PP_PACK(ents.size(), 0, 1, owner_inum);
    meta->ptrs[0].v = RFS_PTR_PACK(0, PTR52);
    meta->ptrs[1].v = RFS_PTR_PACK(0, dotdot_inum);
    meta->ptrs[2].v = RFS_PTR_PACK(0, PTR52);
    auto *leaf = (Bptree_ddentry_leaf_node_t *)node.data();
    for (size_t i = 0; i < ents.size(); i++) {
        ddentry_t de;
        memset(&de, 0, sizeof(de));
        de.inode_ptr.v = RFS_PTR_PACK(ents[i].name.size(), ents[i].inode);
        de.hash = (uint64_t)XXH64(ents[i].name.data(), ents[i].name.size(), 0);
        de.full_char_remain_ptr = (ents[i].name.size() > DENTRY_INLINE_NAME_MAX)
                                      ? ((heap_data_base << 12) | 0)
                                      : PTR_INV;
        size_t ncopy = ents[i].name.size() < DENTRY_INLINE_NAME_MAX ? ents[i].name.size() : (size_t)DENTRY_INLINE_NAME_MAX;
        memcpy(de.name, ents[i].name.data(), ncopy);
        memcpy(&leaf->entries[i], &de, sizeof(de));
    }
}

static void set_common(raven_Inode_t *ino, uint32_t type, uint32_t direct, uint64_t size,
                       uint64_t data_root, uint32_t link_count, uint64_t mode, uint64_t xattr) {
    memset(ino, 0, 256);
    ino->word1      = RFS_INO1_PACK(type, direct, 0);
    ino->uid        = 0;
    ino->gid        = 0;
    ino->link_count = link_count;
    ino->mode_xattr = RFS_MODE_XATTR_PACK(mode, xattr);
    ino->size       = size;
    ino->data_root  = data_root;
}
static void set_dir_heap_inactive(raven_dir_Inode_t *d) {
    for (int i = 0; i < RFS_DIR_HEAP_COUNT; i++)
        RFS_NHC_SET(*rfs_dir_heap(d, i), 0, 0, NAME_HEAP_INACTIVE, 0, NAME_HEAP_INACTIVE);
}

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) { fprintf(stderr, "usage: %s <in.img> <out.img> [long]\n", argv[0]); return 1; }
    /* 复制 */
    {
        FILE *fi = fopen(argv[1], "rb");
        if (!fi) { perror("open in"); return 1; }
        FILE *fo = fopen(argv[2], "wb");
        if (!fo) { perror("open out"); return 1; }
        char buf[1<<20]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fi)) > 0) fwrite(buf, 1, n, fo);
        fclose(fi); fclose(fo);
    }

    Img im; im.fd = open(argv[2], O_RDWR);
    if (im.fd < 0) { perror("open out O_RDWR"); return 1; }

    RavenSuperblock sb;
    if (!im.rd(&sb, sizeof(sb), 0)) return 1;
    im.total_blocks = sb.total_blocks;
    im.inode_count  = sb.inode_array_count;
    im.root_inode   = sb.root_inode;
    uint64_t bb=(im.total_blocks+32767)/32768; im.bitmap_blocks=bb; im.bitmap_start=im.total_blocks-bb;
    uint64_t ib=(im.inode_count*256+4095)/4096; im.inode_blocks=ib; im.inode_array_base=im.bitmap_start-ib;
    uint64_t ibb=(im.inode_count+32767)/32768; im.inode_bm_blocks=ibb; im.inode_bm_base=im.inode_array_base-ibb;

    im.bm.assign((size_t)im.bitmap_blocks*BLK, 0);
    im.ibm.assign((size_t)im.inode_bm_blocks*BLK, 0);
    im.rd(im.bm.data(), im.bm.size(), blkoff(im.bitmap_start));
    im.rd(im.ibm.data(), im.ibm.size(), blkoff(im.inode_bm_base));

    /* ── 分配 inode ── */
    uint64_t i_sub   = im.alloc_inode();
    uint64_t i_txt   = im.alloc_inode();
    uint64_t i_lnk   = im.alloc_inode();
    uint64_t i_fifo  = im.alloc_inode();
    uint64_t i_sock  = im.alloc_inode();
    uint64_t i_chr   = im.alloc_inode();
    uint64_t i_dir   = im.alloc_inode();
    uint64_t i_ext   = im.alloc_inode();
    uint64_t i_nest  = im.alloc_inode();

    /* ── 分配块 ── */
    uint64_t blk_sub   = im.alloc_pair();          /* sub dentry 节点 */
    uint64_t blk_dir   = im.alloc_blocks(2);       /* direct.bin 数据 */
    uint64_t blk_extn  = im.alloc_pair();          /* extent 树根叶节点 */
    uint64_t blk_extd  = im.alloc_blocks(3);       /* extent.bin 数据 */
    uint64_t blk_xattr = im.alloc_blocks(1);       /* inline.txt xattr 块 */

    /* 可选：追加一个 >40B 长名条目（激活 root 名字堆 heaps[0]） */
    bool add_long = (argc == 4 && strcmp(argv[3], "long") == 0);
    uint64_t i_long = 0, blk_hb = 0, blk_hd = 0;
    std::string longname;
    if (add_long) {
        i_long  = im.alloc_inode();
        blk_hb  = im.alloc_pair();        /* 堆位图 2 blocks（8B 粒度 × 16 bits log2） */
        blk_hd  = im.alloc_blocks(128);   /* 堆数据 128 blocks */
        longname = "a_very_long_name_that_is_definitely_over_forty_bytes.dat";
    }

    printf("alloc: sub_ino=%llu txt=%llu lnk=%llu fifo=%llu sock=%llu chr=%llu dir=%llu ext=%llu nest=%llu\n",
           (unsigned long long)i_sub,(unsigned long long)i_txt,(unsigned long long)i_lnk,
           (unsigned long long)i_fifo,(unsigned long long)i_sock,(unsigned long long)i_chr,
           (unsigned long long)i_dir,(unsigned long long)i_ext,(unsigned long long)i_nest);
    printf("alloc blocks: sub=%llu dir=%llu extn=%llu extd=%llu xattr=%llu\n",
           (unsigned long long)blk_sub,(unsigned long long)blk_dir,(unsigned long long)blk_extn,
           (unsigned long long)blk_extd,(unsigned long long)blk_xattr);

    /* ── root 目录条目 ── */
    std::vector<Ent> root_ents = {
        {"sub", i_sub}, {"inline.txt", i_txt}, {"link", i_lnk}, {"fifo", i_fifo},
        {"sock", i_sock}, {"chardev", i_chr}, {"direct.bin", i_dir}, {"extent.bin", i_ext}
    };
    if (add_long) root_ents.push_back({longname, i_long});
    sort_ents(root_ents);
    std::vector<uint8_t> root_node;
    build_dentry_node(root_node, im.root_inode, im.root_inode, root_ents, blk_hd);
    im.wr(root_node.data(), root_node.size(), blkoff(0));

    /* ── sub 目录条目 ── */
    std::vector<Ent> sub_ents = { {"nested.txt", i_nest} };
    sort_ents(sub_ents);
    std::vector<uint8_t> sub_node;
    build_dentry_node(sub_node, i_sub, im.root_inode, sub_ents, 0);
    im.wr(sub_node.data(), sub_node.size(), blkoff(blk_sub));

    /* ── root inode（16）改写 size/link_count ── */
    {
        raven_dir_Inode_t d;
        im.rd(&d, 256, blkoff(im.inode_array_base) + im.root_inode*256);
        d.word1      = RFS_INO1_PACK(INODE_TYPE_DIR, 0, 0);
        d.size       = root_ents.size();
        d.link_count = (uint32_t)(root_ents.size() + 2);
        d.data_root  = 0;
        d.mode_xattr = RFS_MODE_XATTR_PACK(0755, RFS_XATTR_NONE);
        set_dir_heap_inactive(&d);
        if (add_long) {
            /* 激活 heaps_8B_8k = 8B 粒度 / bits_log2=16（位图 2 块、数据 128 块） */
            RFS_NHC_SET(*rfs_dir_heap(&d, 0), 3, 16, blk_hb, 0, blk_hd);
        }
        im.wr(&d, 256, blkoff(im.inode_array_base) + im.root_inode*256);
    }

    /* ── sub inode ── */
    {
        raven_dir_Inode_t d;
        memset(&d, 0, 256);
        d.word1      = RFS_INO1_PACK(INODE_TYPE_DIR, 0, 0);
        d.link_count = (uint32_t)(sub_ents.size() + 2);
        d.mode_xattr = RFS_MODE_XATTR_PACK(0750, RFS_XATTR_NONE);
        d.size       = sub_ents.size();
        d.data_root  = blk_sub;
        set_dir_heap_inactive(&d);
        im.write_inode(i_sub, &d, 256);
    }

    /* ── inline.txt（REG inline + xattr）── */
    {
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 0, 11, PTR_INV, 1, 0644, blk_xattr);
        memcpy(ino.inline_data, "hello world", 11);
        im.write_inode(i_txt, &ino, 256);
        /* xattr 块（**ext4 镜像**：条目区从 32 增长；值区从块尾向下；首个 u32==0 终止） */
        std::vector<uint8_t> xb(BLK, 0);
        auto *h = (rfs_xattr_block_hdr *)xb.data();
        h->h_magic = RFS_XATTR_H_MAGIC; h->h_refcount = 1; h->h_blocks = 1; h->h_hash = 0;
        auto *xe = (rfs_xattr_entry *)(xb.data() + RFS_XATTR_HDR_SIZE);
        xe->e_name_len   = 3;                               /* "abc" */
        xe->e_name_index = RFS_XATTR_INDEX_USER;
        xe->e_value_size = 3;                               /* "xyz" */
        xe->e_value_offs = (uint16_t)((BLK - 3) & ~3u);     /* 4092（4B 对齐） */
        memcpy(xb.data() + RFS_XATTR_HDR_SIZE + RFS_XATTR_ENTRY_HDR_SIZE, "abc", 3);
        memcpy(xb.data() + xe->e_value_offs, "xyz", 3);
        /* 条目区终止：off 48 起保持 0（首个 u32==0） */
        im.wr(xb.data(), xb.size(), blkoff(blk_xattr));
    }

    /* ── link（LNK inline）── */
    {
        raven_Inode_t ino;
        const char *tgt = "inline.txt";
        set_common(&ino, INODE_TYPE_SOFT_SYM, 0, strlen(tgt), PTR_INV, 1, 0777, RFS_XATTR_NONE);
        memcpy(ino.inline_data, tgt, strlen(tgt));
        im.write_inode(i_lnk, &ino, 256);
    }

    /* ── fifo / sock ── */
    {
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_FIFO, 0, 0, PTR_INV, 1, 0644, RFS_XATTR_NONE);
        im.write_inode(i_fifo, &ino, 256);
        set_common(&ino, INODE_TYPE_SOCK, 0, 0, PTR_INV, 1, 0644, RFS_XATTR_NONE);
        im.write_inode(i_sock, &ino, 256);
    }

    /* ── chardev（CHR, rdev major=1 minor=3 → new_encode_dev = 0x103）── */
    {
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_CHR, 0, 0, PTR_INV, 1, 0666, RFS_XATTR_NONE);
        uint32_t rdev = (1u << 8) | 3u;
        memcpy(ino.inline_data, &rdev, 4);
        im.write_inode(i_chr, &ino, 256);
        printf("chardev rdev=0x%x\n", rdev);
    }

    /* ── direct.bin（REG direct, size=5000）── */
    {
        uint64_t sz = 5000;
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 1, sz, blk_dir, 1, 0644, RFS_XATTR_NONE);
        im.write_inode(i_dir, &ino, 256);
        std::vector<uint8_t> data(blkoff(blk_dir+2)-blkoff(blk_dir), 0);
        for (size_t i = 0; i < data.size(); i++) data[i] = (uint8_t)('A' + (i % 26));
        im.wr(data.data(), data.size(), blkoff(blk_dir));
    }

    /* ── extent.bin（REG extent, size=9000, 单条 extent 覆盖 [0,3)）── */
    {
        uint64_t sz = 9000;
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 0, sz, blk_extn, 1, 0644, RFS_XATTR_NONE);
        im.write_inode(i_ext, &ino, 256);
        /* 根叶节点 */
        std::vector<uint8_t> node(NODE, 0);
        auto *meta = (Bptree_meta_t *)node.data();
        meta->magics[0]=BPTREE_MAGIC1; meta->magics[1]=BPTREE_MAGIC2;
        meta->magics[2]=BPTREE_MAGIC3; meta->magics[3]=BPTREE_MAGIC4;
        meta->parent_ptr.v = RFS_PP_PACK(1, 0, 1, i_ext);
        meta->ptrs[0].v = RFS_PTR_PACK(0, PTR52);
        meta->ptrs[1].v = RFS_PTR_PACK(0, PTR52);
        meta->ptrs[2].v = RFS_PTR_PACK(0, PTR52);
        auto *leaf = (Bptree_extent_leaf_node_t *)node.data();
        leaf->extents[0].length    = 3;
        leaf->extents[0].fblkbase  = 0;
        leaf->extents[0].block_base.v = RFS_PTR_PACK(0, blk_extd);
        im.wr(node.data(), node.size(), blkoff(blk_extn));
        /* 数据 */
        std::vector<uint8_t> data(blkoff(blk_extd+3)-blkoff(blk_extd), 0);
        for (size_t i = 0; i < data.size(); i++) data[i] = (uint8_t)('a' + (i % 26));
        im.wr(data.data(), data.size(), blkoff(blk_extd));
    }

    /* ── nested.txt（sub 内 REG inline）── */
    {
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 0, 7, PTR_INV, 1, 0600, RFS_XATTR_NONE);
        memcpy(ino.inline_data, "nested!", 7);
        im.write_inode(i_nest, &ino, 256);
    }

    /* ── 长名（可选）：写 long inode + 堆位图 + 堆数据 ── */
    if (add_long) {
        raven_Inode_t ino;
        set_common(&ino, INODE_TYPE_REGULAR, 0, 9, PTR_INV, 1, 0644, RFS_XATTR_NONE);
        memcpy(ino.inline_data, "long-file", 9);
        im.write_inode(i_long, &ino, 256);

        uint16_t nlen = (uint16_t)longname.size();
        uint16_t tail = (uint16_t)(nlen - DENTRY_INLINE_NAME_MAX);
        uint16_t units = (uint16_t)((tail + 7) / 8);
        std::vector<uint8_t> hbm((size_t)2 * BLK, 0);
        for (uint16_t u = 0; u < units; u++) hbm[u >> 3] |= (uint8_t)(1u << (u & 7));
        im.wr(hbm.data(), hbm.size(), blkoff(blk_hb));
        im.wr(longname.data() + DENTRY_INLINE_NAME_MAX, tail, blkoff(blk_hd));
        printf("long name: \"%s\" (len=%u tail=%u units=%u) heap bmp=%llu data=%llu\n",
               longname.c_str(), nlen, tail, units, (unsigned long long)blk_hb, (unsigned long long)blk_hd);
    }

    /* ── 落盘位图 ── */    im.wr(im.bm.data(),  im.bm.size(),  blkoff(im.bitmap_start));
    im.wr(im.ibm.data(), im.ibm.size(), blkoff(im.inode_bm_base));

    close(im.fd);
    printf("mkcontent: done → %s\n", argv[2]);
    return 0;
}
