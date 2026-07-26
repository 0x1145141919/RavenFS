#include "../include/ravenfs/ravenfs.hpp"
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

void RavenFS::debug_dump(const char *label) {
    fprintf(stderr, "\n──── %s ────\n", label);

    /* ─── Cache view: 遍历 dentry hashtable ─── */
    fprintf(stderr, "[Cache] nr_dentries=%ld\n", vcache.nr_dentries.load());
    if (vcache.dentry_set && !vcache.dentry_set->empty()) {
        std::map<uint64_t, std::vector<std::string>> by_parent;
        for (auto it = vcache.dentry_set->begin(); it != vcache.dentry_set->end(); ++it) {
            const DentryEntry *de = &*it;
            const char *name = de->long_name ? de->long_name : de->name;
            by_parent[de->parent_inum].push_back(name);
        }
        for (auto &[parent, names] : by_parent) {
            fprintf(stderr, "  parent_inum=%-5lu:", (unsigned long)parent);
            for (auto &n : names)
                fprintf(stderr, " %s", n.c_str());
            fprintf(stderr, "\n");
        }
    } else {
        fprintf(stderr, "  (empty)\n");
    }

    /* ─── Disk view: 从 root 递归走 B+tree ─── */
    fprintf(stderr, "[Disk] B+tree:\n");
    struct DiskDumper {
        RavenFS &fs;
        void dump(uint64_t inum, int depth) {
            DirEntryInfo entries[128];
            int n = fs.readdir_inum(inum, entries, 128);
            if (n < 0) {
                fprintf(stderr, "%*sinum=%-5lu: (error %d)\n",
                        depth * 2, "", (unsigned long)inum, n);
                return;
            }
            if (n == 0) {
                fprintf(stderr, "%*sinum=%-5lu: (empty)\n",
                        depth * 2, "", (unsigned long)inum);
                return;
            }
            fprintf(stderr, "%*sinum=%-5lu:", depth * 2, "", (unsigned long)inum);
            for (int i = 0; i < n; i++) {
                const char *en = entries[i].name;
                if (en[0] == '.' && (en[1] == '\0' || (en[1] == '.' && en[2] == '\0')))
                    continue;
                fprintf(stderr, " %s", en);
            }
            fprintf(stderr, "\n");
            for (int i = 0; i < n; i++) {
                const char *en = entries[i].name;
                if (en[0] == '.' && (en[1] == '\0' || (en[1] == '.' && en[2] == '\0')))
                    continue;
                int t = fs.inode_type(entries[i].inode);
                if (t == INODE_TYPE_DIR)
                    dump(entries[i].inode, depth + 1);
            }
        }
    };
    if (root_inum != ~0ULL)
        DiskDumper{*this}.dump(root_inum, 0);
}
