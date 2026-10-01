#include <csignal>
#include <iostream>
#include <new>
#include <string_view>
#include <vector>

#include "driver/cli.hpp"

int main(int argc, char** argv) {
    // Binary-safe, unsynchronised I/O; a closed stdout pipe must surface as a stream error
    // (E_IO), not kill the process (spec 6).
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif
    std::vector<std::string_view> args(argc > 0 ? argv + 1 : argv, argv + argc);
    try {
        return malm::run_cli(args, std::cin, std::cout, std::cerr);
    } catch (const std::bad_alloc&) {
        std::cerr << "malm: out of memory\n";
        return 1;
    }
}
