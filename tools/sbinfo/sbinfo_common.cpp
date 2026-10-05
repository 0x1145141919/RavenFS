/* sbinfo 公共设施实现（见 sbinfo_common.hpp）。*/
#include "sbinfo_common.hpp"

#include <cstring>
#include <unistd.h>   /* pread */

int n_ok = 0, n_bad = 0, n_warn = 0;

static void vreport(const char *tag, const char *fmt, va_list ap) {
    printf("%-6s ", tag);
    vprintf(fmt, ap);
    printf("\n");
}
void ok  (const char *fmt, ...) { n_ok++;  va_list ap; va_start(ap, fmt); vreport("OK",   fmt, ap); va_end(ap); }
void bad (const char *fmt, ...) { n_bad++; va_list ap; va_start(ap, fmt); vreport("BAD",  fmt, ap); va_end(ap); }
void warn(const char *fmt, ...) { n_warn++; va_list ap; va_start(ap, fmt); vreport("WARN", fmt, ap); va_end(ap); }

/* ═══════════════════════ CRC32（与 mkfs.cpp / audit 逐位一致） ═══════════════════════ */
static uint32_t crc32_tab[256];
static bool     crc32_tab_init = false;
static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (c & 1 ? 0xEDB88320u : 0);
        crc32_tab[i] = c;
    }
    crc32_tab_init = true;
}
uint32_t crc32_calc(const void *data, size_t len) {
    if (!crc32_tab_init) crc32_init();
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t *b = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++)
        c = crc32_tab[(c ^ b[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool pread_full(int fd, void *buf, size_t n, off_t off) {
    uint8_t *p = (uint8_t *)buf;
    while (n > 0) {
        ssize_t r = pread(fd, p, n, off);
        if (r <= 0) return false;
        p += r; off += r; n -= (size_t)r;
    }
    return true;
}

std::string fmt_time(uint64_t epoch) {
    if (epoch == 0) return "never (0)";
    time_t t = (time_t)epoch;
    struct tm tmv;
    if (!localtime_r(&t, &tmv)) return "?";
    char buf[64];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
    char out[96];
    snprintf(out, sizeof out, "%s (%llu)", buf, (unsigned long long)epoch);
    return out;
}

std::string fmt_bytes(uint64_t bytes) {
    static const char *unit[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 5) { v /= 1024.0; u++; }
    char buf[64];
    snprintf(buf, sizeof buf, "%.2f %s", v, unit[u]);
    return buf;
}

std::string fmt_label(const uint8_t *name, size_t cap) {
    size_t len = 0;
    while (len < cap && name[len]) len++;
    std::string s;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = name[i];
        s += (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    return s;
}

std::string fmt_uuid(const uint8_t u[16]) {
    char buf[37];
    snprintf(buf, sizeof buf,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[0],u[1],u[2],u[3],u[4],u[5],u[6],u[7],
             u[8],u[9],u[10],u[11],u[12],u[13],u[14],u[15]);
    return buf;
}

void probe_enc(std::string &s) {   /* ID_FS_*_ENC：文件名安全化 */
    for (char &c : s) {
        unsigned char u = (unsigned char)c;
        const bool safe = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ||
                          (u >= '0' && u <= '9') || u == '.' || u == '_' || u == '-';
        if (!safe) c = '_';
    }
}

int sbinfo_finish(void) {
    printf("\nchecks: %d ok, %d bad, %d warn\n", n_ok, n_bad, n_warn);
    return n_bad ? 1 : 0;
}
