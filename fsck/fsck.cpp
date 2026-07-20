#include "fsck.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <unistd.h>
#include <fcntl.h>

#include <uuid/uuid.h>

#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#ifndef BLKSSZGET
#define BLKSSZGET  _IO(0x12, 104)
#endif

/* ═══════════════════════════════════════════════════════════════
 * LBA_size 探测（ioctl）
 * ═══════════════════════════════════════════════════════════════ */
uint64_t fsck_probe_lba_size(int fd) {
    // 优先尝试 ioctl
    int sz = 0;
    if (ioctl(fd, BLKSSZGET, &sz) == 0 && sz > 0) {
        if (4096 % (unsigned)sz != 0) {
            fprintf(stderr, "error: LBA_size %d does not divide 4096\n", sz);
            return 0;
        }
        return (uint64_t)sz;
    }

    // ioctl 失败 → 检查是否是普通文件
    struct stat st;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
        return 4096;

    fprintf(stderr, "error: BLKSSZGET failed (not a block device?)\n");
    return 0;
}

#define PRINT_FIELD(label, fmt, ...) \
    do { \
        fprintf(stdout, "  %-24s ", label); \
        fprintf(stdout, fmt, ##__VA_ARGS__); \
        fprintf(stdout, "\n"); \
    } while (0)

#define PRINT_RAW(...) \
    do { fprintf(stdout, __VA_ARGS__); fprintf(stdout, "\n"); } while (0)

FsGeometry derive_geometry(uint64_t total_blocks, uint64_t inode_count) {
    FsGeometry g{};
    g.total_blocks        = total_blocks;
    g.bitmap_blocks       = (total_blocks + 32767) / 32768;
    g.bitmap_start        = total_blocks - g.bitmap_blocks;
    g.inode_blocks        = (inode_count * 256 + 4095) / 4096;
    g.inode_array_base    = g.bitmap_start - g.inode_blocks;
    g.inode_bitmap_blocks = (inode_count + 32767) / 32768;
    g.inode_array_bitmap_base = g.inode_array_base - g.inode_bitmap_blocks;
    return g;
}

static void print_sep() {
    PRINT_RAW("%s", "────────────────────────────────────────────");
}

static void print_sec(const char *title) {
    PRINT_RAW("%s", "");
    PRINT_RAW("%s", title);
    print_sep();
}

/* ═══════════════════════════════════════════════════════════════
 * Superblock
 * ═══════════════════════════════════════════════════════════════ */
static void dump_superblock(const RavenSuperblock *sb, uint64_t lba_size) {
    print_sec("Primary Superblock");

    char magic_str[9] = {};
    memcpy(magic_str, &sb->magic, 8);
    PRINT_FIELD("magic",           "%s",      magic_str);
    PRINT_FIELD("version",         "%u",      sb->version);
    PRINT_FIELD("world_base_lba",  "%lu",     (unsigned long)sb->world_base_lba);
    if (lba_size > 0)
        PRINT_FIELD("lba_size (probed)","%lu", (unsigned long)lba_size);
    PRINT_FIELD("total_blocks",    "%lu",     (unsigned long)sb->total_blocks);
    PRINT_FIELD("inode_count",     "%lu",     (unsigned long)sb->inode_array_count);
    PRINT_FIELD("root_inode",      "%lu",     (unsigned long)sb->root_inode);

    char uuid_str[37];
    uuid_unparse(sb->uuid, uuid_str);
    PRINT_FIELD("uuid",            "%s",      uuid_str);

    char vname[49] = {};
    memcpy(vname, sb->volume_name, 48);
    PRINT_FIELD("volume_name",     "%s",      vname);

    PRINT_FIELD("backup_prev",     "%lu",     (unsigned long)sb->backup_prev);
    PRINT_FIELD("backup_next",     "%lu",     (unsigned long)sb->backup_next);
    PRINT_FIELD("backup_count",    "%u",      sb->backup_count);
}

/* ═══════════════════════════════════════════════════════════════
 * Layout
 * ═══════════════════════════════════════════════════════════════ */
static void dump_layout(const FsGeometry &g, uint64_t inode_count) {
    print_sec("Derived Layout");
    PRINT_FIELD("bitmap",          "[%lu, %lu)  %lu blocks",
                (unsigned long)g.bitmap_start,
                (unsigned long)g.total_blocks,
                (unsigned long)g.bitmap_blocks);
    PRINT_FIELD("inode array",     "[%lu, %lu)  %lu blocks",
                (unsigned long)g.inode_array_base,
                (unsigned long)g.bitmap_start,
                (unsigned long)g.inode_blocks);
    PRINT_FIELD("inode bitmap",    "[%lu, %lu)  %lu blocks",
                (unsigned long)g.inode_array_bitmap_base,
                (unsigned long)g.inode_array_base,
                (unsigned long)g.inode_bitmap_blocks);

    uint64_t meta = g.bitmap_blocks + g.inode_blocks + g.inode_bitmap_blocks;
    uint64_t data_blocks = g.total_blocks - meta;
    PRINT_FIELD("metadata total",  "%lu",     (unsigned long)meta);
    PRINT_FIELD("data capacity",   "%lu blocks (%.2f%%)",
                (unsigned long)data_blocks,
                (double)data_blocks / g.total_blocks * 100);

    double blk_per_inode = (double)data_blocks / inode_count;
    double kb_per_inode  = blk_per_inode * 4;
    PRINT_FIELD("blocks / inode",  "%.1f  (%.0f KB / inode)",
                blk_per_inode, kb_per_inode);
}

/* ─── FS_block byte offset helper ─── */
// FS_block[n] byte offset = world_base_lba * lba_size + n * 4096.
// For new-format FS (world_base_lba != 0): world_base_lba * lba_size = 4096
//   (Primary occupies byte 0-4095, FS_block world starts at 4096)
// For old-format FS (world_base_lba == 0): world_base_lba * lba_size = 0
//   (buggy layout, Primary overlaps FS_block[0])
static uint64_t blk_off(uint64_t world_base_lba, uint64_t block) {
    uint64_t base = (world_base_lba == 0) ? 0 : 4096;
    return base + block * 4096;
}

/* ═══════════════════════════════════════════════════════════════
 * Backup chain
 * ═══════════════════════════════════════════════════════════════ */
static int dump_backups(int fd, const RavenSuperblock *sb, bool verbose) {
    print_sec("Backup Chain");
    int errors = 0;
    int count = 0;
    uint64_t prev_block = 0;
    uint64_t next = sb->backup_next;

    while (next != 0 && count < 256) {
        RavenSuperblock bs;
        uint64_t off = blk_off(sb->world_base_lba, next);
        if (pread(fd, &bs, sizeof(bs), off) != (ssize_t)sizeof(bs)) {
            PRINT_FIELD("ERROR", "read backup at block %lu",
                        (unsigned long)next);
            return ++errors;
        }

        if (bs.magic != SUPERBLOCK_MAGIC) {
            PRINT_FIELD("ERROR", "bad magic at block %lu (got 0x%lx)",
                        (unsigned long)next, (unsigned long)bs.magic);
            return ++errors;
        }

        if (bs.backup_prev != prev_block) {
            PRINT_FIELD("ERROR", "prev link mismatch at block %lu: "
                       "expected %lu, got %lu",
                       (unsigned long)next,
                       (unsigned long)prev_block,
                       (unsigned long)bs.backup_prev);
            errors++;
        }

        if (bs.backup_count != sb->backup_count) {
            PRINT_FIELD("ERROR", "backup_count mismatch at block %lu: "
                       "%u != %u",
                       (unsigned long)next,
                       bs.backup_count, sb->backup_count);
            errors++;
        }

        char magic_str[9] = {};
        memcpy(magic_str, &bs.magic, 8);
        char label[32];
        snprintf(label, sizeof(label), "Backup #%d", count);
        PRINT_FIELD(label, "block %-10lu  magic=%s",
                    (unsigned long)next, magic_str);

        if (verbose) {
            PRINT_FIELD("  prev",     "%lu", (unsigned long)bs.backup_prev);
            PRINT_FIELD("  next",     "%lu", (unsigned long)bs.backup_next);
        }

        prev_block = next;
        next = bs.backup_next;
        count++;
    }

    if (count == 0) {
        PRINT_FIELD("(none)", "%s", "");
    } else {
        PRINT_FIELD("verified",     "%d / %u",
                    count, sb->backup_count);
    }

    if (count != (int)sb->backup_count) {
        PRINT_FIELD("WARNING",      "traversed %d, superblock claims %u",
                    count, sb->backup_count);
        errors++;
    }

    return errors;
}

/* ═══════════════════════════════════════════════════════════════
 * Bitmap
 * ═══════════════════════════════════════════════════════════════ */
static void dump_bitmap(int fd, const FsGeometry &g, bool verbose,
                         uint64_t world_base_lba) {
    print_sec("Bitmap");

    uint64_t bytes = g.bitmap_blocks * 4096;
    uint8_t *bm = (uint8_t *)malloc(bytes);
    if (!bm) { PRINT_FIELD("ERROR", "alloc failed"); return; }

    if (pread(fd, bm, bytes, blk_off(world_base_lba, g.bitmap_start)) != (ssize_t)bytes) {
        PRINT_FIELD("ERROR", "read bitmap");
        free(bm);
        return;
    }

    uint64_t n_alloc = 0, n_free = 0;
    uint64_t tail_errs = 0;

    for (uint64_t i = 0; i < g.total_blocks; i++) {
        if (bm[i / 8] & (1 << (i % 8))) n_alloc++;
        else n_free++;
    }

    uint64_t total_bits = g.bitmap_blocks * 32768;
    for (uint64_t i = g.total_blocks; i < total_bits; i++) {
        if (!(bm[i / 8] & (1 << (i % 8)))) tail_errs++;
    }

    PRINT_FIELD("total blocks",    "%lu",     (unsigned long)g.total_blocks);
    PRINT_FIELD("allocated",       "%lu",     (unsigned long)n_alloc);
    PRINT_FIELD("free",            "%lu",     (unsigned long)n_free);
    if (tail_errs)
        PRINT_FIELD("TAIL ERRORS",  "%lu bits not set", (unsigned long)tail_errs);
    else
        PRINT_FIELD("tail bits",    "%lu (all 1)", (unsigned long)(total_bits - g.total_blocks));

    if (verbose && n_free > 0) {
        PRINT_FIELD("free ranges", "%s", "");
        int nr = 0;
        bool in = false;
        uint64_t rs = 0;
        for (uint64_t i = 0; i < g.total_blocks && nr < 10; i++) {
            bool set = (bm[i / 8] & (1 << (i % 8))) != 0;
            if (!set && !in) { in = true; rs = i; }
            else if (set && in) {
                char buf[48];
                snprintf(buf, sizeof(buf), "[%lu, %lu)", (unsigned long)rs, (unsigned long)i);
                PRINT_FIELD(buf, "%lu blocks", (unsigned long)(i - rs));
                in = false; nr++;
            }
        }
        if (in) {
            char buf[48];
            snprintf(buf, sizeof(buf), "[%lu, %lu)", (unsigned long)rs, (unsigned long)g.total_blocks);
            PRINT_FIELD(buf, "%lu blocks", (unsigned long)(g.total_blocks - rs));
        }
    }

    free(bm);
}

/* ═══════════════════════════════════════════════════════════════
 * Root Inode
 * ═══════════════════════════════════════════════════════════════ */
static raven_Inode_t read_inode(int fd, uint64_t idx, const FsGeometry &g,
                                 uint64_t world_base_lba) {
    raven_Inode_t ino = {};
    uint64_t off = blk_off(world_base_lba, g.inode_array_base) + idx * 256;
    pread(fd, &ino, sizeof(ino), off);
    return ino;
}

static void dump_root_inode(int fd, const RavenSuperblock *sb, const FsGeometry &g) {
    print_sec("Root Inode");

    raven_Inode_t ino = read_inode(fd, sb->root_inode, g, sb->world_base_lba);

    const char *t = "?";
    if (ino.type == INODE_TYPE_REGULAR)   t = "REGULAR";
    else if (ino.type == INODE_TYPE_DIR)  t = "DIR";
    else if (ino.type == INODE_TYPE_SOFT_LINK) t = "SOFT_LINK";

    PRINT_FIELD("inode",           "%lu",     (unsigned long)sb->root_inode);
    PRINT_FIELD("type",            "%s",      t);
    PRINT_FIELD("data_root",       "%lu (%s)",
                (unsigned long)ino.data_root,
                ino.data_root == 0 ? "inline" :
                    ino.is_data_root_direct_bit ? "direct" : "B+tree");
    PRINT_FIELD("size",            "%lu",     (unsigned long)ino.size);
    PRINT_FIELD("link_count",      "%u",      ino.link_count);
    PRINT_FIELD("uid/gid",         "%u / %u", ino.uid, ino.gid);
    PRINT_FIELD("permission",      "0%o",     (unsigned)ino.permission_bits);
}

/* ═══════════════════════════════════════════════════════════════
 * Root Dentry
 * ═══════════════════════════════════════════════════════════════ */
static void dump_root_dentry(int fd, const RavenSuperblock *sb,
                               const FsGeometry &g) {
    raven_Inode_t ino = read_inode(fd, sb->root_inode, g, sb->world_base_lba);
    uint64_t data_root = ino.data_root;

    if (data_root == 0) {
        PRINT_FIELD("dentry tree",  "(inline, none)");
        return;
    }

    Dentry_Bptree_node_t node;
    if (pread(fd, &node, sizeof(node), blk_off(sb->world_base_lba, data_root)) != sizeof(node)) {
        PRINT_FIELD("ERROR",        "read dentry node at block %lu",
                    (unsigned long)data_root);
        return;
    }

    print_sec("Root Dentry B+tree");

    PRINT_FIELD("block",           "%lu",     (unsigned long)data_root);
    uint8_t ec = node.head.head.fields.entry_count;
    PRINT_FIELD("entry_count",     "%u",      ec + 1);
    PRINT_FIELD("is_internal",     "%s",      node.head.head.fields.is_internal_node ? "yes" : "no");
    PRINT_FIELD("is_root",         "%s",      node.head.head.fields.is_root_node ? "yes" : "no");
    PRINT_FIELD("ptr1/ptr2",       "%lu / %lu",
                (unsigned long)node.head.ptr1,
                (unsigned long)node.head.ptr2);

    if (!node.head.head.fields.is_internal_node) {
        print_sec("  Directory entries");
        for (int i = 0; i <= ec && i < 63; i++) {
            char name[121] = {};
            memcpy(name, node.entries[i].name, 120);
            char lb[32];
            snprintf(lb, sizeof(lb), "entry[%d]", i);
            PRINT_FIELD(lb,         "inode=%-5lu  \"%s\"",
                        (unsigned long)node.entries[i].ptr, name);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
 * Entry point
 * ═══════════════════════════════════════════════════════════════ */
int fsck_dump(int fd, bool verbose, uint64_t lba_size) {
    int errors = 0;

    RavenSuperblock sb;
    if (pread(fd, &sb, sizeof(sb), 0) != (ssize_t)sizeof(sb)) {
        fprintf(stderr, "ERROR: cannot read Primary Superblock\n");
        return 1;
    }

    if (sb.magic != SUPERBLOCK_MAGIC) {
        fprintf(stderr, "ERROR: no RavenFS superblock (magic=0x%lx)\n",
                (unsigned long)sb.magic);
        return 1;
    }

    dump_superblock(&sb, lba_size);

    FsGeometry g = derive_geometry(sb.total_blocks, sb.inode_array_count);
    dump_layout(g, sb.inode_array_count);
    errors += dump_backups(fd, &sb, verbose);
    dump_bitmap(fd, g, verbose, sb.world_base_lba);
    dump_root_inode(fd, &sb, g);
    dump_root_dentry(fd, &sb, g);

    print_sec("Result");
    if (errors)
        PRINT_FIELD("ERRORS",       "%d found", errors);
    else
        PRINT_RAW("  No errors.\n");

    return errors ? 1 : 0;
}
