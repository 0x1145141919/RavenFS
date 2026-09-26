#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
check_super.py —— RavenFS v1.2 盘面只读校验器（用户态；M1 期用于无 root 验证 ko 的解析逻辑）

用法:  python3 check_super.py <image-or-device>

校验内容（与 linux/super.c 的 ravenfs_parse_superblock / ravenfs_read_inode 逐式同构）：
  1. Primary SB @ 设备字节 0：magic=SUPERBLOCK_MAGIC("RAVN") / version==3 / crc32
  2. 几何推导（与 mkfs.ravenfs calc_geometry 对齐）
  3. root inode 解码：type / mode / link_count / size / data_root / xattr_ptr 哨兵
"""
import struct
import sys
import zlib

BLOCK = 4096
BITS_PER_BLOCK = 32768
INODES_PER_BLOCK = 16
INODE_SIZE = 256
SB_MAGIC = 0x5241564E  # "RAVN"
EXPECT_VER = 3
XATTR_NONE = (1 << 52) - 1
PTR_INVALID = (1 << 64) - 1
TYPES = {1: "FIFO", 2: "CHR", 4: "DIR", 6: "BLK", 8: "REG", 10: "LNK", 12: "SOCK"}


def main(path):
    with open(path, "rb") as f:
        sb = f.read(BLOCK)
        magic, ver = struct.unpack_from("<QI", sb, 0)
        wbl, total, iac, root_ino = struct.unpack_from("<QQQQ", sb, 16)
        uuid = sb[48:64]
        crc_stored = struct.unpack_from("<I", sb, 112)[0]
        tmp = bytearray(sb)
        tmp[112:116] = b"\0\0\0\0"
        crc_calc = zlib.crc32(bytes(tmp)) & 0xFFFFFFFF

        print(f"image      : {path}")
        print(f"magic      : 0x{magic:08X} ({'OK' if magic == SB_MAGIC else 'BAD'})")
        print(f"version    : {ver} ({'OK' if ver == EXPECT_VER else 'BAD'})")
        print(f"crc32      : stored 0x{crc_stored:08X} calc 0x{crc_calc:08X} "
              f"({'OK' if crc_stored == crc_calc else 'BAD'})")
        print(f"world_base_lba={wbl}  blk0 byte offset = {wbl * BLOCK} "
              f"({'OK' if wbl * BLOCK == BLOCK else 'BAD'})")
        print(f"total_blocks={total}  inode_array_count={iac}  root_inode={root_ino}  "
              f"uuid={uuid.hex()}")

        bb = (total + BITS_PER_BLOCK - 1) // BITS_PER_BLOCK
        ib = (iac * INODE_SIZE + BLOCK - 1) // BLOCK
        ibb = (iac + BITS_PER_BLOCK - 1) // BITS_PER_BLOCK
        bitmap_start = total - bb
        inode_array_base = bitmap_start - ib
        inode_bm_base = inode_array_base - ibb
        print(f"geometry   : bitmap[{bitmap_start},{total})  "
              f"inode_array[{inode_array_base},{bitmap_start})  "
              f"inode_bm[{inode_bm_base},{inode_array_base})")

        # root inode
        fs_blk = inode_array_base + root_ino // INODES_PER_BLOCK
        off = (root_ino % INODES_PER_BLOCK) * INODE_SIZE
        f.seek(BLOCK * (fs_blk + 1) + off)
        raw = f.read(INODE_SIZE)
        word1, uid, gid, mns, cns, ans, link = struct.unpack_from("<QIIIIII", raw, 0)
        mode_xattr, size, mtime, ctime, atime, data_root = struct.unpack_from("<QQQQQQ", raw, 32)
        typ = word1 & 0x3F
        direct = (word1 >> 6) & 1
        i_flags = (word1 >> 7) & ((1 << 57) - 1)
        mode = mode_xattr & 0xFFF
        xattr_ptr = (mode_xattr >> 12) & XATTR_NONE
        print(f"root inode : idx={root_ino} @ FS_block {fs_blk} +{off}")
        print(f"  type     : {typ} ({TYPES.get(typ, 'INVALID')})  direct={direct}  i_flags={i_flags}")
        print(f"  i_mode   : 0o{(typ << 12) | mode:o} (type<<12 | mode=0o{mode:o})")
        print(f"  nlink    : {link}   size={size}   data_root=0x{data_root:X}"
              f"{' (PTR_INVALID)' if data_root == PTR_INVALID else ''}")
        print(f"  xattr_ptr: {'NONE(sentinel)' if xattr_ptr == XATTR_NONE else hex(xattr_ptr)}")
        print(f"  uid/gid  : {uid}/{gid}   times m={mtime}.{mns} c={ctime}.{cns} a={atime}.{ans}")

        ok = (magic == SB_MAGIC and ver == EXPECT_VER and crc_stored == crc_calc
              and typ in TYPES and data_root != PTR_INVALID)
        print(f"VERDICT    : {'PASS' if ok else 'FAIL'}")
        return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
