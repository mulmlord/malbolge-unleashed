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
#include <iterator>
#include <optional>
#include <ostream>
#include <sstream>
#include <utility>

#include "driver/runtime_files.hpp"

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

std::vector<std::string> target_flags(const fs::path& dir) {
    return {"--target=riscv32-unknown-elf",
            "-march=rv32im",
            "-mabi=ilp32",
            "-nostdlibinc",
            "-isystem",
            (dir / "include").string(),
            "-ffunction-sections",
            "-fdata-sections",
            "-O2"};
}

} // namespace

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
    } else if (cc.find('/') != std::string_view::npos) {
        if (!is_executable(fs::path(cc))) {
            return R::err("compiler '" + std::string(cc) + "' not found or not executable");
        }
        tools.cc = cc;
    } else {
        auto p = search_path(cc, path_env);
        if (!p) {
            return R::err("compiler '" + std::string(cc) + "' is not in PATH");
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
          std::span<const std::string_view> args, std::ostream& err) {
    using R = core::Result<std::vector<std::uint8_t>, std::string>;
    const TempDir tmp;
    if (!tmp.ok()) {
        return R::err(std::string("cannot create a temporary directory: ") + std::strerror(errno));
    }
    const fs::path& dir = tmp.path();
    std::vector<fs::path> libc_sources;
    for (const RuntimeFile& f : runtime_files()) {
        const fs::path p = dir / f.path;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream(p, std::ios::binary)
            .write(f.text.data(), static_cast<std::streamsize>(f.text.size()));
        if (f.path.starts_with("src/")) {
            libc_sources.push_back(p);
        }
    }

    std::vector<std::string> cc_args;
    std::vector<std::string> ld_args;
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

    // crt0 first and alone: if the compiler cannot target riscv32, this is where it says so.
    std::string error;
    const std::vector<std::string> target = target_flags(dir);
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

    // The libc and the program's sources in parallel; the libc's output only if it fails.
    struct Compile {
        Job job;
        bool runtime;
    };
    std::vector<Compile> jobs;
    std::vector<std::string> libc_objects;
    std::vector<std::string> objects;
    const auto start = [&](const std::string& src, const std::string& obj,
                           std::span<const std::string> extra, bool runtime) {
        std::vector<std::string> v{tools.cc};
        v.insert(v.end(), target.begin(), target.end());
        v.insert(v.end(), extra.begin(), extra.end());
        v.insert(v.end(), {"-c", src, "-o", obj});
        jobs.push_back({spawn(v, obj + ".log", error), runtime});
    };
    const std::vector<std::string> libc_flags{"-ffreestanding", "-std=c11"};
    for (const fs::path& src : libc_sources) {
        libc_objects.push_back((dir / src.stem()).string() + ".o");
        start(src.string(), libc_objects.back(), libc_flags, true);
    }
    for (std::size_t i = 0; i < sources.size(); ++i) {
        objects.push_back((dir / ("prog" + std::to_string(i) + ".o")).string());
        start(std::string(sources[i]), objects.back(), cc_args, false);
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
    argv.emplace_back("--start-lib");
    argv.insert(argv.end(), libc_objects.begin(), libc_objects.end());
    argv.insert(argv.end(), {"--end-lib", "-o", elf.string()});
    if (!run(argv, dir / "ld.log", err, error)) {
        return R::err(error.empty() ? "linking failed" : error);
    }
    std::ifstream f(elf, std::ios::binary);
    if (!f) {
        return R::err("cannot read the linked program");
    }
    return R::ok(std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f),
                                           std::istreambuf_iterator<char>()));
}

} // namespace malm
