#pragma once
/*
 * sbinfo 公共设施（多 TU 共享）。
 *
 * 为何拆 TU：v1.x 盘面头（disk_struct_v1.2.h）与 v2.0 盘面头（disk_struct_v2.0.h）
 *   `RavenSuperblock` 等类型**重名且布局不同**，不能同 TU include。故 sbinfo 按
 *   SB magic 分派：v1.x → sbinfo_v1.cpp，v2.0 → sbinfo_v20.cpp；本头 + common.cpp
 *   收共享件（结果簿 / crc32 / 小工具 / 入口声明），不拉任何盘面结构头。
 */

#include <cstdint>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>
#include <sys/types.h>

static constexpr uint64_t FS_BLOCK_BYTES = 4096;
static constexpr uint32_t EXT4_SB_MAGIC   = 0xEF53;   /* ext 系 sbi_magic @ 0x438 */
static constexpr off_t    EXT4_MAGIC_OFF  = 0x438;

/* ── 结果簿（定义在 sbinfo_common.cpp）── */
extern int n_ok, n_bad, n_warn;
void ok  (const char *fmt, ...) __attribute__((format(printf,1,2)));
void bad (const char *fmt, ...) __attribute__((format(printf,1,2)));
void warn(const char *fmt, ...) __attribute__((format(printf,1,2)));

/* ── 小工具 ── */
uint32_t crc32_calc(const void *data, size_t len);            /* poly 0xEDB88320，与 mkfs 同口径 */
bool     pread_full(int fd, void *buf, size_t n, off_t off);
std::string fmt_time(uint64_t epoch);
std::string fmt_bytes(uint64_t bytes);
std::string fmt_label(const uint8_t *name, size_t cap);
std::string fmt_uuid(const uint8_t u[16]);
void        probe_enc(std::string &s);

/* ── 收尾：打印 "checks: ..." 并返回进程退出码（不 close fd）── */
int sbinfo_finish(void);

/* ── 按 magic 分派的盘面入口（各自 TU 实现）──
 * raw = 设备字节 0 起 4096B 的 Primary SB 原始拷贝。 */
int sbinfo_v1_dump (int fd, const char *path, uint64_t dev_bytes, uint64_t lba_size, const uint8_t *raw);
int sbinfo_v20_dump(int fd, const char *path, uint64_t dev_bytes, uint64_t lba_size, const uint8_t *raw);
int sbinfo_v1_probe (const uint8_t *raw);
int sbinfo_v20_probe(const uint8_t *raw);
