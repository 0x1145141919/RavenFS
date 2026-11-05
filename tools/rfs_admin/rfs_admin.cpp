/* ═══════════════════════════════════════════════════════════════════
 * rfs_admin.cpp — RavenFS 离线镜像运维工具（免 sudo / 免 mount）
 *
 * 直接对镜像文件（普通 file；块设备同样可只读）用上帝类 fd 挂载后端做
 * DFS 浏览 / 读写 / 递归复制 / 删除 / 空间统计 / 审计 / .vmcore 检查。
 *
 * 纪律：
 *   · 不改 juvenile_god 核心；私有接口一律借 `#define private public` 访问
 *     （与 tests/test_file_compact.cpp 同一手法）。
 *   · 普通文件 + mmap 后端（is_device_direct=false）；不 mount / losetup。
 *   · 只读命令以 is_readonly=true 挂载（MAP_PRIVATE），析构不刷盘；
 *     写命令正常析构 = all_sycn + 位图回写 + SB 时间戳（唯一提交点）。
 *
 * 目录枚举说明：root_dir_readdir / dir_readdir_interval 只有声明没有实现，
 * 本工具自行按 dentry B+tree 叶子链 DFS 枚举（根叶不跟 next）。
 * ═══════════════════════════════════════════════════════════════════ */
#define private public
#include "ravenfs/ravenfs_juvenile.hpp"
#undef private
#include "ravenfs/inode_cache_radix.hpp"
#include "ravenfs/mdata_piece_caches.hpp"
#include "ravenfs/ravenfs_audit.hpp"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#endif

/* ═══════════════════════════════════════════════════════════════════
 * 全局小工具
 * ═══════════════════════════════════════════════════════════════════ */
static bool g_json = false;
static bool g_readonly_flag = false;

/* ── 可选的“身份”建模（--uid/--gid）────────────────────────────────
 * 默认不启用：uid=gid=0（root），**不做任何权限检查**，行为与历史版本
 * 逐字节一致——运维工具的第一性原则是“离线镜像上无阻运维”。
 * 一旦显式给出 --uid/--gid，则按 Linux VFS 的 DAC 规则（属主/属组/other
 * 三段 rwx + root 旁路）检查每一次 inode 访问，并按该身份铸新 inode 的
 * uid/gid。这是可选的多用户语义层，不是安全边界：离线镜像的物理持有者
 * 永远可以直接改盘——它只负责让权限位在真实盘面上真正起作用。 */
static bool     g_cred_enforce = false;
static uint32_t g_cred_uid = 0;
static uint32_t g_cred_gid = 0;

static constexpr uint64_t kChunk = 1ULL << 20;          /* 1 MiB 流式块 */
static constexpr uint64_t kPtrInvalid52 = (uint64_t)PTR_INVALID & ((1ULL << 52) - 1);

static const char *rc_name(result_code_t rc)
{
    switch (rc) {
    case RC_OK:                 return "RC_OK";
    case RC_ERR_OPEN:           return "RC_ERR_OPEN";
    case RC_ERR_MAGIC:          return "RC_ERR_MAGIC";
    case RC_ERR_VERSION:        return "RC_ERR_VERSION";
    case RC_ERR_CRC:            return "RC_ERR_CRC";
    case RC_ERR_BITMAP_SELF:    return "RC_ERR_BITMAP_SELF";
    case RC_ERR_EXIST:          return "RC_ERR_EXIST";
    case RC_ERR_NOT_FOUND:      return "RC_ERR_NOT_FOUND";
    case RC_ERR_NO_BLOCK:       return "RC_ERR_NO_BLOCK";
    case RC_ERR_NO_INODE:       return "RC_ERR_NO_INODE";
    case RC_ERR_NAME_LEN:       return "RC_ERR_NAME_LEN";
    case RC_ERR_IO:             return "RC_ERR_IO";
    case RC_ERR_STATE:          return "RC_ERR_STATE";
    case RC_ERR_NOT_EMPTY:      return "RC_ERR_NOT_EMPTY";
    case RC_ERR_NOT_SUPPORTED:  return "RC_ERR_NOT_SUPPORTED";
    case RC_ERR_OVERLAP:        return "RC_ERR_OVERLAP";
    case RC_ERR_IS_DIR:         return "RC_ERR_IS_DIR";
    case RC_ERR_XATTR:          return "RC_ERR_XATTR";
    case RC_ERR_BUSY:           return "RC_ERR_BUSY";
    case RC_ERR_NAME_INVALID:   return "RC_ERR_NAME_INVALID";
    default:                    return "RC_???";
    }
}

static void errf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::fputs("rfs_admin: ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}

static bool ptr_invalid(uint64_t p) { return p == kPtrInvalid52 || p == PTR_INVALID; }

[[noreturn]] static void usage(int code)
{
    std::printf(
        "用法: rfs_admin <image> <command> [args...]\n"
        "\n"
        "全局: --readonly  强制只读挂载（写命令会拒绝）\n"
        "      --json      机器可读输出（df/info/ls/stat/hash/audit/vmcore/find）\n"
        "      --uid N     以 uid N 身份运行；默认 0（root，**不做 DAC 检查**）\n"
        "      --gid N     以 gid N 身份运行；默认 0\n"
        "                  给了 --uid/--gid 才启用 Linux VFS 权限（属主/属组/other\n"
        "                  + root 旁路）；新建 inode 按该身份铸 uid/gid\n"
        "      --help      本帮助\n"
        "\n"
        "只读命令:\n"
        "  info                                    SB / 几何 / 占用 / 空闲\n"
        "  df                                      空闲块 / inode / 使用率\n"
        "  ls [-l] [-R] [path]                     列目录（默认 /）\n"
        "  stat <path>                             inode 详情\n"
        "  tree [path] [--depth N]                 打印 dentry B+tree / extent 内部结构\n"
        "  cat <path> [--offset N] [--length N]    原始字节到 stdout\n"
        "  get <path> <hostfile> [--offset N] [--length N] [--force]\n"
        "                                          流式拷出（1 MiB 块）\n"
        "  hash <path> [--offset N] [--length N]   XXH64(seed=0)\n"
        "  audit [--verbose]                       audit_dfs_root；bad>0 退出 1\n"
        "  vmcore <path> [--verify] [--json]       ELF/PT_LOAD/SPARROW note + digest\n"
        "  find [path] [--name SUBSTR]             递归按子串找名字\n"
        "\n"
        "写命令（须镜像离线；已挂载设备被 ctor 以 RC_ERR_BUSY 拒绝）:\n"
        "  mkdir [-p] <path>                       创建目录\n"
        "  put <hostfile> <fspath> [--force] [--mkdirs]\n"
        "  rm [-r] [--shred] <path>                删除（目录须 -r）\n"
        "  cpout <fspath> <hostdir> [--force]      递归导出目录内容\n"
        "  cpin <hostdir> <fspath> [--force]       递归导入目录内容（symlink 跳过）\n"
        "  truncate <path> <size>\n"
        "  chmod <八进制mode> <path>               改权限位（如 644 / 600 / 755）\n"
        "  chown <uid>:<gid> <path>                改属主（仅 root）\n"
        "\n"
        "示例:\n"
        "  rfs_admin ravenfs-data.img df\n"
        "  rfs_admin ravenfs-data.img ls -l /sparrowOS_full_mem_*.vmcore\n"
        "  rfs_admin ravenfs-data.img vmcore /sparrowOS_full_mem_xxx.vmcore --verify\n"
        "  rfs_admin ravenfs-data.img get /dump.vmcore /tmp/dump.vmcore\n"
        "  rfs_admin ravenfs-data.img rm /old_dump.vmcore --shred\n");
    std::exit(code);
}

/* ═══════════════════════════════════════════════════════════════════
 * XXH64（seed = 0）—— 与 god name_hash / kernel rfs::xxh64_seed0 同算法。
 * 流式实现；单次 xxh64(data,len) = reset + update + digest。
 * ═══════════════════════════════════════════════════════════════════ */
class Xxh64 {
    static constexpr uint64_t P1 = 11400714785074694791ULL;
    static constexpr uint64_t P2 = 14029467366897019727ULL;
    static constexpr uint64_t P3 = 1609587929392839161ULL;
    static constexpr uint64_t P4 = 9650029242287828579ULL;
    static constexpr uint64_t P5 = 2870177450012600261ULL;

    uint64_t v1_ = 0, v2_ = 0, v3_ = 0, v4_ = 0;
    uint64_t total_ = 0;
    uint8_t  buf_[32];
    size_t   buf_len_ = 0;

    static uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
    static uint64_t round(uint64_t acc, uint64_t in) {
        acc += in * P2;
        acc = rotl(acc, 31);
        acc *= P1;
        return acc;
    }
    static uint64_t merge(uint64_t acc, uint64_t val) {
        val = round(0, val);
        acc ^= val;
        acc = acc * P1 + P4;
        return acc;
    }
    static uint64_t load64(const uint8_t *p) {
        uint64_t v;
        std::memcpy(&v, p, 8);       /* 宿主 x86_64 / LE */
        return v;
    }
    static uint32_t load32(const uint8_t *p) {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }
    void stripe(const uint8_t *p) {
        v1_ = round(v1_, load64(p + 0));
        v2_ = round(v2_, load64(p + 8));
        v3_ = round(v3_, load64(p + 16));
        v4_ = round(v4_, load64(p + 24));
    }

public:
    Xxh64() { reset(); }
    void reset() {
        v1_ = P1 + P2;
        v2_ = P2;
        v3_ = 0;
        v4_ = 0 - P1;
        total_ = 0;
        buf_len_ = 0;
    }
    void update(const void *data, size_t len) {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        const uint8_t *const end = p + len;
        total_ += len;

        if (buf_len_) {
            const size_t need = 32 - buf_len_;
            const size_t take = len < need ? len : need;
            std::memcpy(buf_ + buf_len_, p, take);
            buf_len_ += take;
            p += take;
            if (buf_len_ == 32) { stripe(buf_); buf_len_ = 0; }
        }
        if ((size_t)(end - p) >= 32) {
            const uint8_t *const limit = end - 32;
            do { stripe(p); p += 32; } while (p <= limit);
        }
        const size_t rem = (size_t)(end - p);
        if (rem) {
            std::memcpy(buf_, p, rem);
            buf_len_ = rem;
        }
    }
    uint64_t digest() const {
        uint64_t h;
        if (total_ >= 32) {
            h = rotl(v1_, 1) + rotl(v2_, 7) + rotl(v3_, 12) + rotl(v4_, 18);
            h = merge(h, v1_);
            h = merge(h, v2_);
            h = merge(h, v3_);
            h = merge(h, v4_);
        } else {
            h = P5;
        }
        h += total_;
        const uint8_t *p = buf_;
        const uint8_t *end = buf_ + buf_len_;
        while (p + 8 <= end) {
            uint64_t k1 = load64(p);
            k1 *= P2;
            k1 = rotl(k1, 31);
            k1 *= P1;
            h ^= k1;
            h = rotl(h, 27) * P1 + P4;
            p += 8;
        }
        if (p + 4 <= end) {
            h ^= (uint64_t)load32(p) * P1;
            h = rotl(h, 23) * P2 + P3;
            p += 4;
        }
        while (p < end) {
            h ^= (uint64_t)(*p) * P5;
            h = rotl(h, 11) * P1;
            ++p;
        }
        h ^= h >> 33;
        h *= P2;
        h ^= h >> 29;
        h *= P3;
        h ^= h >> 32;
        return h;
    }
};

/* ═══════════════════════════════════════════════════════════════════
 * 挂载 / 卸载
 * ═══════════════════════════════════════════════════════════════════ */
struct FsHandle {
    int fd = -1;
    bool readonly = true;
    std::unique_ptr<RavenFS_juvenile> fs;

    ~FsHandle()
    {
        /* 先析构上帝类（提交点），再关 fd——fd 生命周期归工具。 */
        fs.reset();
        if (fd >= 0) ::close(fd);
    }
};

static bool mount_image(const std::string &path, bool readonly, FsHandle &h,
                        std::string &err)
{
    int flags = readonly ? O_RDONLY : O_RDWR;
    h.fd = ::open(path.c_str(), flags);
    if (h.fd < 0) {
        err = "open " + path + " 失败: " + std::strerror(errno);
        return false;
    }
    h.readonly = readonly;

    linux_userspace_mount_t mnt{};
    mnt.dev.fd = h.fd;
    int lba = 4096;
#ifdef BLKSSZGET
    {
        int probe = 4096;
        if (::ioctl(h.fd, BLKSSZGET, &probe) == 0 && probe > 0) lba = probe;
    }
#endif
    mnt.dev.lba_size_log2 = (uint8_t)__builtin_ctz((unsigned)lba);
    mnt.dev.type = DEV_UNKNOWN;
    mnt.dev.is_device_direct = false;       /* 普通文件 / 只读 → mmap 后端 */
    mnt.param.if_will_use_inode_cache = true;
    mnt.param.if_will_use_mdata_piece_cache = true;
    mnt.param.is_readonly = readonly;
    mnt.param.slices_mode = SLICES_MODE_RUNTIME;
    mnt.logical_processor_count = 1;

    uint64_t rc = 0;
    h.fs = std::make_unique<RavenFS_juvenile>(mnt, &rc);
    if (!h.fs->is_mounted()) {
        char b[160];
        std::snprintf(b, sizeof(b), "挂载 %s 失败: %s (rc=%" PRIu64 ")",
                      path.c_str(), rc_name(rc), rc);
        err = b;
        if (rc == RC_ERR_BUSY) {
            err += "\n  镜像已被 Linux 内核挂载（RC_ERR_BUSY）。请先 umount，"
                   "或改用镜像副本；本工具拒绝双写者/撕裂读。";
        }
        h.fs.reset();
        return false;
    }
    return true;
}

/* 写命令收尾（显式让 fs 出作用域，便于错误路径也走正常提交）。 */
static void close_fs(FsHandle &h)
{
    h.fs.reset();
}

/* ═══════════════════════════════════════════════════════════════════
 * inode / 路径
 * ═══════════════════════════════════════════════════════════════════ */
struct InodeInfo {
    bool     valid = false;
    uint64_t inum = 0;
    uint32_t type = 0;
    uint32_t uid = 0, gid = 0;
    uint32_t link_count = 0;
    uint64_t mode_xattr = 0;
    uint64_t size = 0;
    uint64_t data_root = 0;
    bool     direct = false;
    uint64_t name_data_ctl = 0;
    uint64_t name_data_pieces = 0;
    uint64_t fr_ctl = 0;
    uint64_t fr_count = 0;

    uint32_t mode() const { return RFS_MODE_GET(mode_xattr); }
    bool is_dir() const { return type == INODE_TYPE_DIR; }
    bool is_reg() const { return type == INODE_TYPE_REGULAR; }
    bool is_sym() const { return type == INODE_TYPE_SOFT_SYM; }
};

static const char *type_name(uint32_t t)
{
    switch (t) {
    case INODE_TYPE_DIR:      return "dir";
    case INODE_TYPE_REGULAR:  return "file";
    case INODE_TYPE_SOFT_SYM: return "symlink";
    default:                  return "other";
    }
}

static char type_char(uint32_t t)
{
    switch (t) {
    case INODE_TYPE_DIR:      return 'd';
    case INODE_TYPE_REGULAR:  return '-';
    case INODE_TYPE_SOFT_SYM: return 'l';
    default:                  return '?';
    }
}

static bool read_inode(RavenFS_juvenile &fs, uint64_t inum, InodeInfo &out)
{
    out = InodeInfo{};
    out.inum = inum;
    InodeSession s(fs.inode_io, inum);
    if (!s.inode) return false;
    raven_Inode_t *in = s.inode;
    out.valid = true;
    out.type = RFS_INO1_GET_TYPE(in->word1);
    out.uid = in->uid;
    out.gid = in->gid;
    out.link_count = in->link_count;
    out.mode_xattr = in->mode_xattr;
    out.size = in->size;
    out.direct = RFS_INO1_GET_DIRECT(in->word1) != 0;
    if (out.type == INODE_TYPE_DIR) {
        raven_dir_Inode_t *d = reinterpret_cast<raven_dir_Inode_t *>(in);
        out.data_root = d->ddentry_Bptree_root;
        out.name_data_ctl = d->name_data_ctl;
        out.name_data_pieces = RFS_ND_PIECE_COUNT(d->name_data_piece_count);
        out.fr_ctl = d->name_data_free_ranges_ctl;
        out.fr_count = RFS_ND_FR_COUNT(d->name_data_free_ranges_count);
    } else {
        out.data_root = in->data_root;
    }
    return true;
}

/* ── DAC 检查（仅在 --uid/--gid 显式给出时生效）────────────────────
 * mask：4=r，2=w，1=x，可或。语义对齐 Linux generic_permission()：
 *   · 属主匹配取 owner 位，否则属组匹配取 group 位，否则取 other 位；
 *   · uid==0（root）旁路 rwx，但**普通文件的 x** 仍要求任一 x 位置位
 *     （与 POSIX/内核一致）；目录的搜索权 root 直接放行。
 * 未启用身份层时恒真，不改变历史行为。 */
static const char *mask_name(int mask)
{
    switch (mask) {
    case 1: return "x";
    case 2: return "w";
    case 3: return "wx";
    case 4: return "r";
    case 5: return "rx";
    case 6: return "rw";
    case 7: return "rwx";
    default: return "?";
    }
}

static bool may(const InodeInfo &ii, int mask)
{
    if (!g_cred_enforce) return true;
    const uint32_t mode = ii.mode();
    if (g_cred_uid == 0) {
        if ((mask & 1) && !ii.is_dir()) return (mode & 0111u) != 0;
        return true;
    }
    uint32_t bits;
    if (g_cred_uid == ii.uid)      bits = (mode >> 6) & 7u;
    else if (g_cred_gid == ii.gid) bits = (mode >> 3) & 7u;
    else                           bits = mode & 7u;
    return (bits & (uint32_t)mask) == (uint32_t)mask;
}

/* errout 非空时只填错误串（由调用方统一打印，避免重复两行）。 */
static bool need_perm(const InodeInfo &ii, int mask, const std::string &path,
                      const char *what, std::string *errout = nullptr)
{
    if (may(ii, mask)) return true;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "%s: 权限不足（%s 需要 %s；mode=%04o，属主 %u:%u，当前身份 %u:%u）",
                  path.c_str(), what, mask_name(mask), ii.mode(),
                  ii.uid, ii.gid, g_cred_uid, g_cred_gid);
    if (errout) *errout = buf;
    else errf("%s", buf);
    return false;
}

/* 新建 inode 的属主：仅在身份层启用时铸成当前身份；默认仍是 0:0。 */
static void stamp_owner(RavenFS_juvenile &fs, uint64_t inum)
{
    if (!g_cred_enforce) return;
    InodeSession s(fs.inode_io, inum);
    if (!s) return;
    s.inode->uid = g_cred_uid;
    s.inode->gid = g_cred_gid;
    s.dirty();
}

/* 路径拆分：去前导 '/'、折叠重复 '/'、跳过 '.'；任何 '..' → 拒绝。 */
static bool split_path(const std::string &path, std::vector<std::string> &out,
                       std::string &err)
{
    out.clear();
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        if (i >= path.size()) break;
        size_t j = i;
        while (j < path.size() && path[j] != '/') ++j;
        std::string c = path.substr(i, j - i);
        i = j;
        if (c == ".") continue;
        if (c == "..") {
            err = "路径含 '..'，拒绝: " + path;
            return false;
        }
        if (c.size() > RAVENFS_MAX_NAME_LEN) {
            err = "路径分量超过 " + std::to_string(RAVENFS_MAX_NAME_LEN) + "B: " + path;
            return false;
        }
        out.push_back(std::move(c));
    }
    return true;
}

enum class Lookup { Ok, NotFound, Error };

static Lookup lookup_path(RavenFS_juvenile &fs, const std::string &path,
                          uint64_t &inum, std::string &err)
{
    std::vector<std::string> comps;
    if (!split_path(path, comps, err)) return Lookup::Error;

    uint64_t cur = fs.root_inum;
    std::string prefix;
    for (const auto &c : comps) {
        InodeInfo ii;
        if (!read_inode(fs, cur, ii)) {
            err = "无法读取 inode " + std::to_string(cur);
            return Lookup::Error;
        }
        if (!ii.is_dir()) {
            err = "不是目录: " + (prefix.empty() ? std::string("/") : prefix);
            return Lookup::Error;
        }
        if (!need_perm(ii, 1, prefix.empty() ? "/" : prefix, "进入目录", &err))
            return Lookup::Error;
        uint64_t child = 0;
        result_code_t rc = fs.dir_lookup(cur, c.c_str(), &child);
        if (rc == RC_ERR_NOT_FOUND) {
            err = "不存在: " + (prefix.empty() ? "/" : prefix) + c;
            return Lookup::NotFound;
        }
        if (rc != RC_OK) {
            err = "lookup(" + (prefix.empty() ? "/" : prefix) + c + ") → " + rc_name(rc);
            return Lookup::Error;
        }
        prefix += "/" + c;
        cur = child;
    }
    inum = cur;
    return Lookup::Ok;
}

/* 创建用：确保 path 的父目录存在（mkdirs=true 时逐级创建）；输出父 inum + 末名。 */
static bool ensure_parent(RavenFS_juvenile &fs, const std::string &path, bool mkdirs,
                          uint64_t &parent, std::string &name, std::string &err)
{
    std::vector<std::string> comps;
    if (!split_path(path, comps, err)) return false;
    if (comps.empty()) {
        err = "路径指向根目录，不能作为创建目标: " + path;
        return false;
    }
    uint64_t cur = fs.root_inum;
    std::string prefix;
    for (size_t i = 0; i + 1 < comps.size(); ++i) {
        const std::string &c = comps[i];
        InodeInfo ii;
        if (!read_inode(fs, cur, ii) || !ii.is_dir()) {
            err = "父路径不是目录: " + (prefix.empty() ? std::string("/") : prefix);
            return false;
        }
        if (!need_perm(ii, 1, prefix.empty() ? "/" : prefix, "进入目录", &err))
            return false;
        uint64_t child = 0;
        result_code_t rc = fs.dir_lookup(cur, c.c_str(), &child);
        if (rc == RC_ERR_NOT_FOUND) {
            if (!mkdirs) {
                err = "父目录不存在: " + (prefix.empty() ? "/" : prefix) + c;
                return false;
            }
            if (!need_perm(ii, 3, prefix.empty() ? "/" : prefix, "在目录内创建", &err))
                return false;
            result_code_t ir = fs.dir_insert_dentry(cur, c.c_str(), INODE_TYPE_DIR);
            if (ir != RC_OK) {
                err = "创建目录 " + (prefix.empty() ? "/" : prefix) + c + " → " + rc_name(ir);
                return false;
            }
            ir = fs.dir_lookup(cur, c.c_str(), &child);
            if (ir != RC_OK) {
                err = "创建后 lookup " + c + " → " + rc_name(ir);
                return false;
            }
            stamp_owner(fs, child);
        } else if (rc != RC_OK) {
            err = "lookup(" + c + ") → " + rc_name(rc);
            return false;
        }
        prefix += "/" + c;
        cur = child;
    }
    parent = cur;
    name = comps.back();
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * 目录枚举（dentry B+tree 叶子链 DFS；根叶不跟 next）
 * ═══════════════════════════════════════════════════════════════════ */
struct DirEntry {
    std::string name;
    uint64_t inum = 0;
    uint64_t hash = 0;
};

static bool entry_name(RavenFS_juvenile &fs, uint64_t dir_inum, const ddentry_t &e,
                       std::string &out, std::string &err)
{
    const uint32_t nlen = RFS_PTR_FREE(e.inode_ptr);
    if (nlen == 0) {
        err = "目录项 name_len=0（损坏）";
        return false;
    }
    if (nlen > RAVENFS_MAX_NAME_LEN) {
        err = "目录项 name_len=" + std::to_string(nlen) + " 超过上限";
        return false;
    }
    char buf[RAVENFS_MAX_NAME_LEN + 1];
    const uint32_t inl = nlen < DENTRY_INLINE_NAME_MAX ? nlen : DENTRY_INLINE_NAME_MAX;
    std::memcpy(buf, e.name, inl);
    if (nlen > DENTRY_INLINE_NAME_MAX) {
        if (e.full_char_remain_offset == PTR_INVALID) {
            err = "长名但 full_char_remain_offset=PTR_INVALID";
            return false;
        }
        result_code_t rc = fs.name_data_read(dir_inum, e.full_char_remain_offset,
                                             (uint16_t)(nlen - DENTRY_INLINE_NAME_MAX),
                                             buf + DENTRY_INLINE_NAME_MAX);
        if (rc != RC_OK) {
            err = "name_data_read 失败 → " + std::string(rc_name(rc));
            return false;
        }
    }
    buf[nlen] = '\0';
    out.assign(buf, nlen);
    return true;
}

static bool read_dir(RavenFS_juvenile &fs, uint64_t dir_inum,
                     std::vector<DirEntry> &out, std::string &err)
{
    out.clear();
    InodeInfo di;
    if (!read_inode(fs, dir_inum, di)) {
        err = "无法读取目录 inode " + std::to_string(dir_inum);
        return false;
    }
    if (!di.is_dir()) {
        err = "不是目录（inode " + std::to_string(dir_inum) + "）";
        return false;
    }
    const uint64_t root = di.data_root;
    if (ptr_invalid(root)) return true;      /* 空目录（正常不变量下 dir 恒有树根） */

    alignas(8192) uint8_t buf[8192];
    auto read_node = [&](uint64_t blk) -> bool {
        result_code_t rc = fs.read_buff(blk, 2, buf);
        if (rc != RC_OK) {
            err = "read_buff(blk=" + std::to_string(blk) + ") → " + rc_name(rc);
            return false;
        }
        return true;
    };

    std::set<uint64_t> visited;
    uint64_t cur = root;
    uint64_t guard = 0;
    /* 下钻到最左叶 */
    for (;;) {
        if (++guard > (1ULL << 20)) { err = "dentry 树深度异常"; return false; }
        if (!visited.insert(cur).second) { err = "dentry 树下钻成环 @ blk " + std::to_string(cur); return false; }
        if (!read_node(cur)) return false;
        Bptree_meta_t *m = reinterpret_cast<Bptree_meta_t *>(buf);
        if (!is_dentry_node(m)) { err = "dentry 根节点魔数不符 @ blk " + std::to_string(cur); return false; }
        if (!RFS_PP_IS_INNER(m->parent_ptr)) break;
        auto *inner = reinterpret_cast<Bptree_ddentry_inner_node_t *>(buf);
        uint16_t cnt = (uint16_t)RFS_PP_COUNT(inner->meta.parent_ptr);
        if (cnt == 0) { err = "inner 节点无孩子 @ blk " + std::to_string(cur); return false; }
        uint64_t child = RFS_PTR_ADDR(inner->hints[0].sub_ptr);
        if (ptr_invalid(child)) { err = "inner hints[0] 子指针无效"; return false; }
        cur = child;
    }

    /* 叶子链 */
    uint64_t nodes = 0;
    for (;;) {
        if (++nodes > (1ULL << 20)) { err = "dentry 叶子链过长（>1M）"; return false; }
        if (!read_node(cur)) return false;
        auto *leaf = reinterpret_cast<Bptree_ddentry_leaf_node_t *>(buf);
        if (!is_dentry_node(&leaf->meta)) { err = "叶子魔数不符 @ blk " + std::to_string(cur); return false; }
        if (RFS_PP_IS_INNER(leaf->meta.parent_ptr)) { err = "期望叶子却见 inner @ blk " + std::to_string(cur); return false; }
        uint16_t cnt = (uint16_t)RFS_PP_COUNT(leaf->meta.parent_ptr);
        if (cnt > 126) { err = "叶子 entry_count=" + std::to_string(cnt) + " >126"; return false; }
        for (uint16_t i = 0; i < cnt; ++i) {
            const ddentry_t &e = leaf->entries[i];
            DirEntry de;
            if (!entry_name(fs, dir_inum, e, de.name, err)) return false;
            de.inum = RFS_PTR_ADDR(e.inode_ptr);
            de.hash = e.hash;
            out.push_back(std::move(de));
        }
        if (cur == root) break;                 /* 根叶不跟 next */
        uint64_t next = RFS_PTR_ADDR(leaf->meta.ptrs[2]);
        if (ptr_invalid(next)) break;
        if (!visited.insert(next).second) { err = "叶子链表成环 @ blk " + std::to_string(next); return false; }
        cur = next;
    }
    std::sort(out.begin(), out.end(),
              [](const DirEntry &a, const DirEntry &b) { return a.name < b.name; });
    return true;
}

/* ═══════════════════════════════════════════════════════════════════
 * 文件 I/O（流式）
 * ═══════════════════════════════════════════════════════════════════ */
/* 精确读 len 字节；EOF 提前结束（*got 报实际）。失败返回 false。 */
static bool read_exact(RavenFS_juvenile &fs, uint64_t inum, uint64_t off,
                       uint64_t len, void *buf, uint64_t *got, std::string &err)
{
    uint8_t *p = static_cast<uint8_t *>(buf);
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = 0;
        uint64_t ask = len - done;
        result_code_t rc = fs.direct_file_pread(inum, off + done, ask, p + done, &n);
        if (rc != RC_OK) {
            err = "pread(inum=" + std::to_string(inum) + ", off=" + std::to_string(off + done) +
                  ") → " + rc_name(rc);
            return false;
        }
        done += n;
        if (n == 0) break;              /* EOF */
        if (n < ask) break;             /* 短读：按 EOF 处理 */
    }
    if (got) *got = done;
    return true;
}

/* 数据流回调；返回 false 中止。 */
template <typename Fn>
static bool stream_file(RavenFS_juvenile &fs, uint64_t inum, uint64_t off, uint64_t len,
                        Fn &&fn, std::string &err)
{
    std::vector<uint8_t> buf(kChunk);
    uint64_t done = 0;
    while (done < len) {
        uint64_t ask = len - done;
        if (ask > kChunk) ask = kChunk;
        uint64_t got = 0;
        result_code_t rc = fs.direct_file_pread(inum, off + done, ask, buf.data(), &got);
        if (rc != RC_OK) {
            err = "pread(inum=" + std::to_string(inum) + ", off=" + std::to_string(off + done) +
                  ") → " + rc_name(rc);
            return false;
        }
        if (got == 0) break;
        if (!fn(buf.data(), (size_t)got, off + done)) return false;
        done += got;
    }
    return true;
}

static uint64_t effective_length(const InodeInfo &ii, uint64_t offset, bool has_len,
                                 uint64_t length)
{
    if (offset >= ii.size) return 0;
    uint64_t rest = ii.size - offset;
    if (!has_len) return rest;
    return length < rest ? length : rest;
}

/* ═══════════════════════════════════════════════════════════════════
 * 格式化助手
 * ═══════════════════════════════════════════════════════════════════ */
static std::string json_escape(const std::string &s)
{
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x", c);
                o += b;
            } else {
                o += (char)c;
            }
        }
    }
    return o;
}

static std::string mode_string(uint32_t mode)
{
    char b[16];
    std::snprintf(b, sizeof(b), "%04o", mode & 07777);
    return b;
}

struct DfStats {
    uint64_t total_blocks = 0, free_blocks = 0, used_blocks = 0;
    uint64_t total_inodes = 0, free_inodes = 0, used_inodes = 0;
};

static DfStats df_stats(RavenFS_juvenile &fs)
{
    DfStats d;
    d.total_blocks = fs.total_blocks;
    d.free_blocks = fs.allocator ? fs.allocator->block_free_total() : 0;
    d.used_blocks = d.total_blocks >= d.free_blocks ? d.total_blocks - d.free_blocks : 0;
    d.total_inodes = fs.inode_count;
    d.free_inodes = fs.allocator ? fs.allocator->inode_free_total() : 0;
    d.used_inodes = d.total_inodes >= d.free_inodes ? d.total_inodes - d.free_inodes : 0;
    return d;
}

static void print_df(const DfStats &d)
{
    double pct = d.total_blocks ? 100.0 * (double)d.used_blocks / (double)d.total_blocks : 0.0;
    std::printf("blocks: total=%" PRIu64 " free=%" PRIu64 " used=%" PRIu64
                " used_ratio=%.2f%%\n",
                d.total_blocks, d.free_blocks, d.used_blocks, pct);
    std::printf("inodes: total=%" PRIu64 " free=%" PRIu64 " used=%" PRIu64 "\n",
                d.total_inodes, d.free_inodes, d.used_inodes);
    std::printf("bytes : total=%" PRIu64 " free=%" PRIu64 "\n",
                (uint64_t)(d.total_blocks * 4096ULL), (uint64_t)(d.free_blocks * 4096ULL));
}

/* ═══════════════════════════════════════════════════════════════════
 * info / df
 * ═══════════════════════════════════════════════════════════════════ */
static int cmd_info(RavenFS_juvenile &fs)
{
    DfStats d = df_stats(fs);
    if (g_json) {
        std::printf("{\n");
        std::printf("  \"total_blocks\": %" PRIu64 ",\n", d.total_blocks);
        std::printf("  \"free_blocks\": %" PRIu64 ",\n", d.free_blocks);
        std::printf("  \"used_blocks\": %" PRIu64 ",\n", d.used_blocks);
        std::printf("  \"total_inodes\": %" PRIu64 ",\n", d.total_inodes);
        std::printf("  \"free_inodes\": %" PRIu64 ",\n", d.free_inodes);
        std::printf("  \"used_inodes\": %" PRIu64 ",\n", d.used_inodes);
        std::printf("  \"bitmap_start\": %" PRIu64 ",\n", fs.bitmap_start);
        std::printf("  \"bitmap_blocks\": %" PRIu64 ",\n", fs.bitmap_blocks);
        std::printf("  \"inode_data_pieces\": %" PRIu64 ",\n", fs.inode_data_pieces);
        std::printf("  \"root_dentry_blk\": %" PRIu64 "\n", fs.root_dentry_blk);
        std::printf("}\n");
        return 0;
    }
    fs.dump_info();
    std::printf("── df ──\n");
    print_df(d);
    return 0;
}

static int cmd_df(RavenFS_juvenile &fs)
{
    DfStats d = df_stats(fs);
    if (g_json) {
        double pct = d.total_blocks ? 100.0 * (double)d.used_blocks / (double)d.total_blocks : 0.0;
        std::printf("{\"total_blocks\":%" PRIu64 ",\"free_blocks\":%" PRIu64
                    ",\"used_blocks\":%" PRIu64 ",\"total_inodes\":%" PRIu64
                    ",\"free_inodes\":%" PRIu64 ",\"used_inodes\":%" PRIu64
                    ",\"used_ratio\":%.4f}\n",
                    d.total_blocks, d.free_blocks, d.used_blocks,
                    d.total_inodes, d.free_inodes, d.used_inodes, pct);
    } else {
        print_df(d);
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * ls
 * ═══════════════════════════════════════════════════════════════════ */
struct Enumerated { std::string path; uint64_t inum; };

static void ls_one_dir(RavenFS_juvenile &fs, const std::string &path, uint64_t inum,
                       bool longfmt, bool recursive, int depth,
                       std::set<uint64_t> &visited)
{
    if (depth > 128) { errf("递归深度超限: %s", path.c_str()); return; }
    {
        InodeInfo di;
        if (!read_inode(fs, inum, di) || !need_perm(di, 4, path, "列目录")) return;
    }
    std::vector<DirEntry> entries;
    std::string err;
    if (!read_dir(fs, inum, entries, err)) {
        errf("ls %s: %s", path.c_str(), err.c_str());
        return;
    }
    if (recursive && !g_json) std::printf("%s:\n", path.c_str());
    if (longfmt && !g_json) std::printf("%-1s %-4s %8s %8s  %s\n", "t", "mode", "size", "inum", "name");

    if (g_json) {
        if (recursive)
            std::printf("{\"dir\":\"%s\",\"entries\":[", json_escape(path).c_str());
        else
            std::printf("[");
    }
    bool first = true;
    for (const auto &e : entries) {
        InodeInfo ii;
        read_inode(fs, e.inum, ii);
        std::string cpath = (path == "/" ? "" : path) + "/" + e.name;
        if (g_json) {
            if (!first) std::printf(",");
            first = false;
            std::printf("{\"name\":\"%s\",\"path\":\"%s\",\"inum\":%" PRIu64
                        ",\"type\":\"%s\",\"size\":%" PRIu64 "}",
                        json_escape(e.name).c_str(), json_escape(cpath).c_str(),
                        e.inum, type_name(ii.type), ii.size);
        } else if (longfmt) {
            std::printf("%c  %-4s %8" PRIu64 " %8" PRIu64 "  %s\n",
                        type_char(ii.type), mode_string(ii.mode()).c_str(),
                        ii.size, e.inum, e.name.c_str());
        } else {
            std::printf("%s\n", e.name.c_str());
        }
    }
    if (g_json) std::printf(recursive ? "]}\n" : "]\n");

    if (!recursive) return;
    if (!visited.insert(inum).second) {
        errf("ls -R: 目录环 @ inum %" PRIu64 " (%s)", inum, path.c_str());
        return;
    }
    for (const auto &e : entries) {
        InodeInfo ii;
        if (!read_inode(fs, e.inum, ii)) continue;
        if (!ii.is_dir()) continue;
        std::string cpath = (path == "/" ? "" : path) + "/" + e.name;
        if (!need_perm(ii, 1, cpath, "进入目录")) continue;
        if (!g_json) std::printf("\n");
        ls_one_dir(fs, cpath, e.inum, longfmt, recursive, depth + 1, visited);
    }
}

static int cmd_ls(RavenFS_juvenile &fs, const std::vector<std::string> &args)
{
    bool longfmt = false, recursive = false;
    std::string path = "/";
    for (const auto &a : args) {
        if (a == "-l") longfmt = true;
        else if (a == "-R") recursive = true;
        else if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
            for (size_t i = 1; i < a.size(); ++i) {
                if (a[i] == 'l') longfmt = true;
                else if (a[i] == 'R') recursive = true;
                else { errf("ls: 未知选项 -%c", a[i]); return 2; }
            }
        } else if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
            errf("ls: 未知选项 %s", a.c_str());
            return 2;
        } else {
            path = a;
        }
    }

    uint64_t inum = 0;
    std::string err;
    Lookup lr = lookup_path(fs, path, inum, err);
    if (lr != Lookup::Ok) {
        errf("%s", err.c_str());
        return 2;
    }
    InodeInfo ii;
    if (!read_inode(fs, inum, ii)) { errf("无法读取 inode %" PRIu64, inum); return 2; }
    if (ii.is_dir() && !need_perm(ii, 4, path, "列目录")) return 2;
    if (!ii.is_dir()) {
        if (g_json) {
            std::printf("[{\"name\":\"%s\",\"path\":\"%s\",\"inum\":%" PRIu64
                        ",\"type\":\"%s\",\"size\":%" PRIu64 "}]\n",
                        json_escape(path).c_str(), json_escape(path).c_str(),
                        inum, type_name(ii.type), ii.size);
        } else if (longfmt) {
            std::printf("%c  %-4s %8" PRIu64 " %8" PRIu64 "  %s\n",
                        type_char(ii.type), mode_string(ii.mode()).c_str(), ii.size, inum,
                        path.c_str());
        } else {
            std::printf("%s\n", path.c_str());
        }
        return 0;
    }
    std::set<uint64_t> visited;
    ls_one_dir(fs, path, inum, longfmt, recursive, 0, visited);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * stat
 * ═══════════════════════════════════════════════════════════════════ */
static const char *form_name(const InodeInfo &ii)
{
    if (!ii.is_reg() && !ii.is_sym()) return "-";
    if (ii.size <= 160) return "inline";
    if (ii.direct) return "direct";
    return "extent";
}

static int cmd_stat(RavenFS_juvenile &fs, const std::vector<std::string> &args)
{
    if (args.size() != 1) { errf("用法: stat <path>"); return 2; }
    uint64_t inum = 0;
    std::string err;
    Lookup lr = lookup_path(fs, args[0], inum, err);
    if (lr != Lookup::Ok) { errf("%s", err.c_str()); return 2; }
    InodeInfo ii;
    if (!read_inode(fs, inum, ii)) { errf("无法读取 inode %" PRIu64, inum); return 2; }

    if (g_json) {
        std::printf("{\"path\":\"%s\",\"inum\":%" PRIu64 ",\"type\":\"%s\","
                    "\"size\":%" PRIu64 ",\"link_count\":%u,\"mode\":\"%s\","
                    "\"uid\":%u,\"gid\":%u,\"form\":\"%s\",\"data_root\":%" PRIu64
                    ",\"direct\":%s",
                    json_escape(args[0]).c_str(), inum, type_name(ii.type), ii.size,
                    ii.link_count, mode_string(ii.mode()).c_str(), ii.uid, ii.gid,
                    form_name(ii), ii.data_root, ii.direct ? "true" : "false");
        if (ii.is_dir()) {
            std::printf(",\"ddentry_root\":%" PRIu64
                        ",\"name_data_ctl\":%" PRIu64 ",\"name_data_pieces\":%" PRIu64
                        ",\"name_data_fr_ctl\":%" PRIu64 ",\"name_data_fr_count\":%" PRIu64,
                        ii.data_root, ii.name_data_ctl, ii.name_data_pieces,
                        ii.fr_ctl, ii.fr_count);
        }
        std::printf("}\n");
        return 0;
    }

    std::printf("path        : %s\n", args[0].c_str());
    std::printf("inum        : %" PRIu64 "\n", inum);
    std::printf("type        : %s (%u)\n", type_name(ii.type), ii.type);
    std::printf("size        : %" PRIu64 " bytes%s\n", ii.size,
                ii.is_dir() ? " (目录条目数)" : "");
    std::printf("link_count  : %u\n", ii.link_count);
    std::printf("mode        : %s\n", mode_string(ii.mode()).c_str());
    std::printf("uid/gid     : %u/%u\n", ii.uid, ii.gid);
    if (ii.is_dir()) {
        std::printf("form        : dir\n");
        std::printf("ddentry_root: %" PRIu64 "\n", ii.data_root);
        std::printf("name_data   : ctl=%#" PRIx64 " direct=%u pieces=%" PRIu64
                    " fr_ctl=%#" PRIx64 " fr_count=%" PRIu64 "\n",
                    ii.name_data_ctl, RFS_ND_CTL_IS_DIRECT(ii.name_data_ctl),
                    ii.name_data_pieces, ii.fr_ctl, ii.fr_count);
    } else {
        std::printf("form        : %s\n", form_name(ii));
        std::printf("data_root   : %" PRIu64 " (0x%" PRIx64 ")\n", ii.data_root, ii.data_root);
        std::printf("direct_bit  : %u\n", ii.direct ? 1u : 0u);
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * tree —— 内部结构视图
 * ═══════════════════════════════════════════════════════════════════ */
static void print_extent_nodes(RavenFS_juvenile &fs, uint64_t blk, int level,
                               const std::string &indent, std::set<uint64_t> &visited,
                               std::vector<extent_t> *recs, uint64_t &nodes)
{
    if (blk == PTR_INVALID || blk == kPtrInvalid52) return;
    if (!visited.insert(blk).second) {
        std::printf("%s[!] extent 树成环 @ blk %" PRIu64 "\n", indent.c_str(), blk);
        return;
    }
    if (++nodes > (1ULL << 20)) return;

    alignas(8192) uint8_t buf[8192];
    if (fs.read_buff(blk, 2, buf) != RC_OK) {
        std::printf("%s[!] read_buff blk %" PRIu64 " 失败\n", indent.c_str(), blk);
        return;
    }
    Bptree_meta_t *m = reinterpret_cast<Bptree_meta_t *>(buf);
    if (!is_extent_node(m)) {
        std::printf("%s[!] blk %" PRIu64 " 不是 extent 节点\n", indent.c_str(), blk);
        return;
    }
    uint16_t cnt = (uint16_t)RFS_PP_COUNT(m->parent_ptr);
    bool inner = RFS_PP_IS_INNER(m->parent_ptr) != 0;
    std::printf("%sblk=%" PRIu64 " level=%d %s count=%u parent=%" PRIu64 "\n",
                indent.c_str(), blk, level, inner ? "inner" : "leaf", cnt,
                RFS_PP_PTR(m->parent_ptr));
    if (inner) {
        auto *in = reinterpret_cast<Bptree_extent_inner_node_t *>(buf);
        for (uint16_t i = 0; i < cnt; ++i) {
            std::printf("%s  hint[%u] key=%" PRIu64 " sub=%" PRIu64 "\n",
                        indent.c_str(), i, in->extents[i].key,
                        RFS_PTR_ADDR(in->extents[i].sub_node_ptr));
            print_extent_nodes(fs, RFS_PTR_ADDR(in->extents[i].sub_node_ptr), level + 1,
                               indent + "  ", visited, recs, nodes);
        }
    } else {
        auto *lf = reinterpret_cast<Bptree_extent_leaf_node_t *>(buf);
        for (uint16_t i = 0; i < cnt; ++i) {
            const extent_t &e = lf->extents[i];
            uint64_t phys = RFS_PTR_ADDR(e.block_base);
            std::printf("%s  rec[%u] fblkbase=%" PRIu64 " length=%" PRIu64
                        " phys=%" PRIu64 "\n",
                        indent.c_str(), i, e.fblkbase, e.length, phys);
            if (recs) recs->push_back(e);
        }
    }
}

static void collect_extents(RavenFS_juvenile &fs, uint64_t root,
                            std::vector<extent_t> &recs, std::string &err)
{
    recs.clear();
    if (ptr_invalid(root)) return;
    std::set<uint64_t> visited;
    uint64_t nodes = 0;
    /* 复用打印函数的收集模式：独立 DFS，不打印 */
    struct Walker {
        static void go(RavenFS_juvenile &fs, uint64_t blk, std::set<uint64_t> &vis,
                       uint64_t &nodes, std::vector<extent_t> &out, std::string &err)
        {
            if (ptr_invalid(blk)) return;
            if (!vis.insert(blk).second) { err = "extent 树成环"; return; }
            if (++nodes > (1ULL << 20)) { err = "extent 节点超限"; return; }
            alignas(8192) uint8_t buf[8192];
            if (fs.read_buff(blk, 2, buf) != RC_OK) { err = "extent read_buff 失败"; return; }
            Bptree_meta_t *m = reinterpret_cast<Bptree_meta_t *>(buf);
            if (!is_extent_node(m)) { err = "extent 魔数不符"; return; }
            uint16_t cnt = (uint16_t)RFS_PP_COUNT(m->parent_ptr);
            if (RFS_PP_IS_INNER(m->parent_ptr)) {
                auto *in = reinterpret_cast<Bptree_extent_inner_node_t *>(buf);
                for (uint16_t i = 0; i < cnt && err.empty(); ++i)
                    go(fs, RFS_PTR_ADDR(in->extents[i].sub_node_ptr), vis, nodes, out, err);
            } else {
                auto *lf = reinterpret_cast<Bptree_extent_leaf_node_t *>(buf);
                for (uint16_t i = 0; i < cnt; ++i) out.push_back(lf->extents[i]);
            }
        }
    };
    Walker::go(fs, root, visited, nodes, recs, err);
}

static void hexdump(const uint8_t *p, size_t n, const std::string &indent)
{
    for (size_t off = 0; off < n; off += 16) {
        std::printf("%s%08zx  ", indent.c_str(), off);
        for (size_t i = 0; i < 16; ++i) {
            if (off + i < n) std::printf("%02x ", p[off + i]);
            else std::printf("   ");
        }
        std::printf(" |");
        for (size_t i = 0; i < 16 && off + i < n; ++i) {
            unsigned char c = p[off + i];
            std::printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
        }
        std::printf("|\n");
    }
}

static void tree_dir_node(RavenFS_juvenile &fs, uint64_t dir_inum, uint64_t blk,
                          int level, const std::string &indent,
                          std::set<uint64_t> &visited)
{
    if (ptr_invalid(blk)) return;
    if (!visited.insert(blk).second) {
        std::printf("%s[!] dentry 节点重复/成环 @ blk %" PRIu64 "\n", indent.c_str(), blk);
        return;
    }
    alignas(8192) uint8_t buf[8192];
    if (fs.read_buff(blk, 2, buf) != RC_OK) {
        std::printf("%s[!] read_buff blk %" PRIu64 " 失败\n", indent.c_str(), blk);
        return;
    }
    Bptree_meta_t *m = reinterpret_cast<Bptree_meta_t *>(buf);
    if (!is_dentry_node(m)) {
        std::printf("%s[!] blk %" PRIu64 " 不是 dentry 节点\n", indent.c_str(), blk);
        return;
    }
    uint16_t cnt = (uint16_t)RFS_PP_COUNT(m->parent_ptr);
    bool inner = RFS_PP_IS_INNER(m->parent_ptr) != 0;
    bool rootflag = RFS_PP_IS_ROOT(m->parent_ptr) != 0;
    uint64_t parent = RFS_PP_PTR(m->parent_ptr);

    uint64_t hmin = 0, hmax = 0;
    if (inner) {
        auto *in = reinterpret_cast<Bptree_ddentry_inner_node_t *>(buf);
        if (cnt) { hmin = in->hints[0].hash_max; hmax = in->hints[cnt - 1].hash_max; }
    } else {
        auto *lf = reinterpret_cast<Bptree_ddentry_leaf_node_t *>(buf);
        if (cnt) { hmin = lf->entries[0].hash; hmax = lf->entries[cnt - 1].hash; }
    }
    std::printf("%sblk=%" PRIu64 " level=%d %s count=%u root=%u parent=%" PRIu64
                " hash=[%" PRIu64 "..%" PRIu64 "]\n",
                indent.c_str(), blk, level, inner ? "inner" : "leaf", cnt,
                rootflag ? 1u : 0u, parent, hmin, hmax);

    if (inner) {
        auto *in = reinterpret_cast<Bptree_ddentry_inner_node_t *>(buf);
        for (uint16_t i = 0; i < cnt; ++i) {
            uint64_t sub = RFS_PTR_ADDR(in->hints[i].sub_ptr);
            std::printf("%s  hint[%u] hash_max=%" PRIu64 " sub=%" PRIu64 "\n",
                        indent.c_str(), i, in->hints[i].hash_max, sub);
            tree_dir_node(fs, dir_inum, sub, level + 1, indent + "  ", visited);
        }
    } else {
        auto *lf = reinterpret_cast<Bptree_ddentry_leaf_node_t *>(buf);
        for (uint16_t i = 0; i < cnt; ++i) {
            const ddentry_t &e = lf->entries[i];
            std::string name;
            std::string err;
            if (!entry_name(fs, dir_inum, e, name, err)) {
                std::printf("%s  entry[%u] <名字读取失败: %s>\n", indent.c_str(), i, err.c_str());
                continue;
            }
            uint64_t inum = RFS_PTR_ADDR(e.inode_ptr);
            InodeInfo ii;
            read_inode(fs, inum, ii);
            std::printf("%s  entry[%u] hash=%" PRIu64 " name=\"%s\" inum=%" PRIu64
                        " type=%s nlen=%u\n",
                        indent.c_str(), i, e.hash, name.c_str(), inum,
                        type_name(ii.type), RFS_PTR_FREE(e.inode_ptr));
        }
    }
}

static void tree_dir(RavenFS_juvenile &fs, uint64_t inum, const std::string &path,
                     int depth, int maxdepth, std::set<uint64_t> &dirs)
{
    if (depth > maxdepth) return;
    if (!dirs.insert(inum).second) {
        std::printf("%s[!] 目录环 inum=%" PRIu64 "\n", path.c_str(), inum);
        return;
    }
    InodeInfo ii;
    if (!read_inode(fs, inum, ii)) return;
    std::printf("\n[目录] %s  inum=%" PRIu64 " entries=%" PRIu64
                " ddentry_root=%" PRIu64 "\n",
                path.c_str(), inum, ii.size, ii.data_root);
    std::set<uint64_t> visited;
    tree_dir_node(fs, inum, ii.data_root, 0, "  ", visited);

    if (!ptr_invalid(ii.name_data_ctl) && ii.name_data_ctl != RFS_ND_CTL_INACTIVE) {
        bool direct = RFS_ND_CTL_IS_DIRECT(ii.name_data_ctl);
        std::printf("  name_data: ctl=%#" PRIx64 " %s ptr=%" PRIu64
                    " pieces=%" PRIu64 " fr_ctl=%#" PRIx64 " fr_count=%" PRIu64 "\n",
                    ii.name_data_ctl, direct ? "direct" : "tree",
                    RFS_ND_CTL_PTR(ii.name_data_ctl), ii.name_data_pieces,
                    ii.fr_ctl, ii.fr_count);
        if (!direct) {
            std::vector<extent_t> recs;
            std::string err;
            uint64_t root = RFS_ND_CTL_PTR(ii.name_data_ctl);
            /* 名字数据面映射树：节点结构同 extent，但魔数是 NAMEDATA 族 */
            struct Walker {
                static void go(RavenFS_juvenile &fs, uint64_t blk, std::set<uint64_t> &vis,
                               uint64_t &nodes, std::vector<extent_t> &out, std::string &err)
                {
                    if (ptr_invalid(blk)) return;
                    if (!vis.insert(blk).second) { err = "namedata 映射树成环"; return; }
                    if (++nodes > (1ULL << 20)) { err = "namedata 节点超限"; return; }
                    alignas(8192) uint8_t buf[8192];
                    if (fs.read_buff(blk, 2, buf) != RC_OK) { err = "namedata read 失败"; return; }
                    Bptree_meta_t *m = reinterpret_cast<Bptree_meta_t *>(buf);
                    if (!RavenFS_juvenile::is_extent_node_kind(m, RavenFS_juvenile::EXTENT_TREE_NAMEDATA)) {
                        err = "namedata 魔数不符"; return;
                    }
                    uint16_t cnt = (uint16_t)RFS_PP_COUNT(m->parent_ptr);
                    if (RFS_PP_IS_INNER(m->parent_ptr)) {
                        auto *in = reinterpret_cast<Bptree_extent_inner_node_t *>(buf);
                        for (uint16_t i = 0; i < cnt && err.empty(); ++i)
                            go(fs, RFS_PTR_ADDR(in->extents[i].sub_node_ptr), vis, nodes, out, err);
                    } else {
                        auto *lf = reinterpret_cast<Bptree_extent_leaf_node_t *>(buf);
                        for (uint16_t i = 0; i < cnt; ++i) out.push_back(lf->extents[i]);
                    }
                }
            };
            std::set<uint64_t> vis;
            uint64_t nodes = 0;
            Walker::go(fs, root, vis, nodes, recs, err);
            if (!err.empty()) {
                std::printf("    namedata walk: %s\n", err.c_str());
            } else {
                for (size_t i = 0; i < recs.size(); ++i) {
                    std::printf("    nd_extent[%zu] lbase=%" PRIu64 " length=%" PRIu64
                                " phys=%" PRIu64 "\n",
                                i, recs[i].fblkbase, recs[i].length,
                                RFS_PTR_ADDR(recs[i].block_base));
                }
            }
        }
    }

    /* 递归子目录 */
    std::vector<DirEntry> entries;
    std::string err;
    if (!read_dir(fs, inum, entries, err)) {
        std::printf("  [!] read_dir: %s\n", err.c_str());
        return;
    }
    for (const auto &e : entries) {
        InodeInfo ci;
        if (!read_inode(fs, e.inum, ci)) continue;
        if (!ci.is_dir()) continue;
        tree_dir(fs, e.inum, (path == "/" ? "" : path) + "/" + e.name, depth + 1, maxdepth, dirs);
    }
}

static void tree_file(RavenFS_juvenile &fs, uint64_t inum, const std::string &path,
                      const InodeInfo &ii)
{
    const char *form = form_name(ii);
    std::printf("\n[文件] %s  inum=%" PRIu64 " size=%" PRIu64 " form=%s data_root=%" PRIu64 "\n",
                path.c_str(), inum, ii.size, form, ii.data_root);
    if (ii.size <= 160) {
        size_t n = ii.size;
        alignas(8192) uint8_t tmp[8192];
        uint64_t got = 0;
        std::string err;
        if (read_exact(fs, inum, 0, n, tmp, &got, err) && got == n) {
            std::printf("  inline[0..%zu):\n", n);
            hexdump(tmp, n, "    ");
        } else {
            std::printf("  inline 读取失败: %s\n", err.c_str());
        }
        return;
    }
    if (ii.direct) {
        std::printf("  direct: phys_base=%" PRIu64 " blocks=%" PRIu64 "\n",
                    ii.data_root, (ii.size + 4095) / 4096);
        size_t n = ii.size < 256 ? (size_t)ii.size : 256;
        std::vector<uint8_t> tmp(n);
        uint64_t got = 0;
        std::string err;
        if (read_exact(fs, inum, 0, n, tmp.data(), &got, err)) {
            hexdump(tmp.data(), (size_t)got, "    ");
        } else {
            std::printf("    读取失败: %s\n", err.c_str());
        }
        return;
    }
    std::vector<extent_t> recs;
    std::string err;
    collect_extents(fs, ii.data_root, recs, err);
    if (!err.empty()) {
        std::printf("  [!] %s\n", err.c_str());
        return;
    }
    std::set<uint64_t> visited;
    uint64_t nodes = 0;
    std::printf("  extent tree:\n");
    print_extent_nodes(fs, ii.data_root, 0, "    ", visited, nullptr, nodes);
    std::printf("  extents (%zu):\n", recs.size());
    for (size_t i = 0; i < recs.size(); ++i) {
        std::printf("    rec[%zu] fblkbase=%" PRIu64 " length=%" PRIu64 " phys=%" PRIu64 "\n",
                    i, recs[i].fblkbase, recs[i].length, RFS_PTR_ADDR(recs[i].block_base));
    }
}

static int cmd_tree(RavenFS_juvenile &fs, const std::vector<std::string> &args)
{
    std::string path = "/";
    int maxdepth = 64;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--depth") {
            if (i + 1 >= args.size()) { errf("tree: --depth 需要参数"); return 2; }
            maxdepth = std::atoi(args[++i].c_str());
            if (maxdepth < 0) maxdepth = 0;
        } else if (args[i].rfind("--depth=", 0) == 0) {
            maxdepth = std::atoi(args[i].c_str() + 8);
            if (maxdepth < 0) maxdepth = 0;
        } else if (!args[i].empty() && args[i][0] == '-' && args[i] != "-") {
            errf("tree: 未知选项 %s", args[i].c_str());
            return 2;
        } else {
            path = args[i];
        }
    }
    uint64_t inum = 0;
    std::string err;
    Lookup lr = lookup_path(fs, path, inum, err);
    if (lr != Lookup::Ok) { errf("%s", err.c_str()); return 2; }
    InodeInfo ii;
    if (!read_inode(fs, inum, ii)) { errf("无法读取 inode %" PRIu64, inum); return 2; }
    if (ii.is_dir()) {
        std::set<uint64_t> dirs;
        tree_dir(fs, inum, path, 0, maxdepth, dirs);
    } else {
        tree_file(fs, inum, path, ii);
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * cat / get / hash
 * ═══════════════════════════════════════════════════════════════════ */
struct RangeArgs {
    std::string path;
    uint64_t offset = 0;
    bool has_len = false;
    uint64_t length = 0;
};

static bool parse_range_args(const std::vector<std::string> &args, RangeArgs &ra,
                             bool allow_extra, std::vector<std::string> &extra,
                             std::string &err)
{
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "--offset" || a == "--length") {
            if (i + 1 >= args.size()) { err = a + " 需要参数"; return false; }
            uint64_t v = std::strtoull(args[++i].c_str(), nullptr, 0);
            if (a == "--offset") ra.offset = v;
            else { ra.has_len = true; ra.length = v; }
        } else if (a.rfind("--offset=", 0) == 0) {
            ra.offset = std::strtoull(a.c_str() + 9, nullptr, 0);
        } else if (a.rfind("--length=", 0) == 0) {
            ra.has_len = true;
            ra.length = std::strtoull(a.c_str() + 9, nullptr, 0);
        } else if (!a.empty() && a[0] == '-' && a != "-") {
            err = "未知选项 " + a;
            return false;
        } else if (ra.path.empty()) {
            ra.path = a;
        } else if (allow_extra) {
            extra.push_back(a);
        } else {
            err = "多余参数: " + a;
            return false;
        }
    }
    if (ra.path.empty()) { err = "缺少 <path>"; return false; }
    return true;
}

static bool open_fs_file(RavenFS_juvenile &fs, const std::string &path, uint64_t &inum,
                         InodeInfo &ii, std::string &err)
{
    Lookup lr = lookup_path(fs, path, inum, err);
    if (lr != Lookup::Ok) return false;
    if (!read_inode(fs, inum, ii)) { err = "无法读取 inode"; return false; }
    if (ii.is_dir()) { err = path + " 是目录"; return false; }
    if (!ii.is_reg()) { err = path + " 不是普通文件（type=" + type_name(ii.type) + "）"; return false; }
    return true;
}

static int cmd_cat(RavenFS_juvenile &fs, const std::vector<std::string> &args)
{
    RangeArgs ra;
    std::string err;
    std::vector<std::string> extra;
    if (!parse_range_args(args, ra, false, extra, err)) { errf("cat: %s", err.c_str()); return 2; }
    uint64_t inum = 0;
    InodeInfo ii;
    if (!open_fs_file(fs, ra.path, inum, ii, err)) { errf("%s", err.c_str()); return 2; }
    if (!need_perm(ii, 4, ra.path, "读取")) return 2;
    uint64_t len = effective_length(ii, ra.offset, ra.has_len, ra.length);
    if (!stream_file(fs, inum, ra.offset, len,
                     [](const uint8_t *p, size_t n, uint64_t) {
                         return std::fwrite(p, 1, n, stdout) == n;
                     }, err)) {
        errf("cat: %s", err.c_str());
        return 2;
    }
    std::fflush(stdout);
    return 0;
}

static int cmd_get(RavenFS_juvenile &fs, const std::vector<std::string> &args)
{
    RangeArgs ra;
    std::vector<std::string> extra;
    bool force = false;
    std::string err;
    /* 先摘 --force，再交给 range 解析 */
    std::vector<std::string> filtered;
    for (const auto &a : args) {
        if (a == "--force") force = true;
        else filtered.push_back(a);
    }
    if (!parse_range_args(filtered, ra, true, extra, err)) { errf("get: %s", err.c_str()); return 2; }
    if (extra.size() != 1) { errf("用法: get <path> <hostfile> [--offset N] [--length N] [--force]"); return 2; }
    const std::string hostfile = extra[0];

    uint64_t inum = 0;
    InodeInfo ii;
    if (!open_fs_file(fs, ra.path, inum, ii, err)) { errf("%s", err.c_str()); return 2; }
    if (!need_perm(ii, 4, ra.path, "读取")) return 2;
    uint64_t len = effective_length(ii, ra.offset, ra.has_len, ra.length);

    struct stat st;
    if (::lstat(hostfile.c_str(), &st) == 0 && !force) {
        errf("%s 已存在（加 --force 覆盖）", hostfile.c_str());
        return 2;
    }
    int fd = ::open(hostfile.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (fd < 0) { errf("open %s 失败: %s", hostfile.c_str(), std::strerror(errno)); return 2; }

    uint64_t written = 0;
    bool ok = stream_file(fs, inum, ra.offset, len,
                          [&](const uint8_t *p, size_t n, uint64_t) {
                              size_t off = 0;
                              while (off < n) {
                                  ssize_t w = ::write(fd, p + off, n - off);
                                  if (w < 0) {
                                      if (errno == EINTR) continue;
                                      err = std::string("write 失败: ") + std::strerror(errno);
                                      return false;
                                  }
                                  off += (size_t)w;
                              }
                              written += n;
                              return true;
                          }, err);
    if (::fsync(fd) != 0 && ok) {
        err = std::string("fsync 失败: ") + std::strerror(errno);
        ok = false;
    }
    if (::close(fd) != 0 && ok) {
        err = std::string("close 失败: ") + std::strerror(errno);
        ok = false;
    }
    if (!ok) {
        errf("get %s: %s", ra.path.c_str(), err.c_str());
        return 2;
    }
    std::printf("get: %s → %s (%" PRIu64 " bytes)\n", ra.path.c_str(), hostfile.c_str(), written);
    return 0;
}

static int cmd_hash(RavenFS_juvenile &fs, const std::vector<std::string> &args)
{
    RangeArgs ra;
    std::vector<std::string> extra;
    std::string err;
    if (!parse_range_args(args, ra, false, extra, err)) { errf("hash: %s", err.c_str()); return 2; }
    uint64_t inum = 0;
    InodeInfo ii;
    if (!open_fs_file(fs, ra.path, inum, ii, err)) { errf("%s", err.c_str()); return 2; }
    if (!need_perm(ii, 4, ra.path, "读取")) return 2;
    uint64_t len = effective_length(ii, ra.offset, ra.has_len, ra.length);

    Xxh64 h;
    uint64_t done = 0;
    if (!stream_file(fs, inum, ra.offset, len,
                     [&](const uint8_t *p, size_t n, uint64_t) {
                         h.update(p, n);
                         done += n;
                         return true;
                     }, err)) {
        errf("hash: %s", err.c_str());
        return 2;
    }
    uint64_t v = h.digest();
    if (g_json) {
        std::printf("{\"path\":\"%s\",\"xxh64\":\"%016" PRIx64 "\",\"len\":%" PRIu64
                    ",\"offset\":%" PRIu64 "}\n",
                    json_escape(ra.path).c_str(), v, done, ra.offset);
    } else {
        std::printf("xxh64=%016" PRIx64 " len=%" PRIu64 "\n", v, done);
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * audit
 * ═══════════════════════════════════════════════════════════════════ */
static int cmd_audit(RavenFS_juvenile &fs, const std::vector<std::string> &args)
{
    bool verbose = false;
    for (const auto &a : args) {
        if (a == "--verbose" || a == "-v") verbose = true;
        else { errf("audit: 未知参数 %s", a.c_str()); return 2; }
    }
    audit_walk_report_t rep{};
    FILE *out = g_json ? stderr : stdout;
    result_code_t rc = fs.audit_dfs_root(&rep, out, verbose);
    if (rc != RC_OK) {
        errf("audit_dfs_root → %s", rc_name(rc));
        return 2;
    }
    const uint64_t bad = rep.total_bad();
    if (g_json) {
        std::printf("{\"total_bad\":%" PRIu64 ",\"btree_bad\":%" PRIu64
                    ",\"inode_bad\":%" PRIu64 ",\"namedata_bad\":%" PRIu64
                    ",\"inodeplane_bad\":%" PRIu64 ",\"xattr_bad\":%" PRIu64
                    ",\"multi_ref_ino\":%" PRIu64 ",\"multi_claim_blk\":%" PRIu64
                    ",\"link_mismatch\":%" PRIu64 ",\"dotdot_mismatch\":%" PRIu64
                    ",\"unprotected_blk\":%" PRIu64 ",\"unprotected_ino\":%" PRIu64
                    ",\"leaked_blk\":%" PRIu64 ",\"leaked_ino\":%" PRIu64
                    ",\"inodes_walked\":%" PRIu64 ",\"nodes\":%" PRIu64
                    ",\"records\":%" PRIu64 "}\n",
                    bad, rep.btree_bad, rep.inode_bad, rep.namedata_bad,
                    rep.inodeplane_bad, rep.xattr_bad, rep.multi_ref_ino,
                    rep.multi_claim_blk, rep.link_mismatch, rep.dotdot_mismatch,
                    rep.unprotected_blk, rep.unprotected_ino, rep.leaked_blk,
                    rep.leaked_ino, rep.inodes_walked, rep.nodes, rep.records);
    } else {
        std::printf("[audit] total_bad=%" PRIu64 " (btree=%" PRIu64 " inode=%" PRIu64
                    " namedata=%" PRIu64 " inodeplane=%" PRIu64 " xattr=%" PRIu64
                    " multi_ref=%" PRIu64 " multi_claim=%" PRIu64 " link=%" PRIu64
                    " dotdot=%" PRIu64 " | unprot b/i=%" PRIu64 "/%" PRIu64
                    " leak b/i=%" PRIu64 "/%" PRIu64 ") inodes=%" PRIu64
                    " nodes=%" PRIu64 " records=%" PRIu64 "\n",
                    bad, rep.btree_bad, rep.inode_bad, rep.namedata_bad,
                    rep.inodeplane_bad, rep.xattr_bad, rep.multi_ref_ino,
                    rep.multi_claim_blk, rep.link_mismatch, rep.dotdot_mismatch,
                    rep.unprotected_blk, rep.unprotected_ino, rep.leaked_blk,
                    rep.leaked_ino, rep.inodes_walked, rep.nodes, rep.records);
        std::printf("[audit] VERDICT: %s\n", bad == 0 ? "PASS" : "FAIL");
    }
    return bad == 0 ? 0 : 1;
}

/* ═══════════════════════════════════════════════════════════════════
 * vmcore —— ELF64 / PT_LOAD / SPARROW note / digest
 * ═══════════════════════════════════════════════════════════════════ */
#pragma pack(push, 1)
struct Elf64Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};
struct Elf64Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};
struct SparrowDesc {
    uint64_t magic;
    uint32_t version;
    uint32_t flags;
    uint64_t total_bytes;
    uint64_t dram_top;
    uint64_t segments;
    uint64_t layout_xxh64;
    uint64_t data_xxh64;
    uint64_t data_offset;
};
#pragma pack(pop)

static_assert(sizeof(Elf64Ehdr) == 64, "Elf64Ehdr must be 64B");
static_assert(sizeof(Elf64Phdr) == 56, "Elf64Phdr must be 56B");
static_assert(sizeof(SparrowDesc) == 64, "SparrowDesc must be 64B");

static constexpr uint32_t PT_LOAD = 1;
static constexpr uint32_t PT_NOTE = 4;
static constexpr uint16_t ET_CORE = 4;
static constexpr uint16_t EM_X86_64 = 62;
static constexpr uint64_t SPARROW_MAGIC = 0x3130444D46575053ULL;   /* "SPWFMD01" */
static constexpr uint32_t SPARROW_TYPE = 0x53504D44u;               /* "SPMD" */

struct SparrowNote {
    bool found = false;
    uint64_t note_off = 0;       /* SPARROW note 头在文件内的偏移（layout 边界） */
    SparrowDesc desc{};
    bool desc_ok = false;
};

struct NoteSummary {
    uint64_t total = 0;
    uint64_t core_prstatus = 0;
    uint64_t qemu = 0;
    SparrowNote sparrow;
};

static void note_owner(const uint8_t *p, uint32_t namesz, char *out, size_t cap)
{
    size_t n = 0;
    while (n < namesz && n + 1 < cap && p[n] != '\0') { out[n] = (char)p[n]; ++n; }
    out[n] = '\0';
}

static bool parse_notes(const uint8_t *data, size_t n, uint64_t base_off,
                        NoteSummary &sum, std::string &err)
{
    size_t off = 0;
    while (off + 12 <= n) {
        uint32_t namesz, descsz, type;
        std::memcpy(&namesz, data + off, 4);
        std::memcpy(&descsz, data + off + 4, 4);
        std::memcpy(&type, data + off + 8, 4);
        const size_t name_off = off + 12;
        if (name_off + namesz < name_off || name_off + namesz > n) {
            err = "note name 越界";
            return false;
        }
        const size_t desc_off = (name_off + namesz + 3) & ~(size_t)3;
        if (desc_off + descsz < desc_off || desc_off + descsz > n) {
            err = "note desc 越界";
            return false;
        }
        char owner[64];
        note_owner(data + name_off, namesz, owner, sizeof(owner));
        ++sum.total;
        if (std::strcmp(owner, "CORE") == 0 && type == 1) ++sum.core_prstatus;
        if (std::strcmp(owner, "QEMU") == 0) ++sum.qemu;
        if (!g_json) {
            std::printf("    note @%#" PRIx64 " owner=\"%s\" type=%#" PRIx32
                        " namesz=%u descsz=%u\n",
                        base_off + off, owner, type, namesz, descsz);
        }
        if (std::strcmp(owner, "SPARROW") == 0 && type == SPARROW_TYPE && !sum.sparrow.found) {
            sum.sparrow.found = true;
            sum.sparrow.note_off = base_off + off;
            if (descsz >= sizeof(SparrowDesc)) {
                std::memcpy(&sum.sparrow.desc, data + desc_off, sizeof(SparrowDesc));
                sum.sparrow.desc_ok = true;
            }
        }
        size_t next = (desc_off + descsz + 3) & ~(size_t)3;
        if (next <= off) break;
        off = next;
    }
    return true;
}

static int cmd_vmcore(RavenFS_juvenile &fs, const std::vector<std::string> &args)
{
    bool verify = false;
    std::string path;
    for (const auto &a : args) {
        if (a == "--verify") verify = true;
        else if (!a.empty() && a[0] == '-' && a != "-") { errf("vmcore: 未知选项 %s", a.c_str()); return 2; }
        else if (path.empty()) path = a;
        else { errf("vmcore: 多余参数 %s", a.c_str()); return 2; }
    }
    if (path.empty()) { errf("用法: vmcore <path> [--verify]"); return 2; }

    uint64_t inum = 0;
    InodeInfo ii;
    std::string err;
    if (!open_fs_file(fs, path, inum, ii, err)) { errf("%s", err.c_str()); return 2; }
    const uint64_t fsize = ii.size;
    if (fsize < sizeof(Elf64Ehdr)) { errf("文件太小，不是 ELF64: %" PRIu64 "B", fsize); return 2; }

    Elf64Ehdr eh{};
    uint64_t got = 0;
    if (!read_exact(fs, inum, 0, sizeof(eh), &eh, &got, err) || got != sizeof(eh)) {
        errf("读取 ELF 头失败: %s", err.c_str());
        return 2;
    }
    bool ident_ok = eh.e_ident[0] == 0x7f && eh.e_ident[1] == 'E' &&
                    eh.e_ident[2] == 'L' && eh.e_ident[3] == 'F';
    const bool class64 = eh.e_ident[4] == 2;
    const bool le      = eh.e_ident[5] == 1;

    if (!g_json) {
        std::printf("ELF header:\n");
        std::printf("  magic=%s class=%s data=%s version=%u\n",
                    ident_ok ? "ELF" : "BAD", class64 ? "ELF64" : "BAD",
                    le ? "LSB" : "MSB", eh.e_ident[6]);
        std::printf("  e_type=%u%s  e_machine=%u%s  e_version=%u\n",
                    eh.e_type, eh.e_type == ET_CORE ? " (ET_CORE)" : "",
                    eh.e_machine, eh.e_machine == EM_X86_64 ? " (EM_X86_64)" : "",
                    eh.e_version);
        std::printf("  e_phoff=%" PRIu64 " e_phentsize=%u e_phnum=%u"
                    " e_ehsize=%u e_shoff=%" PRIu64 " e_shnum=%u\n",
                    eh.e_phoff, eh.e_phentsize, eh.e_phnum, eh.e_ehsize,
                    eh.e_shoff, eh.e_shnum);
    }

    bool hdr_ok = ident_ok && class64 && le && eh.e_type == ET_CORE &&
                  eh.e_machine == EM_X86_64 && eh.e_version == 1;
    if (!hdr_ok) {
        errf("不是 x86_64 CORE ELF64（class/data/type/machine/version 校验失败）");
        /* 仍然尝试打印 phdr（若表可用），但最终退出 1 */
    }
    if (eh.e_phnum > 4096) { errf("e_phnum=%u 异常", eh.e_phnum); return 2; }
    if (eh.e_phnum && eh.e_phentsize < sizeof(Elf64Phdr)) {
        errf("e_phentsize=%u 小于 Elf64_Phdr", eh.e_phentsize);
        return 2;
    }
    if (eh.e_phnum && eh.e_phoff + (uint64_t)eh.e_phnum * eh.e_phentsize > fsize) {
        errf("程序头表越界（e_phoff=%" PRIu64 " phnum=%u）", eh.e_phoff, eh.e_phnum);
        return 2;
    }

    std::vector<Elf64Phdr> phdrs;
    phdrs.reserve(eh.e_phnum);
    for (uint16_t i = 0; i < eh.e_phnum; ++i) {
        Elf64Phdr ph{};
        uint64_t o = eh.e_phoff + (uint64_t)i * eh.e_phentsize;
        uint64_t g = 0;
        if (!read_exact(fs, inum, o, sizeof(ph), &ph, &g, err) || g != sizeof(ph)) {
            errf("读取 phdr[%u] 失败: %s", i, err.c_str());
            return 2;
        }
        phdrs.push_back(ph);
    }

    uint64_t nload = 0;
    for (const auto &ph : phdrs) if (ph.p_type == PT_LOAD) ++nload;

    if (!g_json) {
        std::printf("program headers (%zu):\n", phdrs.size());
        for (size_t i = 0; i < phdrs.size(); ++i) {
            const Elf64Phdr &ph = phdrs[i];
            std::printf("  [%zu] type=%u%s p_offset=%" PRIu64 " p_vaddr=%#" PRIx64
                        " p_paddr=%#" PRIx64 " p_filesz=%" PRIu64 " p_memsz=%" PRIu64
                        " p_flags=%#" PRIx32 "\n",
                        i, ph.p_type, ph.p_type == PT_LOAD ? " (PT_LOAD)" :
                        (ph.p_type == PT_NOTE ? " (PT_NOTE)" : ""),
                        ph.p_offset, ph.p_vaddr, ph.p_paddr, ph.p_filesz, ph.p_memsz,
                        ph.p_flags);
        }
    }

    /* note 解析 */
    NoteSummary notes;
    if (!g_json) std::printf("notes:\n");
    for (const auto &ph : phdrs) {
        if (ph.p_type != PT_NOTE) continue;
        if (ph.p_filesz > (64ULL << 20)) {
            errf("PT_NOTE p_filesz=%" PRIu64 " 过大，跳过", ph.p_filesz);
            continue;
        }
        if (ph.p_offset + ph.p_filesz > fsize) {
            errf("PT_NOTE 越界（offset=%" PRIu64 " filesz=%" PRIu64 "）", ph.p_offset, ph.p_filesz);
            continue;
        }
        std::vector<uint8_t> nb((size_t)ph.p_filesz);
        if (ph.p_filesz && (!read_exact(fs, inum, ph.p_offset, ph.p_filesz, nb.data(), &got, err) ||
                            got != ph.p_filesz)) {
            errf("读取 PT_NOTE 失败: %s", err.c_str());
            continue;
        }
        std::string perr;
        if (!parse_notes(nb.data(), nb.size(), ph.p_offset, notes, perr)) {
            errf("解析 PT_NOTE 失败: %s", perr.c_str());
        }
    }

    const SparrowNote &sn = notes.sparrow;
    if (!sn.found) {
        if (!g_json) {
            std::printf("\nSPARROW note: 未找到（QEMU 原生 vmcore；digest 校验跳过）\n");
            std::printf("note 统计: total=%" PRIu64 " CORE/NT_PRSTATUS=%" PRIu64
                        " QEMU=%" PRIu64 "\n",
                        notes.total, notes.core_prstatus, notes.qemu);
        } else {
            std::printf("{\"sparrow\":{\"found\":false},\"notes\":{\"total\":%" PRIu64
                        ",\"core_prstatus\":%" PRIu64 ",\"qemu\":%" PRIu64 "},"
                        "\"pt_loads\":%" PRIu64 ",\"size\":%" PRIu64 "}\n",
                        notes.total, notes.core_prstatus, notes.qemu, nload, fsize);
        }
        return hdr_ok ? 0 : 2;
    }

    const SparrowDesc &d = sn.desc;
    if (!sn.desc_ok) {
        errf("SPARROW note desc 不足 64B，无法校验");
        return 2;
    }
    std::string magic;
    for (int i = 0; i < 8; ++i) {
        char c = (char)((d.magic >> (i * 8)) & 0xff);
        magic.push_back(c >= 0x20 && c < 0x7f ? c : '?');
    }
    if (!g_json) {
        std::printf("\nSPARROW note @%#" PRIx64 ":\n", sn.note_off);
        std::printf("  magic=%s (%#" PRIx64 ") version=%u flags=%#" PRIx32
                    " [complete=%u verified=%u regs=%u]\n",
                    magic.c_str(), d.magic, d.version, d.flags,
                    (d.flags & 1u) ? 1u : 0u, (d.flags & 2u) ? 1u : 0u,
                    (d.flags & 4u) ? 1u : 0u);
        std::printf("  total_bytes=%" PRIu64 " dram_top=%#" PRIx64
                    " segments=%" PRIu64 " data_offset=%" PRIu64 "\n",
                    d.total_bytes, d.dram_top, d.segments, d.data_offset);
        std::printf("  layout_xxh64=%016" PRIx64 " data_xxh64=%016" PRIx64 "\n",
                    d.layout_xxh64, d.data_xxh64);
    }

    bool verify_ok = true;
    bool digest_ok = true, layout_ok = true, meta_ok = true;
    uint64_t calc_data = 0, calc_layout = 0;
    uint64_t first_load_off = 0;
    bool have_first_load = false;
    for (const auto &ph : phdrs) {
        if (ph.p_type != PT_LOAD) continue;
        if (!have_first_load) { first_load_off = ph.p_offset; have_first_load = true; }
    }

    if (magic != "SPWFMD01" || d.version != 1) {
        errf("SPARROW magic/version 不符");
        verify_ok = false;
        meta_ok = false;
    }
    if (d.total_bytes != fsize) {
        errf("total_bytes=%" PRIu64 " != 文件大小 %" PRIu64, d.total_bytes, fsize);
        verify_ok = false;
        meta_ok = false;
    }
    if (d.segments != nload) {
        errf("segments=%" PRIu64 " != PT_LOAD 数量 %" PRIu64, d.segments, nload);
        verify_ok = false;
        meta_ok = false;
    }
    if (!have_first_load || d.data_offset != first_load_off) {
        errf("data_offset=%" PRIu64 " != 第一个 PT_LOAD.p_offset=%" PRIu64,
             d.data_offset, first_load_off);
        verify_ok = false;
        meta_ok = false;
    }

    if (verify) {
        /* data_xxh64：phdr 顺序拼接所有 PT_LOAD 的 p_filesz 字节 */
        Xxh64 dh;
        uint64_t hashed = 0;
        uint64_t next_progress = 256ULL << 20;
        for (const auto &ph : phdrs) {
            if (ph.p_type != PT_LOAD) continue;
            if (ph.p_offset + ph.p_filesz > fsize) {
                errf("PT_LOAD 越界（offset=%" PRIu64 " filesz=%" PRIu64 "）", ph.p_offset, ph.p_filesz);
                digest_ok = false;
                break;
            }
            std::string serr;
            bool ok = stream_file(fs, inum, ph.p_offset, ph.p_filesz,
                                  [&](const uint8_t *p, size_t n, uint64_t) {
                                      dh.update(p, n);
                                      hashed += n;
                                      if (hashed >= next_progress) {
                                          std::fprintf(stderr, "  data hash progress: %" PRIu64
                                                       " MiB\n", hashed >> 20);
                                          next_progress += 256ULL << 20;
                                      }
                                      return true;
                                  }, serr);
            if (!ok) {
                errf("data digest 读取失败: %s", serr.c_str());
                digest_ok = false;
                break;
            }
        }
        calc_data = digest_ok ? dh.digest() : 0;
        if (calc_data != d.data_xxh64) digest_ok = false;
        if (!g_json) {
            std::printf("  data_xxh64   : %s (calc=%016" PRIx64 ", note=%016" PRIx64
                        ", %" PRIu64 " bytes)\n",
                        digest_ok ? "OK" : "FAIL", calc_data, d.data_xxh64, hashed);
        }

        /* layout_xxh64：[0, SPARROW note 起始偏移) */
        Xxh64 lh;
        std::string lerr;
        bool lok = true;
        if (sn.note_off > fsize) {
            lok = false;
            lerr = "note_off 越界";
        } else {
            lok = stream_file(fs, inum, 0, sn.note_off,
                              [&](const uint8_t *p, size_t n, uint64_t) {
                                  lh.update(p, n);
                                  return true;
                              }, lerr);
        }
        calc_layout = lok ? lh.digest() : 0;
        layout_ok = lok && calc_layout == d.layout_xxh64;
        if (!g_json) {
            std::printf("  layout_xxh64 : %s (calc=%016" PRIx64 ", note=%016" PRIx64
                        ", %" PRIu64 " bytes)\n",
                        layout_ok ? "OK" : "FAIL", calc_layout, d.layout_xxh64, sn.note_off);
            if (!lok) std::printf("    layout 读取失败: %s\n", lerr.c_str());
        }
        verify_ok = verify_ok && digest_ok && layout_ok;
    }

    if (g_json) {
        std::printf("{\"size\":%" PRIu64 ",\"pt_loads\":%" PRIu64
                    ",\"notes\":{\"total\":%" PRIu64 ",\"core_prstatus\":%" PRIu64
                    ",\"qemu\":%" PRIu64 "},"
                    "\"sparrow\":{\"found\":true,\"note_offset\":%" PRIu64
                    ",\"magic\":\"%s\",\"version\":%u,\"flags\":%u,"
                    "\"total_bytes\":%" PRIu64 ",\"dram_top\":%" PRIu64
                    ",\"segments\":%" PRIu64 ",\"data_offset\":%" PRIu64
                    ",\"layout_xxh64\":\"%016" PRIx64 "\",\"data_xxh64\":\"%016" PRIx64 "\"",
                    fsize, nload, notes.total, notes.core_prstatus, notes.qemu,
                    sn.note_off, magic.c_str(), d.version, d.flags, d.total_bytes,
                    d.dram_top, d.segments, d.data_offset, d.layout_xxh64, d.data_xxh64);
        if (verify) {
            std::printf(",\"verify\":{\"ok\":%s,\"meta_ok\":%s,\"data_ok\":%s,"
                        "\"layout_ok\":%s,\"calc_data_xxh64\":\"%016" PRIx64 "\","
                        "\"calc_layout_xxh64\":\"%016" PRIx64 "\"}",
                        verify_ok ? "true" : "false", meta_ok ? "true" : "false",
                        digest_ok ? "true" : "false", layout_ok ? "true" : "false",
                        calc_data, calc_layout);
        }
        std::printf("}}\n");
    } else if (verify) {
        std::printf("verify: %s (meta=%s data=%s layout=%s)\n",
                    verify_ok ? "PASS" : "FAIL", meta_ok ? "OK" : "FAIL",
                    digest_ok ? "OK" : "FAIL", layout_ok ? "OK" : "FAIL");
    }
    return verify ? (verify_ok ? 0 : 1) : (hdr_ok ? 0 : 2);
}

/* ═══════════════════════════════════════════════════════════════════
 * mkdir
 * ═══════════════════════════════════════════════════════════════════ */
static int cmd_mkdir(RavenFS_juvenile &fs, const std::vector<std::string> &args)
{
    bool parents = false;
    std::string path;
    for (const auto &a : args) {
        if (a == "-p" || a == "--parents") parents = true;
        else if (!a.empty() && a[0] == '-' && a != "-") { errf("mkdir: 未知选项 %s", a.c_str()); return 2; }
        else if (path.empty()) path = a;
        else { errf("mkdir: 多余参数 %s", a.c_str()); return 2; }
    }
    if (path.empty()) { errf("用法: mkdir [-p] <path>"); return 2; }

    std::vector<std::string> comps;
    std::string err;
    if (!split_path(path, comps, err)) { errf("%s", err.c_str()); return 2; }
    if (comps.empty()) {
        if (parents) return 0;                 /* mkdir -p / 幂等成功 */
        errf("mkdir: / 已存在"); return 2;
    }

    uint64_t cur = fs.root_inum;
    std::string prefix;
    bool created_any = false;
    for (size_t i = 0; i < comps.size(); ++i) {
        const std::string &c = comps[i];
        InodeInfo ii;
        if (!read_inode(fs, cur, ii) || !ii.is_dir()) {
            errf("mkdir: 父路径不是目录: %s", (prefix.empty() ? "/" : prefix).c_str());
            return 2;
        }
        if (!need_perm(ii, 1, prefix.empty() ? "/" : prefix, "进入目录")) return 2;
        uint64_t child = 0;
        result_code_t lr = fs.dir_lookup(cur, c.c_str(), &child);
        bool is_last = (i + 1 == comps.size());
        if (lr == RC_OK) {
            InodeInfo ci;
            if (!read_inode(fs, child, ci)) { errf("mkdir: 读取 %s 失败", c.c_str()); return 2; }
            if (!ci.is_dir()) {
                errf("mkdir: %s 已存在且不是目录", ((prefix.empty() ? "/" : prefix) + c).c_str());
                return 2;
            }
            /* 已存在且是目录：按契约直接成功（-p 只影响是否补建缺失的父级） */
            (void)is_last;
            cur = child;
        } else if (lr == RC_ERR_NOT_FOUND) {
            if (!is_last && !parents) {
                errf("mkdir: 父目录不存在: %s", ((prefix.empty() ? "/" : prefix) + c).c_str());
                return 2;
            }
            if (!need_perm(ii, 3, prefix.empty() ? "/" : prefix, "在目录内创建")) return 2;
            result_code_t ir = fs.dir_insert_dentry(cur, c.c_str(), INODE_TYPE_DIR);
            if (ir != RC_OK) {
                errf("mkdir: 创建 %s → %s", ((prefix.empty() ? "/" : prefix) + c).c_str(),
                     rc_name(ir));
                return 2;
            }
            uint64_t ni = 0;
            ir = fs.dir_lookup(cur, c.c_str(), &ni);
            if (ir != RC_OK) {
                errf("mkdir: 创建后 lookup %s → %s", c.c_str(), rc_name(ir));
                return 2;
            }
            cur = ni;
            created_any = true;
            stamp_owner(fs, ni);
        } else {
            errf("mkdir: lookup %s → %s", c.c_str(), rc_name(lr));
            return 2;
        }
        prefix += "/" + c;
    }
    std::printf("mkdir: %s %s (inum=%" PRIu64 ")\n",
                created_any ? "created" : "exists", path.c_str(), cur);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * put
 * ═══════════════════════════════════════════════════════════════════ */
static int cmd_put(RavenFS_juvenile &fs, const std::vector<std::string> &args)
{
    bool force = false, mkdirs = false;
    std::vector<std::string> pos;
    for (const auto &a : args) {
        if (a == "--force") force = true;
        else if (a == "--mkdirs") mkdirs = true;
        else if (!a.empty() && a[0] == '-' && a != "-") { errf("put: 未知选项 %s", a.c_str()); return 2; }
        else pos.push_back(a);
    }
    if (pos.size() != 2) { errf("用法: put <hostfile> <fspath> [--force] [--mkdirs]"); return 2; }
    const std::string &hostfile = pos[0];
    const std::string &fspath = pos[1];

    struct stat st;
    if (::stat(hostfile.c_str(), &st) != 0) {
        errf("stat %s 失败: %s", hostfile.c_str(), std::strerror(errno));
        return 2;
    }
    if (!S_ISREG(st.st_mode)) { errf("%s 不是普通文件", hostfile.c_str()); return 2; }

    int hfd = ::open(hostfile.c_str(), O_RDONLY);
    if (hfd < 0) { errf("open %s 失败: %s", hostfile.c_str(), std::strerror(errno)); return 2; }

    uint64_t parent = 0;
    std::string name, err;
    if (!ensure_parent(fs, fspath, mkdirs, parent, name, err)) {
        errf("put: %s", err.c_str());
        ::close(hfd);
        return 2;
    }
    InodeInfo pi;
    if (!read_inode(fs, parent, pi) || !pi.is_dir()) {
        errf("put: 父路径不是目录");
        ::close(hfd);
        return 2;
    }
    if (!need_perm(pi, 3, fspath, "在父目录创建")) { ::close(hfd); return 2; }

    uint64_t inum = 0;
    bool created = false;
    result_code_t lr = fs.dir_lookup(parent, name.c_str(), &inum);
    if (lr == RC_OK) {
        InodeInfo ti;
        if (!read_inode(fs, inum, ti)) { errf("put: 读取目标 inode 失败"); ::close(hfd); return 2; }
        if (ti.is_dir()) { errf("put: 目标是目录: %s", fspath.c_str()); ::close(hfd); return 2; }
        if (!ti.is_reg()) { errf("put: 目标不是普通文件: %s", fspath.c_str()); ::close(hfd); return 2; }
        if (!force) {
            errf("put: 目标已存在: %s（加 --force 覆盖）", fspath.c_str());
            ::close(hfd);
            return 2;
        }
        if (!need_perm(ti, 2, fspath, "覆盖写")) { ::close(hfd); return 2; }
        result_code_t tr = fs.direct_file_trunacate(inum, 0);
        if (tr != RC_OK) {
            errf("put: truncate(%s, 0) → %s", fspath.c_str(), rc_name(tr));
            ::close(hfd);
            return 2;
        }
    } else if (lr == RC_ERR_NOT_FOUND) {
        result_code_t ir = fs.dir_insert_dentry(parent, name.c_str(), INODE_TYPE_REGULAR);
        if (ir != RC_OK) {
            errf("put: 创建 %s → %s", fspath.c_str(), rc_name(ir));
            ::close(hfd);
            return 2;
        }
        created = true;
        ir = fs.dir_lookup(parent, name.c_str(), &inum);
        if (ir != RC_OK) {
            errf("put: 创建后 lookup %s → %s", fspath.c_str(), rc_name(ir));
            ::close(hfd);
            return 2;
        }
        stamp_owner(fs, inum);
    } else {
        errf("put: lookup %s → %s", fspath.c_str(), rc_name(lr));
        ::close(hfd);
        return 2;
    }

    std::vector<uint8_t> buf(kChunk);
    uint64_t off = 0;
    bool ok = true;
    for (;;) {
        ssize_t n = ::read(hfd, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            err = std::string("read host 失败: ") + std::strerror(errno);
            ok = false;
            break;
        }
        if (n == 0) break;
        result_code_t rc = fs.direct_file_pwrite(inum, off, (uint64_t)n, buf.data());
        if (rc != RC_OK) {
            err = "pwrite(" + fspath + ", off=" + std::to_string(off) + ") → " + rc_name(rc);
            ok = false;
            break;
        }
        off += (uint64_t)n;
    }
    ::close(hfd);

    if (!ok) {
        errf("put %s: %s", fspath.c_str(), err.c_str());
        if (created) {
            /* 尽力清理半成品（失败也不掩盖原始错误） */
            result_code_t rr = fs.dir_rm_dentry(parent, name.c_str(), false);
            if (rr == RC_OK) std::fprintf(stderr, "  已回滚半成品目录项: %s\n", fspath.c_str());
            else std::fprintf(stderr, "  回滚半成品失败: %s\n", rc_name(rr));
        }
        return 2;
    }
    std::printf("put: %s → %s (%" PRIu64 " bytes, inum=%" PRIu64 ")\n",
                hostfile.c_str(), fspath.c_str(), off, inum);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * rm
 * ═══════════════════════════════════════════════════════════════════ */
static bool rm_recursive(RavenFS_juvenile &fs, uint64_t parent, const std::string &name,
                         bool shred, std::string &err)
{
    uint64_t inum = 0;
    result_code_t lr = fs.dir_lookup(parent, name.c_str(), &inum);
    if (lr != RC_OK) {
        err = "lookup(" + name + ") → " + rc_name(lr);
        return false;
    }
    InodeInfo ii;
    if (!read_inode(fs, inum, ii)) { err = "读取 inode 失败: " + name; return false; }
    if (ii.is_dir()) {
        std::vector<DirEntry> entries;
        std::string derr;
        if (!read_dir(fs, inum, entries, derr)) { err = derr; return false; }
        for (const auto &e : entries) {
            if (!rm_recursive(fs, inum, e.name, shred, err)) return false;
        }
        result_code_t rr = fs.dir_rm_dentry(parent, name.c_str(), shred);
        if (rr != RC_OK && rr != RC_ERR_NOT_SUPPORTED) {
            err = "rm_dentry(" + name + ") → " + std::string(rc_name(rr));
            return false;
        }
        if (rr == RC_ERR_NOT_SUPPORTED) {
            err = "rm_dentry(" + name + ") → RC_ERR_NOT_SUPPORTED（树删已生效，inode 孤儿待 fsck）";
            return false;
        }
        return true;
    }
    result_code_t rr = fs.dir_rm_dentry(parent, name.c_str(), shred);
    if (rr != RC_OK && rr != RC_ERR_NOT_SUPPORTED) {
        err = "rm_dentry(" + name + ") → " + std::string(rc_name(rr));
        return false;
    }
    if (rr == RC_ERR_NOT_SUPPORTED) {
        err = "rm_dentry(" + name + ") → RC_ERR_NOT_SUPPORTED（树删已生效，inode 孤儿待 fsck）";
        return false;
    }
    return true;
}

static int cmd_rm(RavenFS_juvenile &fs, const std::vector<std::string> &args)
{
    bool recursive = false, shred = false;
    std::string path;
    for (const auto &a : args) {
        if (a == "-r" || a == "-R" || a == "--recursive") recursive = true;
        else if (a == "--shred") shred = true;
        else if (a.size() > 1 && a[0] == '-' && a[1] != '-') {
            for (size_t i = 1; i < a.size(); ++i) {
                if (a[i] == 'r' || a[i] == 'R') recursive = true;
                else { errf("rm: 未知选项 -%c", a[i]); return 2; }
            }
        } else if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
            errf("rm: 未知选项 %s", a.c_str());
            return 2;
        } else if (path.empty()) {
            path = a;
        } else {
            errf("rm: 多余参数 %s", a.c_str());
            return 2;
        }
    }
    if (path.empty()) { errf("用法: rm [-r] [--shred] <path>"); return 2; }

    std::vector<std::string> comps;
    std::string err;
    if (!split_path(path, comps, err)) { errf("%s", err.c_str()); return 2; }
    if (comps.empty()) { errf("rm: 拒绝删除根目录 /"); return 2; }

    uint64_t parent = fs.root_inum;
    std::string prefix;
    for (size_t i = 0; i + 1 < comps.size(); ++i) {
        InodeInfo di;
        if (!read_inode(fs, parent, di) ||
            !need_perm(di, 1, prefix.empty() ? "/" : prefix, "进入目录")) return 2;
        uint64_t child = 0;
        result_code_t rc = fs.dir_lookup(parent, comps[i].c_str(), &child);
        if (rc != RC_OK) {
            errf("rm: %s → %s", path.c_str(), rc_name(rc));
            return 2;
        }
        InodeInfo ii;
        if (!read_inode(fs, child, ii) || !ii.is_dir()) {
            errf("rm: 父路径不是目录: %s", comps[i].c_str());
            return 2;
        }
        prefix += "/" + comps[i];
        parent = child;
    }
    {
        InodeInfo di;
        if (!read_inode(fs, parent, di) ||
            !need_perm(di, 3, prefix.empty() ? "/" : prefix, "删除目录项（需写+搜索）"))
            return 2;
    }
    const std::string &name = comps.back();
    uint64_t inum = 0;
    result_code_t lr = fs.dir_lookup(parent, name.c_str(), &inum);
    if (lr != RC_OK) {
        errf("rm: %s → %s", path.c_str(), rc_name(lr));
        return 2;
    }
    InodeInfo ii;
    if (!read_inode(fs, inum, ii)) { errf("rm: 读取 inode 失败"); return 2; }

    if (ii.is_dir() && !recursive) {
        errf("rm: %s 是目录（非空/空目录均需 -r）", path.c_str());
        return 2;
    }
    if (ii.is_dir() && !need_perm(ii, 4, path, "递归删除需列目录")) return 2;

    uint64_t before = fs.allocator ? fs.allocator->block_free_total() : 0;
    bool ok;
    if (ii.is_dir()) {
        ok = rm_recursive(fs, parent, name, shred, err);
    } else {
        result_code_t rr = fs.dir_rm_dentry(parent, name.c_str(), shred);
        ok = (rr == RC_OK);
        if (rr == RC_ERR_NOT_SUPPORTED) {
            err = "rm_dentry(" + name + ") → RC_ERR_NOT_SUPPORTED（树删已生效，inode 孤儿待 fsck）";
        } else if (rr != RC_OK) {
            err = "rm_dentry(" + name + ") → " + rc_name(rr);
        }
    }
    if (!ok) {
        errf("rm %s: %s", path.c_str(), err.c_str());
        return 2;
    }
    uint64_t after = fs.allocator ? fs.allocator->block_free_total() : 0;
    std::printf("rm: removed %s (%s)  free_blocks %" PRIu64 " → %" PRIu64
                " (+%" PRIu64 ")\n",
                path.c_str(), shred ? "shred" : "unlink", before, after,
                after >= before ? after - before : 0);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * cpout / cpin
 * ═══════════════════════════════════════════════════════════════════ */
static bool mkdir_hosts(const std::string &path, std::string &err)
{
    if (path.empty()) { err = "空 host 路径"; return false; }
    std::string cur;
    size_t i = 0;
    if (path[0] == '/') { cur = "/"; i = 1; }
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string c = path.substr(i, j - i);
        i = j + 1;
        if (c.empty()) continue;
        if (!cur.empty() && cur.back() != '/') cur += "/";
        cur += c;
        struct stat st;
        if (::stat(cur.c_str(), &st) == 0) {
            if (!S_ISDIR(st.st_mode)) { err = cur + " 已存在且不是目录"; return false; }
        } else {
            if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
                err = "mkdir " + cur + " 失败: " + std::strerror(errno);
                return false;
            }
        }
    }
    return true;
}

static bool write_host_file(RavenFS_juvenile &fs, uint64_t inum, const std::string &host,
                            bool force, std::string &err)
{
    struct stat st;
    if (::lstat(host.c_str(), &st) == 0) {
        if (!force) { err = host + " 已存在（加 --force 覆盖）"; return false; }
        if (S_ISDIR(st.st_mode)) { err = host + " 是目录"; return false; }
    }
    int fd = ::open(host.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (fd < 0) { err = "open " + host + " 失败: " + std::strerror(errno); return false; }
    bool ok = stream_file(fs, inum, 0, UINT64_MAX,
                          [&](const uint8_t *p, size_t n, uint64_t) {
                              size_t off = 0;
                              while (off < n) {
                                  ssize_t w = ::write(fd, p + off, n - off);
                                  if (w < 0) {
                                      if (errno == EINTR) continue;
                                      err = std::string("write 失败: ") + std::strerror(errno);
                                      return false;
                                  }
                                  off += (size_t)w;
                              }
                              return true;
                          }, err);
    if (ok && ::fsync(fd) != 0) {
        err = std::string("fsync 失败: ") + std::strerror(errno);
        ok = false;
    }
    ::close(fd);
    return ok;
}

/* 把 FS 目录的“内容”导出到 hostdir（hostdir 已存在）。 */
static bool cpout_dir_contents(RavenFS_juvenile &fs, uint64_t dir_inum,
                               const std::string &hostdir, bool force,
                               const std::string &display, std::string &err)
{
    std::vector<DirEntry> entries;
    if (!read_dir(fs, dir_inum, entries, err)) return false;
    for (const auto &e : entries) {
        InodeInfo ii;
        if (!read_inode(fs, e.inum, ii)) {
            err = "读取 inode 失败: " + display + "/" + e.name;
            return false;
        }
        if (ii.is_sym()) {
            err = "symlink 不支持导出（god pread 不受理 SOFT_SYM）: " +
                  display + "/" + e.name;
            return false;
        }
        std::string host = hostdir;
        if (!host.empty() && host.back() != '/') host += "/";
        host += e.name;
        const std::string child_disp = display.empty() ? e.name : display + "/" + e.name;
        if (ii.is_dir()) {
            if (!mkdir_hosts(host, err)) return false;
            std::printf("  %s/ → %s/\n", child_disp.c_str(), host.c_str());
            if (!cpout_dir_contents(fs, e.inum, host, force, child_disp, err))
                return false;
        } else {
            if (!write_host_file(fs, e.inum, host, force, err)) return false;
            std::printf("  %s → %s (%" PRIu64 " bytes)\n",
                        child_disp.c_str(), host.c_str(), ii.size);
        }
    }
    return true;
}

static bool cpout_file(RavenFS_juvenile &fs, uint64_t inum, const std::string &host,
                       bool force, const std::string &display, uint64_t size,
                       std::string &err)
{
    InodeInfo ii;
    if (!read_inode(fs, inum, ii)) { err = "读取 inode 失败: " + display; return false; }
    if (ii.is_sym()) {
        err = "symlink 不支持导出（god pread 不受理 SOFT_SYM）: " + display;
        return false;
    }
    if (ii.is_dir()) { err = display + " 是目录"; return false; }
    if (!write_host_file(fs, inum, host, force, err)) return false;
    std::printf("  %s → %s (%" PRIu64 " bytes)\n", display.c_str(), host.c_str(), size);
    return true;
}

static int cmd_cpout(RavenFS_juvenile &fs, const std::vector<std::string> &args)
{
    bool force = false;
    std::vector<std::string> pos;
    for (const auto &a : args) {
        if (a == "--force") force = true;
        else if (!a.empty() && a[0] == '-' && a != "-") { errf("cpout: 未知选项 %s", a.c_str()); return 2; }
        else pos.push_back(a);
    }
    if (pos.size() != 2) { errf("用法: cpout <fspath> <hostdir> [--force]"); return 2; }
    const std::string &fspath = pos[0];
    const std::string &hostdir = pos[1];

    uint64_t inum = 0;
    std::string err;
    Lookup lr = lookup_path(fs, fspath, inum, err);
    if (lr != Lookup::Ok) { errf("%s", err.c_str()); return 2; }
    InodeInfo ii;
    if (!read_inode(fs, inum, ii)) { errf("读取 inode 失败"); return 2; }
    if (!need_perm(ii, ii.is_dir() ? 5 : 4, fspath, "导出")) return 2;

    if (!mkdir_hosts(hostdir, err)) { errf("cpout: %s", err.c_str()); return 2; }

    if (ii.is_dir()) {
        std::printf("cpout: %s → %s/ (contents)\n", fspath.c_str(), hostdir.c_str());
        if (!cpout_dir_contents(fs, inum, hostdir, force, "", err)) {
            errf("cpout: %s", err.c_str());
            return 2;
        }
    } else {
        /* 文件：导出为 <hostdir>/<basename> */
        std::string base = fspath;
        while (!base.empty() && base.back() == '/') base.pop_back();
        size_t p = base.find_last_of('/');
        if (p != std::string::npos) base = base.substr(p + 1);
        if (base.empty()) { errf("cpout: 无法得出 basename: %s", fspath.c_str()); return 2; }
        std::string host = hostdir;
        if (!host.empty() && host.back() != '/') host += "/";
        host += base;
        std::printf("cpout: %s → %s\n", fspath.c_str(), host.c_str());
        if (!cpout_file(fs, inum, host, force, base, ii.size, err)) {
            errf("cpout: %s", err.c_str());
            return 2;
        }
    }
    return 0;
}

/* host 目录内容 → FS 目录（存在则合并；--force 覆盖同名文件） */
static bool cpin_host_dir(RavenFS_juvenile &fs, const std::string &hostdir,
                          uint64_t fsinum, bool force, std::string &err);

static bool cpin_host_entry(RavenFS_juvenile &fs, const std::string &hostpath,
                            const std::string &name, uint64_t fsparent, bool force,
                            std::string &err)
{
    {
        InodeInfo pi;
        if (!read_inode(fs, fsparent, pi) || !pi.is_dir()) {
            err = "导入目标父 inode 不是目录";
            return false;
        }
        if (!need_perm(pi, 3, name, "导入（父目录需写+搜索）")) return false;
    }
    struct stat st;
    if (::lstat(hostpath.c_str(), &st) != 0) {
        err = "lstat " + hostpath + " 失败: " + std::strerror(errno);
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        std::fprintf(stderr, "rfs_admin: cpin: 跳过 symlink（不跟随）: %s\n", hostpath.c_str());
        return true;
    }
    if (S_ISDIR(st.st_mode)) {
        InodeInfo ci;
        uint64_t child = 0;
        result_code_t lr = fs.dir_lookup(fsparent, name.c_str(), &child);
        if (lr == RC_ERR_NOT_FOUND) {
            result_code_t ir = fs.dir_insert_dentry(fsparent, name.c_str(), INODE_TYPE_DIR);
            if (ir != RC_OK) { err = "mkdir " + name + " → " + rc_name(ir); return false; }
            ir = fs.dir_lookup(fsparent, name.c_str(), &child);
            if (ir != RC_OK) { err = "lookup " + name + " → " + rc_name(ir); return false; }
            stamp_owner(fs, child);
        } else if (lr != RC_OK) {
            err = "lookup " + name + " → " + rc_name(lr);
            return false;
        } else {
            if (!read_inode(fs, child, ci)) { err = "读取 inode 失败: " + name; return false; }
            if (!ci.is_dir()) { err = "目标同名但不是目录: " + name; return false; }
        }
        return cpin_host_dir(fs, hostpath, child, force, err);
    }
    if (!S_ISREG(st.st_mode)) {
        std::fprintf(stderr, "rfs_admin: cpin: 跳过非普通文件（fifo/socket/device）: %s\n",
                     hostpath.c_str());
        return true;
    }
    if (name.size() > RAVENFS_MAX_NAME_LEN) {
        err = "文件名超过 " + std::to_string(RAVENFS_MAX_NAME_LEN) + "B: " + name;
        return false;
    }

    uint64_t inum = 0;
    result_code_t lr = fs.dir_lookup(fsparent, name.c_str(), &inum);
    bool created = false;
    if (lr == RC_OK) {
        InodeInfo ti;
        if (!read_inode(fs, inum, ti)) { err = "读取 inode 失败: " + name; return false; }
        if (ti.is_dir()) { err = "目标同名但是目录: " + name; return false; }
        if (!ti.is_reg()) { err = "目标同名但不是普通文件: " + name; return false; }
        if (!force) { err = "目标已存在（加 --force 覆盖）: " + name; return false; }
        if (!need_perm(ti, 2, name, "覆盖写")) return false;
        result_code_t tr = fs.direct_file_trunacate(inum, 0);
        if (tr != RC_OK) { err = "truncate " + name + " → " + rc_name(tr); return false; }
    } else if (lr == RC_ERR_NOT_FOUND) {
        result_code_t ir = fs.dir_insert_dentry(fsparent, name.c_str(), INODE_TYPE_REGULAR);
        if (ir != RC_OK) { err = "create " + name + " → " + rc_name(ir); return false; }
        created = true;
        ir = fs.dir_lookup(fsparent, name.c_str(), &inum);
        if (ir != RC_OK) { err = "lookup " + name + " → " + rc_name(ir); return false; }
        stamp_owner(fs, inum);
    } else {
        err = "lookup " + name + " → " + rc_name(lr);
        return false;
    }

    int hfd = ::open(hostpath.c_str(), O_RDONLY | O_NOFOLLOW);
    if (hfd < 0) { err = "open " + hostpath + " 失败: " + std::strerror(errno); return false; }
    std::vector<uint8_t> buf(kChunk);
    uint64_t off = 0;
    bool ok = true;
    for (;;) {
        ssize_t n = ::read(hfd, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            err = "read " + hostpath + " 失败: " + std::strerror(errno);
            ok = false;
            break;
        }
        if (n == 0) break;
        result_code_t rc = fs.direct_file_pwrite(inum, off, (uint64_t)n, buf.data());
        if (rc != RC_OK) {
            err = "pwrite(" + name + ", off=" + std::to_string(off) + ") → " + rc_name(rc);
            ok = false;
            break;
        }
        off += (uint64_t)n;
    }
    ::close(hfd);
    if (!ok) {
        if (created) {
            result_code_t rr = fs.dir_rm_dentry(fsparent, name.c_str(), false);
            if (rr != RC_OK) std::fprintf(stderr, "  回滚 %s 失败: %s\n", name.c_str(), rc_name(rr));
        }
        return false;
    }
    std::printf("  %s → %s (%" PRIu64 " bytes)\n", hostpath.c_str(), name.c_str(), off);
    return true;
}

static bool cpin_host_dir(RavenFS_juvenile &fs, const std::string &hostdir,
                          uint64_t fsinum, bool force, std::string &err)
{
    DIR *d = ::opendir(hostdir.c_str());
    if (!d) { err = "opendir " + hostdir + " 失败: " + std::strerror(errno); return false; }
    std::vector<std::string> names;
    struct dirent *de;
    while ((de = ::readdir(d)) != nullptr) {
        std::string n = de->d_name;
        if (n == "." || n == "..") continue;
        names.push_back(n);
    }
    ::closedir(d);
    std::sort(names.begin(), names.end());
    for (const auto &n : names) {
        std::string hp = hostdir;
        if (!hp.empty() && hp.back() != '/') hp += "/";
        hp += n;
        /* symlink/特殊文件：报告并继续（不静默跳过） */
        std::string e2;
        if (!cpin_host_entry(fs, hp, n, fsinum, force, e2)) {
            errf("cpin: %s", e2.c_str());
            err = e2;
            return false;
        }
    }
    return true;
}

static int cmd_cpin(RavenFS_juvenile &fs, const std::vector<std::string> &args)
{
    bool force = false;
    std::vector<std::string> pos;
    for (const auto &a : args) {
        if (a == "--force") force = true;
        else if (!a.empty() && a[0] == '-' && a != "-") { errf("cpin: 未知选项 %s", a.c_str()); return 2; }
        else pos.push_back(a);
    }
    if (pos.size() != 2) { errf("用法: cpin <hostdir> <fspath> [--force]"); return 2; }
    const std::string &hostdir = pos[0];
    const std::string &fspath = pos[1];

    struct stat st;
    if (::stat(hostdir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        errf("cpin: %s 不是目录", hostdir.c_str());
        return 2;
    }

    uint64_t inum = 0;
    std::string err;
    Lookup lr = lookup_path(fs, fspath, inum, err);
    if (lr == Lookup::NotFound) {
        uint64_t parent = 0;
        std::string name;
        if (!ensure_parent(fs, fspath, true, parent, name, err)) {
            errf("cpin: %s", err.c_str());
            return 2;
        }
        result_code_t ir = fs.dir_insert_dentry(parent, name.c_str(), INODE_TYPE_DIR);
        if (ir != RC_OK) { errf("cpin: mkdir %s → %s", fspath.c_str(), rc_name(ir)); return 2; }
        ir = fs.dir_lookup(parent, name.c_str(), &inum);
        if (ir != RC_OK) { errf("cpin: lookup %s → %s", fspath.c_str(), rc_name(ir)); return 2; }
    } else if (lr != Lookup::Ok) {
        errf("%s", err.c_str());
        return 2;
    } else {
        InodeInfo ii;
        if (!read_inode(fs, inum, ii)) { errf("cpin: 读取 inode 失败"); return 2; }
        if (!ii.is_dir()) { errf("cpin: %s 已存在且不是目录", fspath.c_str()); return 2; }
    }

    std::printf("cpin: %s/ → %s (contents)\n", hostdir.c_str(), fspath.c_str());
    if (!cpin_host_dir(fs, hostdir, inum, force, err)) {
        errf("cpin: %s", err.c_str());
        return 2;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * truncate / find
 * ═══════════════════════════════════════════════════════════════════ */
/* ═══════════════════════════════════════════════════════════════════
 * chmod / chown —— 属主与权限位（身份层的“道具”）
 * 默认（未给 --uid/--gid）就是 root，可随意改；给了身份则按 VFS：
 *   chmod 需属主或 root；chown 仅 root。
 * ═══════════════════════════════════════════════════════════════════ */
static int cmd_chmod(RavenFS_juvenile &fs, const std::vector<std::string> &args)
{
    if (args.size() != 2) { errf("用法: chmod <八进制mode> <path>"); return 2; }
    char *end = nullptr;
    const unsigned long mode = std::strtoul(args[0].c_str(), &end, 8);
    if (end == args[0].c_str() || *end != '\0' || mode > 07777) {
        errf("chmod: 非法 mode: %s（八进制，如 644 / 600 / 755）", args[0].c_str());
        return 2;
    }
    const std::string &path = args[1];
    uint64_t inum = 0;
    std::string err;
    Lookup lr = lookup_path(fs, path, inum, err);
    if (lr != Lookup::Ok) { errf("%s", err.c_str()); return 2; }
    InodeInfo ii;
    if (!read_inode(fs, inum, ii)) { errf("chmod: 读取 inode 失败"); return 2; }
    if (g_cred_enforce && g_cred_uid != 0 && g_cred_uid != ii.uid) {
        errf("%s: 权限不足（chmod 需属主或 root；属主 %u:%u，当前身份 %u:%u）",
             path.c_str(), ii.uid, ii.gid, g_cred_uid, g_cred_gid);
        return 2;
    }
    InodeSession s(fs.inode_io, inum);
    if (!s) { errf("chmod: 无法打开 inode 会话"); return 2; }
    s.inode->mode_xattr = RFS_MODE_XATTR_PACK(mode, RFS_XATTRPTR_GET(s.inode->mode_xattr));
    s.dirty();
    std::printf("chmod: %s %04o → %04lo\n", path.c_str(), ii.mode(), mode);
    return 0;
}

static int cmd_chown(RavenFS_juvenile &fs, const std::vector<std::string> &args)
{
    if (args.size() != 2) { errf("用法: chown <uid>:<gid> <path>"); return 2; }
    const std::string &own = args[0];
    size_t colon = own.find(':');
    if (colon == std::string::npos) { errf("chown: 需要 uid:gid 形式: %s", own.c_str()); return 2; }
    const std::string uid_s = own.substr(0, colon);
    const std::string gid_s = own.substr(colon + 1);
    char *e1 = nullptr, *e2 = nullptr;
    const unsigned long uid = std::strtoul(uid_s.c_str(), &e1, 10);
    const unsigned long gid = std::strtoul(gid_s.c_str(), &e2, 10);
    if (uid_s.empty() || gid_s.empty() || *e1 != '\0' || *e2 != '\0' ||
        uid > 0xffffffffUL || gid > 0xffffffffUL) {
        errf("chown: 非法 uid:gid: %s", own.c_str());
        return 2;
    }
    if (g_cred_enforce && g_cred_uid != 0) {
        errf("chown: 权限不足（只有 root 能改属主；当前身份 %u:%u）",
             g_cred_uid, g_cred_gid);
        return 2;
    }
    const std::string &path = args[1];
    uint64_t inum = 0;
    std::string err;
    Lookup lr = lookup_path(fs, path, inum, err);
    if (lr != Lookup::Ok) { errf("%s", err.c_str()); return 2; }
    InodeInfo ii;
    if (!read_inode(fs, inum, ii)) { errf("chown: 读取 inode 失败"); return 2; }
    InodeSession s(fs.inode_io, inum);
    if (!s) { errf("chown: 无法打开 inode 会话"); return 2; }
    s.inode->uid = (uint32_t)uid;
    s.inode->gid = (uint32_t)gid;
    s.dirty();
    std::printf("chown: %s %u:%u → %lu:%lu\n", path.c_str(), ii.uid, ii.gid, uid, gid);
    return 0;
}

static int cmd_truncate(RavenFS_juvenile &fs, const std::vector<std::string> &args)
{
    std::string path;
    uint64_t size = 0;
    bool have_size = false;
    for (const auto &a : args) {
        if (!a.empty() && a[0] == '-' && a != "-") { errf("truncate: 未知选项 %s", a.c_str()); return 2; }
        if (!have_size && path.empty()) path = a;
        else if (!have_size) { size = std::strtoull(a.c_str(), nullptr, 0); have_size = true; }
        else { errf("truncate: 多余参数 %s", a.c_str()); return 2; }
    }
    if (path.empty() || !have_size) { errf("用法: truncate <path> <size>"); return 2; }
    uint64_t inum = 0;
    InodeInfo ii;
    std::string err;
    if (!open_fs_file(fs, path, inum, ii, err)) { errf("%s", err.c_str()); return 2; }
    if (!need_perm(ii, 2, path, "截断")) return 2;
    result_code_t rc = fs.direct_file_trunacate(inum, size);
    if (rc != RC_OK) { errf("truncate %s → %s", path.c_str(), rc_name(rc)); return 2; }
    std::printf("truncate: %s %" PRIu64 " → %" PRIu64 " bytes\n", path.c_str(), ii.size, size);
    return 0;
}

static void find_walk(RavenFS_juvenile &fs, uint64_t inum, const std::string &path,
                      const std::string &substr, int depth, std::set<uint64_t> &visited)
{
    if (depth > 128) return;
    {
        InodeInfo di;
        if (!read_inode(fs, inum, di) || !need_perm(di, 4, path, "遍历目录")) return;
    }
    std::vector<DirEntry> entries;
    std::string err;
    if (!read_dir(fs, inum, entries, err)) {
        errf("find %s: %s", path.c_str(), err.c_str());
        return;
    }
    for (const auto &e : entries) {
        std::string cpath = (path == "/" ? "" : path) + "/" + e.name;
        if (substr.empty() || e.name.find(substr) != std::string::npos) {
            InodeInfo ii;
            read_inode(fs, e.inum, ii);
            if (g_json) {
                std::printf("{\"path\":\"%s\",\"inum\":%" PRIu64 ",\"type\":\"%s\","
                            "\"size\":%" PRIu64 "}\n",
                            json_escape(cpath).c_str(), e.inum, type_name(ii.type), ii.size);
            } else {
                std::printf("%s\n", cpath.c_str());
            }
        }
        InodeInfo ii;
        if (read_inode(fs, e.inum, ii) && ii.is_dir()) {
            if (!visited.insert(e.inum).second) continue;
            find_walk(fs, e.inum, cpath, substr, depth + 1, visited);
        }
    }
}

static int cmd_find(RavenFS_juvenile &fs, const std::vector<std::string> &args)
{
    std::string path = "/";
    std::string substr;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--name") {
            if (i + 1 >= args.size()) { errf("find: --name 需要参数"); return 2; }
            substr = args[++i];
        } else if (args[i].rfind("--name=", 0) == 0) {
            substr = args[i].c_str() + 7;
        } else if (!args[i].empty() && args[i][0] == '-' && args[i] != "-") {
            errf("find: 未知选项 %s", args[i].c_str());
            return 2;
        } else {
            path = args[i];
        }
    }
    uint64_t inum = 0;
    std::string err;
    Lookup lr = lookup_path(fs, path, inum, err);
    if (lr != Lookup::Ok) { errf("%s", err.c_str()); return 2; }
    InodeInfo ii;
    if (!read_inode(fs, inum, ii)) { errf("find: 读取 inode 失败"); return 2; }
    std::set<uint64_t> visited;
    visited.insert(inum);
    if (!ii.is_dir()) {
        if (substr.empty() || path.find(substr) != std::string::npos) {
            if (g_json) {
                std::printf("{\"path\":\"%s\",\"inum\":%" PRIu64 ",\"type\":\"%s\","
                            "\"size\":%" PRIu64 "}\n",
                            json_escape(path).c_str(), inum, type_name(ii.type), ii.size);
            } else {
                std::printf("%s\n", path.c_str());
            }
        }
        return 0;
    }
    find_walk(fs, inum, path, substr, 0, visited);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════════ */
static bool command_is_write(const std::string &cmd)
{
    return cmd == "mkdir" || cmd == "put" || cmd == "rm" ||
           cmd == "cpin" || cmd == "truncate" ||
           cmd == "chmod" || cmd == "chown";
}

int main(int argc, char **argv)
{
    if (argc < 2) usage(2);
    if (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) usage(0);

    const std::string image = argv[1];

    /* --uid/--gid 可出现在镜像名后任意位置；两种写法：--uid N / --uid=N。
     * 允许负号前缀的命令参数（如 rm -r）不受影响：只截获这两个精确选项。 */
    auto parse_id = [](const std::string &s, uint32_t &out) -> bool {
        if (s.empty()) return false;
        char *end = nullptr;
        errno = 0;
        unsigned long v = std::strtoul(s.c_str(), &end, 10);
        if (errno || end == s.c_str() || *end != '\0' || v > 0xffffffffUL) return false;
        out = (uint32_t)v;
        return true;
    };

    std::vector<std::string> toks;
    bool have_uid = false, have_gid = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--readonly") { g_readonly_flag = true; continue; }
        if (a == "--json") { g_json = true; continue; }
        if (a == "--uid" || a == "-u") {
            if (i + 1 >= argc || !parse_id(argv[++i], g_cred_uid)) {
                errf("--uid 需要非负整数"); return 2;
            }
            have_uid = true;
            continue;
        }
        if (a == "--gid" || a == "-g") {
            if (i + 1 >= argc || !parse_id(argv[++i], g_cred_gid)) {
                errf("--gid 需要非负整数"); return 2;
            }
            have_gid = true;
            continue;
        }
        if (a.rfind("--uid=", 0) == 0) {
            if (!parse_id(a.substr(6), g_cred_uid)) { errf("--uid 需要非负整数"); return 2; }
            have_uid = true;
            continue;
        }
        if (a.rfind("--gid=", 0) == 0) {
            if (!parse_id(a.substr(6), g_cred_gid)) { errf("--gid 需要非负整数"); return 2; }
            have_gid = true;
            continue;
        }
        toks.push_back(std::move(a));
    }
    /* 只有“显式给出了身份”才开 DAC；只给 --uid 时 gid 保持 0（进程主组缺省）。 */
    g_cred_enforce = have_uid || have_gid;
    if (toks.empty() || toks[0] == "--help" || toks[0] == "-h" || toks[0] == "help")
        usage(toks.empty() ? 2 : 0);

    const std::string cmd = toks[0];
    std::vector<std::string> args(toks.begin() + 1, toks.end());

    const bool is_write = command_is_write(cmd);
    if (g_readonly_flag && is_write) {
        errf("命令 '%s' 是写命令，不能与 --readonly 同时使用", cmd.c_str());
        return 2;
    }
    const bool readonly = !is_write || g_readonly_flag;

    FsHandle h;
    std::string err;
    if (!mount_image(image, readonly, h, err)) {
        errf("%s", err.c_str());
        return 2;
    }
    RavenFS_juvenile &fs = *h.fs;

    int rc = 0;
    if (cmd == "info") rc = cmd_info(fs);
    else if (cmd == "df") rc = cmd_df(fs);
    else if (cmd == "ls") rc = cmd_ls(fs, args);
    else if (cmd == "stat") rc = cmd_stat(fs, args);
    else if (cmd == "tree") rc = cmd_tree(fs, args);
    else if (cmd == "cat") rc = cmd_cat(fs, args);
    else if (cmd == "get") rc = cmd_get(fs, args);
    else if (cmd == "hash") rc = cmd_hash(fs, args);
    else if (cmd == "audit") rc = cmd_audit(fs, args);
    else if (cmd == "vmcore") rc = cmd_vmcore(fs, args);
    else if (cmd == "find") rc = cmd_find(fs, args);
    else if (cmd == "mkdir") rc = cmd_mkdir(fs, args);
    else if (cmd == "put") rc = cmd_put(fs, args);
    else if (cmd == "rm") rc = cmd_rm(fs, args);
    else if (cmd == "cpout") rc = cmd_cpout(fs, args);
    else if (cmd == "cpin") rc = cmd_cpin(fs, args);
    else if (cmd == "truncate") rc = cmd_truncate(fs, args);
    else if (cmd == "chmod") rc = cmd_chmod(fs, args);
    else if (cmd == "chown") rc = cmd_chown(fs, args);
    else {
        errf("未知命令: %s", cmd.c_str());
        usage(2);
    }

    /* 正常离开作用域 = 提交（写挂载 all_sycn + 位图回写 + SB 时间戳）。 */
    close_fs(h);
    std::fflush(stdout);
    return rc;
}
