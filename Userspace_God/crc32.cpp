#include "../include/Userspace_God.hpp"

uint32_t RavenFS::crc32_table[256];
bool     RavenFS::crc32_table_init = false;

void RavenFS::crc32_init() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (crc & 1 ? 0xEDB88320 : 0);
        crc32_table[i] = crc;
    }
    crc32_table_init = true;
}

uint32_t RavenFS::crc32_compute(const void *data, size_t len) {
    if (!crc32_table_init) crc32_init();
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t *buf = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
}
