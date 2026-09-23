#include "ext4_probe.h"
#include "../utils/vine_log.h"

extern "C" {
#include <ext2fs/ext2fs.h>
}

namespace vine::rootfs {

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

} // namespace vine::rootfs
