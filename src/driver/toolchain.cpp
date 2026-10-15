#include "driver/toolchain.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <ostream>
#include <sstream>
#include <utility>

#include "driver/runtime_files.hpp"
#include "driver/sysroot.hpp"

extern char** environ;

namespace malm {
namespace {

namespace fs = std::filesystem;

// Versioned names (Ubuntu's clang-18, ld.lld-18) are tried from this version down.
constexpr int kNewestVersion = 40;
constexpr int kOldestVersion = 14;

bool is_executable(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && ::access(p.c_str(), X_OK) == 0;
}

// `name` in the directories of `path_env`, like execvp.
std::optional<std::string> search_path(std::string_view name, std::string_view path_env) {
    std::size_t start = 0;
    while (start <= path_env.size()) {
        std::size_t end = path_env.find(':', start);
        if (end == std::string_view::npos) {
            end = path_env.size();
        }
        const std::string_view dir = path_env.substr(start, end - start);
        const fs::path p = fs::path(dir.empty() ? "." : std::string(dir)) / name;
        if (is_executable(p)) {
            return p.string();
        }
        start = end + 1;
    }
    return std::nullopt;
}

// `name`, else the newest `name-N`, in PATH.
std::optional<std::string> search_versioned(std::string_view name, std::string_view path_env) {
    if (auto p = search_path(name, path_env)) {
        return p;
    }
    for (int v = kNewestVersion; v >= kOldestVersion; --v) {
        if (auto p = search_path(std::string(name) + '-' + std::to_string(v), path_env)) {
            return p;
        }
    }
    return std::nullopt;
}

// A directory of its own under the system's temporary directory, removed with the object.
class TempDir {
  public:
    TempDir() {
        std::error_code ec;
        std::string tmpl = (fs::temp_directory_path(ec) / "malm-build-XXXXXX").string();
        if (!ec && ::mkdtemp(tmpl.data()) != nullptr) {
            path_ = tmpl;
        }
    }
    ~TempDir() {
        if (!path_.empty()) {
            std::error_code ec;
            fs::remove_all(path_, ec);
        }
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    bool ok() const { return !path_.empty(); }
    const fs::path& path() const { return path_; }

  private:
    fs::path path_;
};

// A tool started with stdin from /dev/null and stdout and stderr into the file `log`.
struct Job {
    pid_t pid = -1;
    fs::path log;
};

Job spawn(const std::vector<std::string>& argv, const fs::path& log, std::string& error) {
    std::vector<char*> cargv;
    for (const std::string& a : argv) {
        cargv.push_back(const_cast<char*>(a.c_str()));
    }
    cargv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
    Job job{-1, log};
    const int rc = ::posix_spawn(&job.pid, cargv[0], &actions, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        job.pid = -1;
        error = "cannot run " + argv[0] + ": " + std::strerror(rc);
    }
    return job;
}

// Waits for the job and copies its output to `err`. True if it exited with status 0.
bool finish(const Job& job, std::ostream& err) {
    int status = 0;
    while (::waitpid(job.pid, &status, 0) < 0 && errno == EINTR) {
    }
    // Not `err << rdbuf()`: an empty log would set failbit on `err`.
    std::ifstream f(job.log, std::ios::binary);
    err << std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Runs one tool to completion; see finish.
bool run(const std::vector<std::string>& argv, const fs::path& log, std::ostream& err,
         std::string& error) {
    const Job job = spawn(argv, log, error);
    return job.pid >= 0 && finish(job, err);
}

// `includes` are the header directories, searched in this order: for C++ libc++'s, then the C
// library's.
std::vector<std::string> target_flags(std::initializer_list<fs::path> includes) {
    std::vector<std::string> flags{"--target=riscv32-unknown-elf", "-march=rv32im", "-mabi=ilp32",
                                   "-nostdlibinc"};
    for (const fs::path& include : includes) {
        flags.insert(flags.end(), {"-isystem", include.string()});
    }
    flags.insert(flags.end(), {"-ffunction-sections", "-fdata-sections", "-O2"});
    return flags;
}

bool ends_with_any(std::string_view path, std::initializer_list<std::string_view> suffixes) {
    for (const std::string_view suffix : suffixes) {
        if (path.size() > suffix.size() && path.ends_with(suffix)) {
            return true;
        }
    }
    return false;
}

// The compiler that an option names: a path if it has a '/', else a name searched in PATH.
core::Result<std::string, std::string> find_named(std::string_view name,
                                                  std::string_view path_env) {
    using R = core::Result<std::string, std::string>;
    if (name.find('/') != std::string_view::npos) {
        if (!is_executable(fs::path(name))) {
            return R::err("compiler '" + std::string(name) + "' not found or not executable");
        }
        return R::ok(std::string(name));
    }
    auto p = search_path(name, path_env);
    if (!p) {
        return R::err("compiler '" + std::string(name) + "' is not in PATH");
    }
    return R::ok(std::move(*p));
}

// Writes the runtime's files whose paths `wanted` accepts into `dir`; returns their paths.
template <typename Pred> std::vector<fs::path> write_runtime(const fs::path& dir, Pred wanted) {
    std::vector<fs::path> written;
    for (const RuntimeFile& f : runtime_files()) {
        if (!wanted(f.path)) {
            continue;
        }
        const fs::path p = dir / f.path;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream(p, std::ios::binary)
            .write(f.text.data(), static_cast<std::streamsize>(f.text.size()));
        written.push_back(p);
    }
    return written;
}

// The user's arguments split into the compiler's and the linker's (`-Wl,a,b`).
void split_args(std::span<const std::string_view> args, std::vector<std::string>& cc_args,
                std::vector<std::string>& ld_args) {
    for (const std::string_view a : args) {
        if (a.starts_with("-Wl,")) {
            std::istringstream parts{std::string(a.substr(4))};
            for (std::string part; std::getline(parts, part, ',');) {
                ld_args.push_back(part);
            }
        } else {
            cc_args.emplace_back(a);
        }
    }
}

// The linked program.
core::Result<std::vector<std::uint8_t>, std::string> read_elf(const fs::path& elf) {
    using R = core::Result<std::vector<std::uint8_t>, std::string>;
    std::ifstream f(elf, std::ios::binary);
    if (!f) {
        return R::err("cannot read the linked program");
    }
    return R::ok(std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f),
                                           std::istreambuf_iterator<char>()));
}

} // namespace

bool is_c_source(std::string_view path) { return ends_with_any(path, {".c"}); }

bool is_cxx_source(std::string_view path) { return ends_with_any(path, {".cpp", ".cc", ".cxx"}); }

bool is_rust_source(std::string_view path) { return ends_with_any(path, {".rs"}); }

bool wants_exceptions(std::span<const std::string_view> args) {
    bool on = false;
    for (const std::string_view a : args) {
        on = a == "-fexceptions" || (on && a != "-fno-exceptions");
    }
    return on;
}

core::Result<Toolchain, std::string> find_toolchain(std::string_view cc,
                                                    std::string_view path_env) {
    using R = core::Result<Toolchain, std::string>;
    Toolchain tools;
    if (cc.empty()) {
        auto p = search_versioned("clang", path_env);
        if (!p) {
            return R::err("no C compiler: clang is not in PATH (install clang and lld, or name "
                          "the compiler with --cc)");
        }
        tools.cc = std::move(*p);
    } else {
        auto p = find_named(cc, path_env);
        if (!p) {
            return R::err(std::move(p.error()));
        }
        tools.cc = std::move(*p);
    }

    std::error_code ec;
    const fs::path real = fs::canonical(tools.cc, ec);
    for (const fs::path& dir :
         {ec ? fs::path() : real.parent_path(), fs::path(tools.cc).parent_path()}) {
        if (!dir.empty() && is_executable(dir / "ld.lld")) {
            tools.ld = (dir / "ld.lld").string();
            return R::ok(std::move(tools));
        }
    }
    auto ld = search_versioned("ld.lld", path_env);
    if (!ld) {
        return R::err("no linker: ld.lld is not next to " + tools.cc + " or in PATH (install lld)");
    }
    tools.ld = std::move(*ld);
    return R::ok(std::move(tools));
}

core::Result<std::vector<std::uint8_t>, std::string>
compile_c(const Toolchain& tools, std::span<const std::string_view> sources,
          std::span<const std::string_view> args, std::ostream& err, Libc libc) {
    using R = core::Result<std::vector<std::uint8_t>, std::string>;
    bool cxx = false;
    for (const std::string_view src : sources) {
        cxx = cxx || is_cxx_source(src);
    }
    if (cxx && libc == Libc::Minimal) {
        return R::err("C++ needs picolibc (--libc picolibc)");
    }
    if (libc == Libc::Picolibc && picolibc_sysroot().empty()) {
        return R::err("this malm is built without picolibc (MALM_PICOLIBC)");
    }
    const TempDir tmp;
    if (!tmp.ok()) {
        return R::err(std::string("cannot create a temporary directory: ") + std::strerror(errno));
    }
    const fs::path& dir = tmp.path();
    const std::vector<fs::path> runtime_paths =
        write_runtime(dir, [](std::string_view path) { return !path.starts_with("rust/"); });
    std::vector<fs::path> libc_sources;
    if (libc == Libc::Minimal) {
        for (const fs::path& p : runtime_paths) {
            if (p.parent_path() == dir / "src") {
                libc_sources.push_back(p);
            }
        }
    }
    std::vector<std::string> cc_args;
    std::vector<std::string> ld_args;
    split_args(args, cc_args, ld_args);
    const bool exceptions = wants_exceptions(args);

    // picolibc: its headers and libraries, and libmalm.a, its system interface; for C++ libc++ and
    // libc++abi, with exceptions those in lib/eh and libunwind (rv/runtime/CMakeLists.txt).
    fs::path include = dir / "include";
    fs::path cxx_include;
    std::vector<std::string> libs;
    if (libc == Libc::Picolibc) {
        const fs::path sysroot = dir / "picolibc";
        const std::string unpacked = unpack_tar_zst(picolibc_sysroot(), sysroot);
        if (!unpacked.empty()) {
            return R::err("internal error: the picolibc sysroot: " + unpacked);
        }
        include = sysroot / "include";
        cxx_include = include / "c++" / "v1";
        if (cxx) {
            const fs::path lib = sysroot / "lib" / (exceptions ? "eh" : "");
            for (const char* name : {"libc++.a", "libc++abi.a"}) {
                libs.push_back((lib / name).string());
            }
            if (exceptions) {
                libs.push_back((lib / "libunwind.a").string());
                ld_args.insert(ld_args.begin(), "--eh-frame-hdr");
            }
        }
        for (const char* lib : {"libmalm.a", "libc.a", "libclang_rt.builtins.a"}) {
            libs.push_back((sysroot / "lib" / lib).string());
        }
    }

    // crt0 first and alone: if the compiler cannot target riscv32, this is where it says so.
    std::string error;
    const std::vector<std::string> target = target_flags({include});
    const fs::path crt0 = dir / "crt0.o";
    std::vector<std::string> argv{tools.cc};
    argv.insert(argv.end(), target.begin(), target.end());
    argv.insert(argv.end(), {"-c", (dir / "crt0.S").string(), "-o", crt0.string()});
    std::ostringstream probe;
    if (!run(argv, dir / "crt0.log", probe, error)) {
        if (!error.empty()) {
            return R::err(error);
        }
        err << probe.str();
        return R::err(tools.cc + " cannot compile for riscv32-unknown-elf (malm build needs clang "
                                 "with the RISC-V target)");
    }

    // The minimal libc and the program's sources in parallel; the libc's output only if it fails.
    // C++ sources see libc++'s headers first, and by default neither exceptions nor RTTI;
    // -fbinutils-version=none lets ld.lld drop a function's exception table with the function.
    struct Compile {
        Job job;
        bool runtime;
    };
    std::vector<Compile> jobs;
    std::vector<std::string> libc_objects;
    std::vector<std::string> objects;
    const auto start = [&](const std::string& src, const std::string& obj,
                           std::span<const std::string> flags, std::span<const std::string> extra,
                           bool runtime) {
        std::vector<std::string> v{tools.cc};
        v.insert(v.end(), flags.begin(), flags.end());
        v.insert(v.end(), extra.begin(), extra.end());
        v.insert(v.end(), {"-c", src, "-o", obj});
        jobs.push_back({spawn(v, obj + ".log", error), runtime});
    };
    const std::vector<std::string> libc_flags{"-ffreestanding", "-std=c11"};
    for (const fs::path& src : libc_sources) {
        libc_objects.push_back((dir / src.stem()).string() + ".o");
        start(src.string(), libc_objects.back(), target, libc_flags, true);
    }
    std::vector<std::string> cxx_target = target_flags({cxx_include, include});
    cxx_target.insert(cxx_target.end(),
                      {"-fno-exceptions", "-fno-rtti", "-fbinutils-version=none"});
    for (std::size_t i = 0; i < sources.size(); ++i) {
        objects.push_back((dir / ("prog" + std::to_string(i) + ".o")).string());
        start(std::string(sources[i]), objects.back(),
              is_cxx_source(sources[i]) ? cxx_target : target, cc_args, false);
    }
    bool ok = true;
    bool runtime_ok = true;
    for (const Compile& c : jobs) {
        if (c.job.pid < 0) {
            ok = false;
            continue;
        }
        std::ostringstream log;
        const bool done = finish(c.job, log);
        if (!c.runtime || !done) {
            err << log.str();
        }
        ok = ok && done;
        runtime_ok = runtime_ok && (done || !c.runtime);
    }
    if (!error.empty()) {
        return R::err(error);
    }
    if (!runtime_ok) {
        return R::err("internal error: the runtime does not compile with " + tools.cc);
    }
    if (!ok) {
        return R::err("compilation failed");
    }

    const fs::path elf = dir / "prog.elf";
    argv = {tools.ld, "-T", (dir / "link.ld").string(), "--gc-sections"};
    argv.insert(argv.end(), ld_args.begin(), ld_args.end());
    argv.push_back(crt0.string());
    argv.insert(argv.end(), objects.begin(), objects.end());
    if (!libc_objects.empty()) {
        argv.emplace_back("--start-lib");
        argv.insert(argv.end(), libc_objects.begin(), libc_objects.end());
        argv.emplace_back("--end-lib");
    }
    argv.insert(argv.end(), libs.begin(), libs.end());
    argv.insert(argv.end(), {"-o", elf.string()});
    if (!run(argv, dir / "ld.log", err, error)) {
        return R::err(error.empty() ? "linking failed" : error);
    }
    return read_elf(elf);
}

core::Result<std::string, std::string> find_rustc(std::string_view rustc,
                                                  std::string_view path_env) {
    using R = core::Result<std::string, std::string>;
    if (!rustc.empty()) {
        return find_named(rustc, path_env);
    }
    auto p = search_path("rustc", path_env);
    if (!p) {
        return R::err("no Rust compiler: rustc is not in PATH (install Rust with rustup, or name "
                      "the compiler with --rustc)");
    }
    return R::ok(std::move(*p));
}

core::Result<std::vector<std::uint8_t>, std::string>
compile_rust(const std::string& rustc, std::string_view source,
             std::span<const std::string_view> args, std::ostream& err) {
    using R = core::Result<std::vector<std::uint8_t>, std::string>;
    const TempDir tmp;
    if (!tmp.ok()) {
        return R::err(std::string("cannot create a temporary directory: ") + std::strerror(errno));
    }
    const fs::path& dir = tmp.path();
    write_runtime(
        dir, [](std::string_view path) { return path == "link.ld" || path.starts_with("rust/"); });
    const std::string target(kRustTarget);

    // The target's core library, which rustup installs with the target.
    std::string error;
    std::ostringstream libdir;
    if (!run({rustc, "--print", "target-libdir", "--target", target}, dir / "libdir.log", libdir,
             error)) {
        if (!error.empty()) {
            return R::err(error);
        }
        err << libdir.str();
        return R::err(rustc + " cannot compile for " + target);
    }
    std::string path = libdir.str();
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) {
        path.pop_back();
    }
    bool core = false;
    std::error_code ec;
    for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        core = core || (name.starts_with("libcore-") && name.ends_with(".rlib"));
    }
    if (!core) {
        return R::err("the Rust target " + target + " is not installed for " + rustc +
                      " (rustup target add " + target + ")");
    }

    // The runtime crate, its output only if it fails.
    const std::vector<std::string> flags{"--target", target, "-C", "opt-level=2"};
    const fs::path rlib = dir / "libmalm.rlib";
    std::vector<std::string> argv{rustc};
    argv.insert(argv.end(), flags.begin(), flags.end());
    argv.insert(argv.end(), {"--edition", "2024", "--crate-type", "rlib", "--crate-name", "malm",
                             (dir / "rust" / "malm.rs").string(), "-o", rlib.string()});
    std::ostringstream log;
    if (!run(argv, dir / "malm.log", log, error)) {
        if (!error.empty()) {
            return R::err(error);
        }
        err << log.str();
        return R::err("the Rust runtime does not compile with " + rustc +
                      " (malm build needs Rust 1.85 or newer)");
    }

    // The program, compiled and linked by rustc.
    std::vector<std::string> rustc_args;
    std::vector<std::string> ld_args;
    split_args(args, rustc_args, ld_args);
    bool edition = false;
    for (const std::string& a : rustc_args) {
        edition = edition || a == "--edition" || a.starts_with("--edition=");
    }
    const fs::path elf = dir / "prog.elf";
    argv = {rustc};
    argv.insert(argv.end(), flags.begin(), flags.end());
    if (!edition) {
        argv.insert(argv.end(), {"--edition", "2024"});
    }
    argv.insert(argv.end(), {"--crate-type", "bin", "--extern", "malm=" + rlib.string(), "-C",
                             "link-arg=-T" + (dir / "link.ld").string()});
    for (const std::string& a : ld_args) {
        argv.insert(argv.end(), {"-C", "link-arg=" + a});
    }
    argv.insert(argv.end(), rustc_args.begin(), rustc_args.end());
    argv.insert(argv.end(), {std::string(source), "-o", elf.string()});
    std::ostringstream diags;
    const bool ok = run(argv, dir / "prog.log", diags, error);
    err << diags.str();
    if (!ok) {
        if (!error.empty()) {
            return R::err(error);
        }
        return R::err(diags.str().find("error: linking with") != std::string::npos
                          ? "linking failed"
                          : "compilation failed");
    }
    return read_elf(elf);
}

} // namespace malm
