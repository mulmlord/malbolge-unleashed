// Unpacking the picolibc sysroot built into malm (runtime_files.hpp) for `malm build --libc
// picolibc` (docs/rv.md 6).
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace malm {

// Writes the files of `archive`, a zstd-compressed ustar archive of regular files and directories
// as `cmake -E tar --zstd` makes it, under `dir`, creating directories as needed. Returns an error
// message, or empty. Members with an absolute path or a `..` component are errors.
std::string unpack_tar_zst(std::string_view archive, const std::filesystem::path& dir);

} // namespace malm
