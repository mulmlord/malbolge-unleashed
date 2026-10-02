// Startup check of a packed executable (docs/malm-format.md, "Packed executables").
#pragma once

#include <iosfwd>
#include <optional>

namespace malm {

inline constexpr const char* kSelfExe = "/proc/self/exe";

// Opens `path` read-only, close-on-exec, on a descriptor above 2. With stdin/stdout/stderr closed
// a plain open() would hand out fd 0-2 and the program would read or write the executable; this
// moves it up so the standard streams keep their (closed) state. Returns -1 and sets errno on
// failure.
int open_self_exe(const char* path);

// Looks at the tail of `exe_path` (one open, one 24-byte read). If it is not a packed executable
// returns nullopt and the normal CLI should run. Otherwise runs the embedded program on
// `in`/`out` (ignoring the command line) and returns its exit status, or 1 with
// `malm: corrupt packed executable: <reason>` if the trailer or payload is invalid.
std::optional<int> run_packed(const char* exe_path, std::istream& in, std::ostream& out,
                              std::ostream& err);

} // namespace malm
