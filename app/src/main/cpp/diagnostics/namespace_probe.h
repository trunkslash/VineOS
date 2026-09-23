#pragma once
#include <string>

namespace vine::diagnostics {

struct NamespaceProbeResult {
    bool unshare_ok = false;
    int unshare_errno = 0;
    bool mount_ok = false;
    int mount_errno = 0;
    bool userns_ok = false;
    int userns_errno = 0;
    bool userns_mount_ok = false;
    int userns_mount_errno = 0;
    int caller_uid = -1;
};

// Tests both the current privileged namespace path and the rootless
// CLONE_NEWUSER + UID/GID mapping path from disposable child processes.
NamespaceProbeResult probe_namespace_support();

} // namespace vine::diagnostics
