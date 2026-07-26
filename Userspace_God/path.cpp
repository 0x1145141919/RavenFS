#include "../include/ravenfs/ravenfs.hpp"
#include <cerrno>
#include <cstring>

RavenFS::PathCursor::PathCursor(RavenFS &fs, const char *path)
    : fs_ref(fs), root_inum(fs.root_inum)
{
    if (!path || path[0] != '/') { good = false; return; }

    strncpy(path_copy, path, sizeof(path_copy) - 1);
    path_copy[sizeof(path_copy) - 1] = '\0';

    const char *p = path_copy + 1;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        if (ncomps >= 64) { good = false; return; }
        comps[ncomps]     = p;
        const char *end = p;
        while (*end && *end != '/') end++;
        comp_lens[ncomps] = end - p;
        ncomps++;
        p = end;
    }

    cur_inode = root_inum;
    consumed  = 0;
    good      = true;
}

bool RavenFS::PathCursor::step() {
    if (!good || consumed >= ncomps || finished) {
        path_err = -ENOENT;
        return false;
    }

    const char *name = comps[consumed];
    size_t      len  = comp_lens[consumed];

    if (len >= 128) {
        path_err = -ENAMETOOLONG;
        good = false;
        return false;
    }

    raven_Inode_t inode;
    fs_ref.inode_read(cur_inode, &inode);
    if (inode.type != INODE_TYPE_DIR) {
        path_err = -ENOTDIR;
        good = false;
        return false;
    }

    char buf[128];
    memcpy(buf, name, len);
    buf[len] = '\0';

    uint64_t next;
    if (!fs_ref.dentry_lookup(cur_inode, buf, &next)) {
        path_err = -ENOENT;
        good = false;
        return false;
    }

    cur_inode = next;
    consumed++;

    if (consumed == ncomps)
        finished = true;

    return true;
}

bool RavenFS::PathCursor::walk_all() {
    if (ncomps == 0) {
        finished = true;
        return true;
    }
    while (consumed < ncomps) {
        if (!step()) return false;
    }
    return true;
}

bool RavenFS::PathCursor::walk_parent() {
    if (ncomps < 1) {
        path_err = -EBUSY;
        return false;
    }
    while (consumed < ncomps - 1) {
        if (!step()) return false;
    }
    const char *last = comps[ncomps - 1];
    size_t last_len  = comp_lens[ncomps - 1];
    if (last_len >= sizeof(leaf_buf)) {
        path_err = -ENAMETOOLONG;
        return false;
    }
    memcpy(leaf_buf, last, last_len);
    leaf_buf[last_len] = '\0';
    return true;
}

int RavenFS::path_resolve(const char *path, uint64_t *out_inum) {
    PathCursor cur(*this, path);
    if (!cur.ok()) {
        if (path && path[0] == '/' && path[1] == '\0') {
            *out_inum = root_inum;
            return 0;
        }
        return cur.err() ? cur.err() : -ENOENT;
    }
    if (!cur.walk_all())
        return cur.err();
    *out_inum = cur.inode();
    return 0;
}
