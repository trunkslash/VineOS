#pragma once

#include <string>

namespace vine::rootfs {

// Opens an ext2/3/4 image entirely in userspace and validates the two paths
// VineOS needs before attempting rootless extraction/boot.
bool probe_ext4_rootfs(const std::string& image_path);

} // namespace vine::rootfs
