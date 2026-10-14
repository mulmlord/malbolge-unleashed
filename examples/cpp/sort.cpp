// Sorts the lines of stdin bytewise, like `LC_ALL=C sort` and examples/c/sort.c, with
// std::vector and std::sort: lines may hold any bytes, a last line without a newline gets one.
#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

int main() {
    // All of stdin, then the lines in it.
    std::string input;
    char buf[256];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, stdin)) > 0;) {
        input.append(buf, n);
    }
    std::vector<std::string_view> lines;
    for (std::size_t start = 0; start < input.size();) {
        const std::size_t end = std::min(input.find('\n', start), input.size());
        lines.emplace_back(input.data() + start, end - start);
        start = end + 1;
    }
    std::sort(lines.begin(), lines.end());
    for (const std::string_view line : lines) {
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::putchar('\n');
    }
}
