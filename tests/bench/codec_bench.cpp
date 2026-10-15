// Compression benchmark (issue #39, docs/benchmarks.md): compresses one program's normalized cells
// with zstd in process, in a given layout, level and window, and times compressing and
// decompressing; or, with --load, times loading the program as `malm run` does.
//
// Usage: codec_bench [--layout p3|s6|s8] [--level L] [--window-log W] [--long] [--workers N]
//                    [--dump <file>] <program>
//        codec_bench --load <program>
// Layouts: p3 = 3 bits per cell, packed (codec 1), s6 = two cells per byte, s8 = one cell per byte
// (codec 2). --long enables zstd's long-distance matching; --window-log 0 is the level's default;
// --workers 0 (the default) compresses on the calling thread. --dump writes the layout's bytes to
// <file> (for other compressors) and compresses nothing.
// Prints one line: layout bytes, compressed bytes, compress seconds, decompress seconds (the
// fastest of three, streamed in 1 MiB chunks as the reader does). --load prints: file bytes, cells,
// read seconds (read_malm: decompress, unpack, CRC), memory seconds (building the VM's memory),
// the fastest of three each.

#include <zstd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "container/container.hpp"
#include "core/vm.hpp"
#include "driver/program_io.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using Bytes = std::vector<std::uint8_t>;

double since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

// The cells' symbols in the layout.
Bytes layout(std::span<const std::uint8_t> cells, std::string_view form) {
    Bytes sym(cells.size());
    for (std::size_t i = 0; i < cells.size(); ++i) {
        sym[i] = malm::core::normalize(i, cells[i]);
    }
    if (form == "s8") {
        return sym;
    }
    Bytes out;
    if (form == "s6") {
        for (std::size_t i = 0; i < sym.size(); i += 2) {
            const std::uint8_t hi = i + 1 < sym.size() ? sym[i + 1] : 0;
            out.push_back(static_cast<std::uint8_t>(sym[i] | hi << 3));
        }
        return out;
    }
    std::uint32_t acc = 0;
    int nbits = 0;
    for (const std::uint8_t s : sym) {
        acc |= static_cast<std::uint32_t>(s) << nbits;
        nbits += 3;
        if (nbits >= 8) {
            out.push_back(static_cast<std::uint8_t>(acc));
            acc >>= 8;
            nbits -= 8;
        }
    }
    if (nbits > 0) {
        out.push_back(static_cast<std::uint8_t>(acc));
    }
    return out;
}

int usage() {
    std::cerr << "usage: codec_bench [--layout p3|s6|s8] [--level L] [--window-log W] [--long]\n"
                 "                   [--workers N] [--dump <file>] <program>\n"
                 "       codec_bench --load <program>\n";
    return 2;
}

int load(std::string_view path) {
    Bytes bytes;
    if (!malm::read_file(path, bytes, std::cerr)) {
        return 1;
    }
    double read_s = 1e9;
    double mem_s = 1e9;
    std::size_t cells = 0;
    for (int i = 0; i < 3; ++i) {
        const auto t0 = Clock::now();
        auto p = malm::container::read_malm(bytes);
        read_s = std::min(read_s, since(t0));
        if (!p) {
            std::cerr << "codec_bench: " << p.error() << '\n';
            return 1;
        }
        cells = p->cells.size();
        const auto t1 = Clock::now();
        const malm::core::Vm vm(p->cells);
        mem_s = std::min(mem_s, since(t1));
    }
    std::printf("%zu %zu %.6f %.6f\n", bytes.size(), cells, read_s, mem_s);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string_view form = "p3";
    int level = 9;
    int window_log = 0;
    bool long_mode = false;
    int workers = 0;
    bool load_only = false;
    std::string_view dump;
    std::string_view path;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--layout" && i + 1 < argc) {
            form = argv[++i];
        } else if (arg == "--level" && i + 1 < argc) {
            level = std::atoi(argv[++i]);
        } else if (arg == "--window-log" && i + 1 < argc) {
            window_log = std::atoi(argv[++i]);
        } else if (arg == "--long") {
            long_mode = true;
        } else if (arg == "--workers" && i + 1 < argc) {
            workers = std::atoi(argv[++i]);
        } else if (arg == "--dump" && i + 1 < argc) {
            dump = argv[++i];
        } else if (arg == "--load") {
            load_only = true;
        } else if (arg.starts_with('-') || !path.empty()) {
            return usage();
        } else {
            path = arg;
        }
    }
    if (path.empty() || (form != "p3" && form != "s6" && form != "s8")) {
        return usage();
    }
    if (load_only) {
        return load(path);
    }

    malm::LoadedProgram prog;
    if (const int rc = malm::load_program(path, false, std::cerr, prog, ~std::size_t{0}); rc != 0) {
        return rc;
    }
    const Bytes src = layout(prog.cells, form);
    if (!dump.empty()) {
        std::ofstream f{std::string(dump), std::ios::binary};
        f.write(reinterpret_cast<const char*>(src.data()),
                static_cast<std::streamsize>(src.size()));
        return f ? 0 : 1;
    }

    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, window_log);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, long_mode ? 1 : 0);
    if (ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, workers))) {
        std::cerr << "codec_bench: this zstd cannot use " << workers << " workers\n";
        return 1;
    }
    Bytes z(ZSTD_compressBound(src.size()));
    const auto t0 = Clock::now();
    const std::size_t zn = ZSTD_compress2(cctx, z.data(), z.size(), src.data(), src.size());
    const double comp_s = since(t0);
    ZSTD_freeCCtx(cctx);
    if (ZSTD_isError(zn)) {
        std::cerr << "codec_bench: " << ZSTD_getErrorName(zn) << '\n';
        return 1;
    }
    z.resize(zn);

    double dec_s = 1e9;
    Bytes buf(std::size_t{1} << 20);
    for (int i = 0; i < 3; ++i) {
        ZSTD_DCtx* dctx = ZSTD_createDCtx();
        ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, 31);
        ZSTD_inBuffer in{z.data(), z.size(), 0};
        std::size_t total = 0;
        const auto t1 = Clock::now();
        for (std::size_t ret = 1; ret != 0;) {
            ZSTD_outBuffer out{buf.data(), buf.size(), 0};
            ret = ZSTD_decompressStream(dctx, &out, &in);
            if (ZSTD_isError(ret)) {
                std::cerr << "codec_bench: " << ZSTD_getErrorName(ret) << '\n';
                return 1;
            }
            total += out.pos;
        }
        dec_s = std::min(dec_s, since(t1));
        ZSTD_freeDCtx(dctx);
        if (total != src.size()) {
            std::cerr << "codec_bench: decompressed " << total << " of " << src.size() << '\n';
            return 1;
        }
    }
    std::printf("%zu %zu %.3f %.6f\n", src.size(), zn, comp_s, dec_s);
    return 0;
}
