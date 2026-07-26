#pragma once

#include <cstdint>

enum RavenFSError : uint64_t {
    RAVENFS_OK              = 0,
    RAVENFS_ERR_OPEN        = 1,
    RAVENFS_ERR_LBA         = 2,
    RAVENFS_ERR_NOMAGIC     = 3,
    RAVENFS_ERR_VERSION     = 4,
    RAVENFS_ERR_WORLD_BASE  = 5,
    RAVENFS_ERR_CRC         = 6,
    RAVENFS_ERR_BACKUP      = 7,
    RAVENFS_ERR_BITMAP      = 8,
};
