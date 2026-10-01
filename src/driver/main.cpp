#include <iostream>
#include <string_view>
#include <vector>

#include "driver/cli.hpp"

int main(int argc, char** argv) {
    std::vector<std::string_view> args(argc > 0 ? argv + 1 : argv, argv + argc);
    return malm::run_cli(args, std::cout, std::cerr);
}
