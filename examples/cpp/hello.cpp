// Hello world in C++ on the RISC-V runtime: libc++'s std::string_view, and stdio for the output
// (malm's C++ has no iostreams, docs/rv.md 6).
#include <cstdio>
#include <string_view>

int main() {
    constexpr std::string_view greeting = "Hello, world!";
    std::fwrite(greeting.data(), 1, greeting.size(), stdout);
    std::putchar('\n');
}
