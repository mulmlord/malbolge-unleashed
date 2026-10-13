// The RISC-V runtime (rv/runtime, docs/rv.md 6) built into malm, for `malm build`.
#pragma once

#include <string_view>
#include <vector>

namespace malm {

struct RuntimeFile {
    std::string_view path; // relative to rv/runtime: crt0.S, link.ld, include/*.h, src/*.c
    std::string_view text;
};

// Every file of the runtime, in a fixed order: crt0.S, link.ld, the headers, the libc sources.
// Generated at build time (cmake/EmbedRuntime.cmake).
std::vector<RuntimeFile> runtime_files();

} // namespace malm
