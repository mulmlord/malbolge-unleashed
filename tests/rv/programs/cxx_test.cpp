// Self-checking test of the C++ runtime (libc++ and libc++abi on picolibc, docs/rv.md 6), run by
// rvref_pipe_test.sh. Built as `malm build` builds C++, without exceptions and RTTI, and also with
// -fexceptions -frtti (libunwind), which adds the checks under __cpp_exceptions and __cpp_rtti.
// Needs C++20. With empty stdin it prints "cxx_test: N checks passed", then "~Global 2" and
// "~Global 1" from the static destructors (the reverse order of construction), and exits with
// main's 42; a failed check prints a FAIL line and the status is 1. With stdin "abort" it calls
// std::vector::at out of range: libc++'s message (or, with exceptions, libc++abi's for the uncaught
// std::out_of_range) on stderr, then abort's status 134.
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#if __cpp_exceptions
#include <stdexcept>
#endif
#if __cpp_rtti
#include <typeinfo>
#endif

namespace {

int checks;
int failures;

void check(bool ok, int line, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, what);
    }
}

#define CHECK(e) check(static_cast<bool>(e), __LINE__, #e)

// Static constructors run before main, in order within the file; the destructors after main, in
// reverse.
struct Global {
    int id;
    explicit Global(int i) : id(i) {}
    ~Global() { std::printf("~Global %d\n", id); }
};
Global global1(1);
Global global2(global1.id + 1);

// A function-local static is constructed once, on first use (libc++abi's __cxa_guard_*).
int constructions;
int& counter() {
    static int c = (++constructions, 100);
    return c;
}

struct Shape {
    virtual ~Shape() = default;
    virtual int area() const = 0;
};
struct Square : Shape {
    int side;
    explicit Square(int s) : side(s) {}
    int area() const override { return side * side; }
};
struct Rect : Shape {
    int w, h;
    Rect(int a, int b) : w(a), h(b) {}
    int area() const override { return w * h; }
};

int destroyed;
struct Tracked {
    ~Tracked() { ++destroyed; }
};

struct alignas(64) Aligned {
    char c;
};

void check_language() {
    CHECK(global1.id == 1 && global2.id == 2);
    CHECK(counter() == 100 && counter() == 100 && constructions == 1);

    std::vector<std::unique_ptr<Shape>> shapes;
    shapes.push_back(std::make_unique<Square>(3));
    shapes.push_back(std::make_unique<Rect>(2, 5));
    int total = 0;
    for (const auto& s : shapes) {
        total += s->area();
    }
    CHECK(total == 19);

    int* array = new int[100];
    std::iota(array, array + 100, 0);
    CHECK(std::accumulate(array, array + 100, 0) == 4950);
    delete[] array;
    {
        const auto t = std::make_shared<Tracked>();
        const auto copy = t;
        CHECK(t.use_count() == 2);
    }
    CHECK(destroyed == 1);
    auto* a = new Aligned;
    CHECK(reinterpret_cast<std::uintptr_t>(a) % 64 == 0);
    delete a;

    int captured = 5;
    const std::function<int(int)> add = [&](int x) { return x + captured; };
    captured = 7;
    CHECK(add(1) == 8);
}

void check_library() {
    std::vector<int> v{5, 3, 9, 1, 7};
    std::sort(v.begin(), v.end());
    CHECK((v == std::vector<int>{1, 3, 5, 7, 9}));
    CHECK(std::binary_search(v.begin(), v.end(), 7));
    v.resize(1000, 4);
    CHECK(v.size() == 1000 && v.back() == 4);

    std::string s = "Hello";
    s += ", ";
    s.append(40, 'x'); // past the small-string buffer
    CHECK(s.size() == 47 && s.find("xx") == 7 && s.compare(0, 5, "Hello") == 0);
    CHECK(std::to_string(-1234) == "-1234");
    CHECK(std::stoi("  77") == 77);
    const std::string_view view = s;
    CHECK(view.substr(0, 4) == "Hell" && view.starts_with("He"));

    std::map<std::string, int> m{{"b", 2}, {"a", 1}, {"c", 3}};
    std::string keys;
    for (const auto& [k, value] : m) {
        keys += k;
    }
    CHECK(keys == "abc");
    std::unordered_map<int, int> squares;
    for (int i = 0; i < 50; ++i) {
        squares[i] = i * i;
    }
    CHECK(squares.size() == 50 && squares.at(49) == 2401);

    const std::optional<int> none;
    CHECK(!none && none.value_or(3) == 3);
    std::variant<int, std::string> var = std::string("text");
    CHECK(std::visit([](const auto& x) { return sizeof(x) > 4; }, var));
    var = 9;
    CHECK(std::get<int>(var) == 9);
    const std::array<int, 3> arr{1, 2, 3};
    CHECK(std::accumulate(arr.begin(), arr.end(), 0) == 6);

    // charconv, also floating point (LLVM libc's parser), and cmath (picolibc's libm).
    char buf[32];
    const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, 255, 16);
    CHECK(ec == std::errc() && std::string_view(buf, end) == "ff");
    double d = 0;
    const char* text = "2.5e3";
    CHECK(std::from_chars(text, text + 5, d).ec == std::errc() && d == 2500.0);
    CHECK(std::sqrt(2.0) > 1.414 && std::sqrt(2.0) < 1.415);
}

#if __cpp_exceptions
void check_exceptions() {
    int caught = 0;
    try {
        throw 42;
    } catch (int e) {
        caught = e;
    }
    CHECK(caught == 42);

    // Destructors run during unwinding; a derived exception is caught by its base.
    destroyed = 0;
    std::string what;
    try {
        Tracked t;
        throw std::runtime_error("boom");
    } catch (const std::exception& e) {
        what = e.what();
    }
    CHECK(what == "boom" && destroyed == 1);

    bool out_of_range = false;
    try {
        static_cast<void>(std::vector<int>(3).at(3));
    } catch (const std::out_of_range&) {
        out_of_range = true;
    }
    CHECK(out_of_range);

    int rethrown = 0;
    try {
        try {
            throw std::logic_error("inner");
        } catch (...) {
            ++rethrown;
            throw;
        }
    } catch (const std::logic_error& e) {
        rethrown += std::strcmp(e.what(), "inner") == 0;
    }
    CHECK(rethrown == 2);
}
#endif

#if __cpp_rtti
void check_rtti() {
    const std::unique_ptr<Shape> s = std::make_unique<Square>(2);
    CHECK(dynamic_cast<Square*>(s.get()) != nullptr);
    CHECK(dynamic_cast<Rect*>(s.get()) == nullptr);
    const Shape& r = *s;
    CHECK(typeid(r) == typeid(Square) && typeid(r) != typeid(Rect));
    CHECK(std::strcmp(typeid(int).name(), "i") == 0);
}
#endif

} // namespace

int main() {
    char input[16] = {};
    const std::size_t n = std::fread(input, 1, sizeof input - 1, stdin);
    if (std::string_view(input, n).starts_with("abort")) {
        const std::vector<int> v(3);
        return v.at(5);
    }
    check_language();
    check_library();
#if __cpp_exceptions
    check_exceptions();
#endif
#if __cpp_rtti
    check_rtti();
#endif
    std::printf("cxx_test: %d checks passed\n", checks - failures);
    return failures == 0 ? 42 : 1;
}
