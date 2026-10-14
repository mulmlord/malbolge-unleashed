# cmake -DARCHIVE=<file.tar.zst> -DPATHS=<path>|<path>... -P PackSysroot.cmake, run in the sysroot
# Packs the picolibc sysroot (rv/runtime/CMakeLists.txt) as `cmake -E tar --zstd` would, a ustar
# archive with fixed times that src/driver/sysroot.cpp unpacks, but at zstd's highest level: the
# archive is in every malm binary and every executable that `malm pack` makes.
string(REPLACE "|" ";" paths "${PATHS}")
file(ARCHIVE_CREATE OUTPUT "${ARCHIVE}" PATHS ${paths} FORMAT paxr COMPRESSION Zstd
    COMPRESSION_LEVEL 19 MTIME "1970-01-01 00:00:00 UTC")
