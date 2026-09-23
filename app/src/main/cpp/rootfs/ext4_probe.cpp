#include "ext4_probe.h"
#include "../utils/vine_log.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include <ext2fs/ext2fs.h>
}

namespace vine::rootfs {
namespace {

struct ExtractContext {
    ext2_filsys fs;
    std::string output_dir;
    size_t files = 0;
    size_t directories = 0;
    size_t symlinks = 0;
    size_t skipped = 0;
    bool failed = false;
};

bool write_regular_file(ext2_filsys fs, ext2_ino_t ino,
                        const struct ext2_inode& inode,
                        const std::string& path) {
    ext2_file_t file = nullptr;
    errcode_t err = ext2fs_file_open(fs, ino, 0, &file);
    if (err) {
        VINE_LOGE("ext4 extract: open inode %u failed err=%ld", ino, static_cast<long>(err));
        return false;
    }

    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0700);
    if (fd < 0) {
        VINE_LOGE("ext4 extract: create %s failed: %s", path.c_str(), strerror(errno));
        ext2fs_file_close(file);
        return false;
    }

    std::vector<unsigned char> buffer(1024 * 1024);
    bool ok = true;
    while (true) {
        unsigned int got = 0;
        err = ext2fs_file_read(file, buffer.data(),
                               static_cast<unsigned int>(buffer.size()), &got);
        if (err) {
            VINE_LOGE("ext4 extract: read inode %u failed err=%ld", ino, static_cast<long>(err));
            ok = false;
            break;
        }
        if (got == 0) break;

        size_t offset = 0;
        while (offset < got) {
            ssize_t written = write(fd, buffer.data() + offset, got - offset);
            if (written <= 0) {
                VINE_LOGE("ext4 extract: write %s failed: %s", path.c_str(), strerror(errno));
                ok = false;
                break;
            }
            offset += static_cast<size_t>(written);
        }
        if (!ok) break;
    }

    close(fd);
    ext2fs_file_close(file);

    if (ok) {
        // Ownership cannot be reproduced by an ordinary Android app. Preserve
        // the permission bits; UID/GID virtualization is handled separately.
        chmod(path.c_str(), inode.i_mode & 07777);
    }
    return ok;
}

bool read_symlink(ext2_filsys fs, ext2_ino_t ino,
                  const struct ext2_inode& inode, std::string* target) {
    if (ext2fs_is_fast_symlink(&inode)) {
        const size_t size = static_cast<size_t>(inode.i_size);
        target->assign(reinterpret_cast<const char*>(inode.i_block), size);
        return true;
    }

    ext2_file_t file = nullptr;
    errcode_t err = ext2fs_file_open(fs, ino, 0, &file);
    if (err) return false;

    std::vector<char> buffer(static_cast<size_t>(inode.i_size) + 1, 0);
    size_t offset = 0;
    while (offset < inode.i_size) {
        unsigned int got = 0;
        const unsigned int wanted = static_cast<unsigned int>(
            std::min<size_t>(buffer.size() - 1 - offset, 1024 * 1024));
        err = ext2fs_file_read(file, buffer.data() + offset, wanted, &got);
        if (err || got == 0) break;
        offset += got;
    }
    ext2fs_file_close(file);
    if (err || offset != inode.i_size) return false;
    target->assign(buffer.data(), offset);
    return true;
}

bool extract_inode(ExtractContext* ctx, ext2_ino_t ino,
                   const struct ext2_inode& inode,
                   const std::string& path);

int extract_dirent(ext2_ino_t, int, struct ext2_dir_entry* dirent, int, int, char*, void* private_data) {
    auto* ctx = static_cast<ExtractContext*>(private_data);
    if (ctx->failed || dirent->inode == 0) return 0;

    const int len = ext2fs_dirent_name_len(dirent);
    if (len <= 0 || len > EXT2_NAME_LEN) return 0;
    std::string name(dirent->name, static_cast<size_t>(len));
    if (name == "." || name == "..") return 0;
    if (name.find('/') != std::string::npos) {
        VINE_LOGW("ext4 extract: skipping unsafe directory entry");
        ++ctx->skipped;
        return 0;
    }

    struct ext2_inode inode {};
    errcode_t err = ext2fs_read_inode(ctx->fs, dirent->inode, &inode);
    if (err) {
        VINE_LOGE("ext4 extract: read inode %u failed err=%ld",
                  dirent->inode, static_cast<long>(err));
        ctx->failed = true;
        return 0;
    }

    const std::string child = ctx->output_dir + "/" + name;
    const std::string parent = ctx->output_dir;
    ctx->output_dir = child;
    if (!extract_inode(ctx, dirent->inode, inode, child)) ctx->failed = true;
    ctx->output_dir = parent;
    return 0;
}

bool extract_inode(ExtractContext* ctx, ext2_ino_t ino,
                   const struct ext2_inode& inode,
                   const std::string& path) {
    if (LINUX_S_ISDIR(inode.i_mode)) {
        if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
            VINE_LOGE("ext4 extract: mkdir %s failed: %s", path.c_str(), strerror(errno));
            return false;
        }
        ++ctx->directories;
        errcode_t err = ext2fs_dir_iterate2(ctx->fs, ino, 0, nullptr,
                                            extract_dirent, ctx);
        chmod(path.c_str(), inode.i_mode & 07777);
        return err == 0 && !ctx->failed;
    }

    if (LINUX_S_ISREG(inode.i_mode)) {
        if (!write_regular_file(ctx->fs, ino, inode, path)) return false;
        ++ctx->files;
        return true;
    }

    if (LINUX_S_ISLNK(inode.i_mode)) {
        std::string target;
        if (!read_symlink(ctx->fs, ino, inode, &target)) {
            VINE_LOGE("ext4 extract: failed reading symlink inode %u", ino);
            return false;
        }
        unlink(path.c_str());
        if (symlink(target.c_str(), path.c_str()) != 0) {
            VINE_LOGE("ext4 extract: symlink %s -> %s failed: %s",
                      path.c_str(), target.c_str(), strerror(errno));
            return false;
        }
        ++ctx->symlinks;
        return true;
    }

    // Device nodes/FIFOs/sockets require privileges or are inappropriate to
    // copy from an untrusted image. Runtime setup creates the needed /dev nodes.
    ++ctx->skipped;
    return true;
}

} // namespace

bool probe_ext4_rootfs(const std::string& image_path) {
    ext2_filsys fs = nullptr;
    errcode_t err = ext2fs_open(
        image_path.c_str(),
        EXT2_FLAG_SOFTSUPP_FEATURES,
        0,
        0,
        unix_io_manager,
        &fs);

    if (err != 0 || fs == nullptr) {
        VINE_LOGE("libext2fs: failed to open %s (err=%ld)",
                  image_path.c_str(), static_cast<long>(err));
        return false;
    }

    VINE_LOGI("libext2fs: opened %s blocksize=%u blocks=%llu inodes=%u",
              image_path.c_str(),
              fs->blocksize,
              static_cast<unsigned long long>(ext2fs_blocks_count(fs->super)),
              fs->super->s_inodes_count);

    ext2_ino_t init_ino = 0;
    err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, "/init", &init_ino);
    if (err != 0 || init_ino == 0) {
        VINE_LOGE("libext2fs: /init missing (err=%ld)", static_cast<long>(err));
        ext2fs_close(fs);
        return false;
    }

    ext2_ino_t shell_ino = 0;
    err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, "/system/bin/sh", &shell_ino);
    if (err != 0 || shell_ino == 0) {
        VINE_LOGE("libext2fs: /system/bin/sh missing (err=%ld)", static_cast<long>(err));
        ext2fs_close(fs);
        return false;
    }

    VINE_LOGI("libext2fs rootfs probe OK: /init inode=%u /system/bin/sh inode=%u",
              init_ino, shell_ino);
    ext2fs_close(fs);
    return true;
}

bool extract_ext4_rootfs(const std::string& image_path, const std::string& output_dir) {
    ext2_filsys fs = nullptr;
    errcode_t err = ext2fs_open(image_path.c_str(), EXT2_FLAG_SOFTSUPP_FEATURES,
                                0, 0, unix_io_manager, &fs);
    if (err || fs == nullptr) {
        VINE_LOGE("ext4 extract: open %s failed err=%ld",
                  image_path.c_str(), static_cast<long>(err));
        return false;
    }

    if (mkdir(output_dir.c_str(), 0700) != 0 && errno != EEXIST) {
        VINE_LOGE("ext4 extract: mkdir output failed: %s", strerror(errno));
        ext2fs_close(fs);
        return false;
    }

    ExtractContext ctx {fs, output_dir};
    err = ext2fs_dir_iterate2(fs, EXT2_ROOT_INO, 0, nullptr, extract_dirent, &ctx);
    ext2fs_close(fs);

    if (err || ctx.failed) {
        VINE_LOGE("ext4 rootfs extraction failed err=%ld files=%zu dirs=%zu symlinks=%zu skipped=%zu",
                  static_cast<long>(err), ctx.files, ctx.directories, ctx.symlinks, ctx.skipped);
        return false;
    }

    VINE_LOGI("ext4 rootfs extraction OK: files=%zu dirs=%zu symlinks=%zu skipped=%zu output=%s",
              ctx.files, ctx.directories, ctx.symlinks, ctx.skipped, output_dir.c_str());
    return true;
}

} // namespace vine::rootfs
