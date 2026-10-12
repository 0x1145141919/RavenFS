#include "audit/audit_types.hpp"

namespace rfs_audit {

const char *severity_str(severity_t s)
{
    switch (s) {
    case severity_t::info:  return "I";
    case severity_t::warn:  return "W";
    case severity_t::error: return "E";
    }
    return "?";
}

const char *scope_str(scope_t s)
{
    switch (s) {
    case scope_t::boot:        return "BOOT";
    case scope_t::block:       return "BLOCK";
    case scope_t::inode_plane: return "INODEPLANE";
    case scope_t::name_plane:  return "NAMEPLANE";
    case scope_t::inode:       return "INODE";
    case scope_t::btree:       return "BTREE";
    case scope_t::dentry:      return "DENTRY";
    case scope_t::extent:      return "EXTENT";
    case scope_t::xattr:       return "XATTR";
    case scope_t::namedata:    return "NAMEDATA";
    case scope_t::recon:       return "RECON";
    }
    return "?";
}

} /* namespace rfs_audit */
