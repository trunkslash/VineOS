#include "namespace_probe.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <jni.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../utils/vine_log.h"

namespace vine::diagnostics {

namespace {
struct ProbeWireResult {
    int unshare_ok;
    int unshare_errno;
    int mount_ok;
    int mount_errno;
    int userns_ok;
    int userns_errno;
    int userns_mount_ok;
    int userns_mount_errno;
};

bool write_text(const char* path, const std::string& value) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const char* p = value.c_str();
    size_t left = value.size();
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        p += n;
        left -= static_cast<size_t>(n);
    }
    close(fd);
    return true;
}

void probe_privileged(ProbeWireResult& wire) {
    if (unshare(CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC) == 0) {
        wire.unshare_ok = 1;
        if (mount("none", "/", nullptr, MS_REC | MS_PRIVATE, nullptr) == 0) {
            wire.mount_ok = 1;
        } else {
            wire.mount_errno = errno;
        }
    } else {
        wire.unshare_errno = errno;
    }
}

void probe_userns(ProbeWireResult& wire) {
    const uid_t uid = getuid();
    const gid_t gid = getgid();

    if (unshare(CLONE_NEWUSER) != 0) {
        wire.userns_errno = errno;
        return;
    }

    // Linux requires setgroups to be denied before an unprivileged process
    // may install gid_map. Some kernels omit setgroups; ENOENT is harmless.
    if (!write_text("/proc/self/setgroups", "deny") && errno != ENOENT) {
        wire.userns_errno = errno;
        return;
    }
    if (!write_text("/proc/self/uid_map", "0 " + std::to_string(uid) + " 1\n")) {
        wire.userns_errno = errno;
        return;
    }
    if (!write_text("/proc/self/gid_map", "0 " + std::to_string(gid) + " 1\n")) {
        wire.userns_errno = errno;
        return;
    }

    wire.userns_ok = 1;

    // CAP_SYS_ADMIN is scoped to the new user namespace. Creating the mount
    // namespace after mapping IDs is the rootless-container pattern.
    if (unshare(CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWIPC) != 0) {
        wire.userns_mount_errno = errno;
        return;
    }
    if (mount("none", "/", nullptr, MS_REC | MS_PRIVATE, nullptr) == 0) {
        wire.userns_mount_ok = 1;
    } else {
        wire.userns_mount_errno = errno;
    }
}
}

NamespaceProbeResult probe_namespace_support() {
    NamespaceProbeResult result;
    result.caller_uid = (int)getuid();

    int pipefd[2];
    if (pipe(pipefd) != 0) { VINE_LOGE_ERRNO("pipe"); return result; }

    pid_t pid = fork();
    if (pid < 0) {
        VINE_LOGE_ERRNO("fork");
        close(pipefd[0]); close(pipefd[1]);
        return result;
    }

    if (pid == 0) {
        close(pipefd[0]);
        ProbeWireResult wire{};

        pid_t privileged = fork();
        if (privileged == 0) {
            ProbeWireResult child{};
            probe_privileged(child);
            write(pipefd[1], &child, sizeof(child));
            _exit(0);
        }
        waitpid(privileged, nullptr, 0);

        // The privileged child has completed and written its result first.
        // Now test the user-namespace path in this disposable process.
        probe_userns(wire);
        write(pipefd[1], &wire, sizeof(wire));
        close(pipefd[1]);
        _exit(0);
    }

    close(pipefd[1]);
    ProbeWireResult first{}, second{};
    ssize_t n1 = read(pipefd[0], &first, sizeof(first));
    ssize_t n2 = read(pipefd[0], &second, sizeof(second));
    close(pipefd[0]);
    waitpid(pid, nullptr, 0);

    if (n1 == (ssize_t)sizeof(first)) {
        result.unshare_ok = first.unshare_ok != 0;
        result.unshare_errno = first.unshare_errno;
        result.mount_ok = first.mount_ok != 0;
        result.mount_errno = first.mount_errno;
    }
    if (n2 == (ssize_t)sizeof(second)) {
        result.userns_ok = second.userns_ok != 0;
        result.userns_errno = second.userns_errno;
        result.userns_mount_ok = second.userns_mount_ok != 0;
        result.userns_mount_errno = second.userns_mount_errno;
    }
    return result;
}

} // namespace vine::diagnostics

extern "C" JNIEXPORT jstring JNICALL
Java_com_hexadecinull_vineos_shizuku_VineShizukuService_nativeProbeNamespaces(JNIEnv* env, jobject) {
    auto r = vine::diagnostics::probe_namespace_support();
    char buf[384];
    snprintf(buf, sizeof(buf),
        "unshare_ok=%d;unshare_errno=%d;mount_ok=%d;mount_errno=%d;"
        "userns_ok=%d;userns_errno=%d;userns_mount_ok=%d;userns_mount_errno=%d;uid=%d",
        r.unshare_ok ? 1 : 0, r.unshare_errno,
        r.mount_ok ? 1 : 0, r.mount_errno,
        r.userns_ok ? 1 : 0, r.userns_errno,
        r.userns_mount_ok ? 1 : 0, r.userns_mount_errno,
        r.caller_uid);
    return env->NewStringUTF(buf);
}
