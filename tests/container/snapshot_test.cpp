// Startup snapshots (issue #38, docs/snapshot.md): SHA-256, the round trip VM state -> snapshot ->
// restored VM on random programs and the asm and HLA examples, the validation of every field, and
// compare(), which `malm snapshot --check` relies on.

#include <doctest/doctest.h>
#include <unistd.h>
#include <zstd.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "container/container.hpp"
#include "container/sha256.hpp"
#include "container/snapshot.hpp"
#include "core/loader.hpp"
#include "core/memory.hpp"
#include "core/tables.hpp"
#include "core/vm.hpp"
#include "driver/cli.hpp"
#include "driver/program_io.hpp"

namespace {

using namespace malm;
using container::Snapshot;
using container::SnapshotProgram;
using core::Memory;
using core::Value;
using core::Vm;

constexpr std::uint64_t kPage = Memory::kPageCells;

std::span<const std::uint8_t> bytes_of(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

struct StrIo : core::Io {
    std::string in;
    std::size_t pos = 0;
    std::string out;
    int read_byte() override {
        return pos < in.size() ? static_cast<unsigned char>(in[pos++]) : core::kEof;
    }
    bool write_byte(std::uint8_t b) override {
        out.push_back(static_cast<char>(b));
        return true;
    }
    bool flush() override { return true; }
};

SnapshotProgram key(const std::vector<std::uint8_t>& cells) {
    return {container::sha256(cells), cells};
}

// Registers, steps, n, the materialised pages and every cell in them.
void require_same_state(const Vm& a, const Vm& b) {
    REQUIRE(a.steps() == b.steps());
    REQUIRE(a.a() == b.a());
    REQUIRE(a.c() == b.c());
    REQUIRE(a.d() == b.d());
    REQUIRE(a.n() == b.n());
    REQUIRE(a.memory().page_indices() == b.memory().page_indices());
    for (const std::uint64_t idx : a.memory().page_indices()) {
        const auto pa = a.memory().page(idx);
        const auto pb = b.memory().page(idx);
        for (std::uint64_t i = 0; i < kPage; ++i) {
            if (pa[i] != pb[i]) {
                FAIL("page " << idx << " cell " << i << ": " << pa[i].to_string() << " vs "
                             << pb[i].to_string());
            }
        }
    }
}

// Snapshots `vm` (a VM of `cells`), restores it into a fresh VM and requires the same state; then
// runs both on `input` for up to `max_steps` and requires the same output and end.
void check_round_trip(const std::vector<std::uint8_t>& cells, Vm& vm, const std::string& input,
                      std::uint64_t max_steps = 2'000'000) {
    const SnapshotProgram prog = key(cells);
    const std::vector<std::uint8_t> bytes = container::write_snapshot(vm, prog);
    auto s = Snapshot::read(bytes, prog);
    REQUIRE_MESSAGE(s.has_value(), s.error());
    CHECK(s->steps() == vm.steps());
    CHECK(s->pages() == vm.memory().page_count());
    CHECK(s->compare(vm, cells) == "");
    Vm restored(cells);
    s->restore(restored, cells);
    require_same_state(vm, restored);
    CHECK(s->compare(restored, cells) == "");
    // Encoding is deterministic: the restored state gives the same bytes.
    CHECK(container::write_snapshot(restored, prog) == bytes);

    StrIo io1;
    StrIo io2;
    io1.in = io2.in = input;
    const core::RunResult r1 = vm.run(io1, max_steps);
    const core::RunResult r2 = restored.run(io2, max_steps);
    CHECK(r1.kind == r2.kind);
    CHECK(io1.out == io2.out);
    if (r1.fault && r2.fault) {
        CHECK(core::format_fault(*r1.fault) == core::format_fault(*r2.fault));
    }
    require_same_state(vm, restored);
}

// Loadable programs. With `pure`, every cell is a Nop or a MovD at its own position, so run_pure
// runs through the program (the examples and the synthetic state below write memory).
std::vector<std::uint8_t> random_cells(std::mt19937_64& rng, std::size_t len, bool pure) {
    std::vector<std::uint8_t> p(len);
    for (std::size_t i = 0; i < len; ++i) {
        const std::uint64_t r = rng();
        const auto sym = static_cast<std::uint8_t>(!pure ? r % 8 : r % 10 == 0 ? 0 : 7);
        p[i] = core::denormalize(i % 94, sym);
    }
    return p;
}

// Compiles an example with `malm asm` or `malm hla` (in process) and returns its cells.
std::vector<std::uint8_t> example(std::string_view tool, const std::string& file) {
    const std::filesystem::path out =
        std::filesystem::temp_directory_path() /
        ("malm_snapshot_test_" + std::to_string(static_cast<long>(getpid())) + ".malm");
    const std::string src = std::string(MALM_EXAMPLES_DIR) + "/" + file;
    const std::string out_s = out.string();
    std::istringstream in;
    std::ostringstream o;
    std::ostringstream e;
    const std::vector<std::string_view> args = {tool, src, "-o", out_s};
    REQUIRE_MESSAGE(malm::run_cli(args, in, o, e) == 0, e.str());
    LoadedProgram prog;
    REQUIRE(load_program(out_s, false, e, prog) == 0);
    std::filesystem::remove(out);
    return std::move(prog.cells);
}

} // namespace

TEST_CASE("sha256 test vectors (FIPS 180-4, and every padding case)") {
    const std::pair<std::string, const char*> cases[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {std::string(55, 'a'), "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {std::string(56, 'a'), "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {std::string(63, 'a'), "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {std::string(64, 'a'), "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {std::string(119, 'a'), "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
        {std::string(1000000, 'a'),
         "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
    };
    for (const auto& [msg, want] : cases) {
        CAPTURE(msg.size());
        CHECK(container::to_hex(container::sha256(bytes_of(msg))) == want);
    }
}

TEST_CASE("round trip: random programs, snapshots at random steps") {
    std::mt19937_64 rng(38);
    int tested = 0;
    for (int i = 0; i < 300; ++i) {
        CAPTURE(i);
        const auto cells = random_cells(rng, 2 + rng() % 30000, i % 2 == 0);
        Vm vm(cells);
        const core::RunResult r = vm.run_pure(rng() % 3000);
        if (r.kind == core::RunResult::Kind::Fault) {
            continue;
        }
        check_round_trip(cells, vm, "xyz", 20000);
        tested += vm.steps() > 10 ? 1 : 0;
    }
    CHECK(tested > 100);
}

TEST_CASE("round trip: the asm and HLA examples at their first I/O") {
    struct Case {
        std::string_view tool;
        const char* file;
        const char* input;
    };
    for (const Case& c : {Case{"asm", "asm/hello.mua", ""}, Case{"asm", "asm/adder.mua", "12 34\n"},
                          Case{"asm", "asm/halting_cat.mua", "cat!\n"},
                          Case{"hla", "hla/hello.mhl", ""}, Case{"hla", "hla/rev.mhl", "abc"}}) {
        CAPTURE(c.file);
        const auto cells = example(c.tool, c.file);
        Vm vm(cells);
        REQUIRE(vm.run_pure().kind == core::RunResult::Kind::BeforeIo);
        CHECK(vm.steps() > 0);
        check_round_trip(cells, vm, c.input);
    }
}

TEST_CASE("round trip: every run kind, far pages of all heads, wide registers") {
    const std::string text =
        "(=<`#9]~6ZY32Vw/.R,+Op(L,+k#Gh&}Cdz@aw=;zyKw%ut4Uqp0/mlejihtfrHcbaC2^W\\>Z,XW";
    const auto loaded = core::load_text(bytes_of(text));
    REQUIRE(loaded.has_value());
    const std::vector<std::uint8_t> cells = *loaded;
    Vm vm(cells);
    // Program cells encrypted 0..7 times (7 has no run kind: a literal), runs of each length.
    for (std::uint64_t i = 0; i < cells.size(); ++i) {
        std::uint32_t x = cells[i];
        for (std::uint64_t k = 0; k < i % 8; ++k) {
            x = core::encrypt(x);
        }
        vm.poke(*Value::natural(i), *Value::natural(x));
    }
    // Fill cells that hold cells too (encrypted fill), and literals of every head, also next to
    // the last page and around the ...222 -> 0 wrap.
    const std::uint64_t top = core::pow3[core::kTrits] - 1;
    for (const std::uint64_t addr : {std::uint64_t{200}, kPage - 1, kPage, 7 * kPage + 3,
                                     core::pow3[20], core::pow3[39] + 5, top - kPage, top}) {
        vm.poke(Value::from_raw(addr),
                Value::from_raw(addr % 2 ? top - addr % 1000 : addr % 94 + 33));
    }
    vm.poke(Value::from_raw(core::pow3[39] + 6), *Value::parse("...1102"));
    vm.memory_for_restore().restore_n(39);
    vm.set_registers(
        {Value::from_raw(top), Value::from_raw(core::pow3[38] - 1), Value::from_raw(top - 7)});
    check_round_trip(cells, vm, "", 0); // compare the state only; the forged state need not run
}

namespace {

void put_varint(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

void put_le(std::vector<std::uint8_t>& b, std::size_t off, std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        b[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

// The header of `snap` with `body` (compressed, CRC and length updated) and `pages`.
std::vector<std::uint8_t> forge(const std::vector<std::uint8_t>& snap,
                                const std::vector<std::uint8_t>& body, std::uint64_t pages) {
    std::vector<std::uint8_t> out(snap.begin(), snap.begin() + container::kSnapshotHeaderSize);
    std::vector<std::uint8_t> z(ZSTD_compressBound(body.size()));
    z.resize(ZSTD_compress(z.data(), z.size(), body.data(), body.size(), 1));
    put_le(out, 12, container::crc32(body), 4);
    put_le(out, 96, pages, 8);
    put_le(out, 104, z.size(), 8);
    out.insert(out.end(), z.begin(), z.end());
    return out;
}

// One page: its gap from the previous one, then runs (token, literals).
std::vector<std::uint8_t> page_body(std::uint64_t gap, std::uint64_t kind, std::uint64_t len) {
    std::vector<std::uint8_t> b;
    put_varint(b, gap);
    put_varint(b, (len - 1) << 3 | kind);
    return b;
}

} // namespace

TEST_CASE("validation: every error has its own message") {
    const std::string text =
        "(=<`#9]~6ZY32Vw/.R,+Op(L,+k#Gh&}Cdz@aw=;zyKw%ut4Uqp0/mlejihtfrHcbaC2^W\\>Z,XW";
    const std::vector<std::uint8_t> cells = *core::load_text(bytes_of(text));
    const SnapshotProgram prog = key(cells);
    Vm vm(cells);
    REQUIRE(vm.run_pure().kind == core::RunResult::Kind::BeforeIo);
    const std::vector<std::uint8_t> good = container::write_snapshot(vm, prog);
    REQUIRE(Snapshot::read(good, prog).has_value());

    const auto fails = [&](const std::vector<std::uint8_t>& bytes, const std::string& want,
                           const SnapshotProgram& p) {
        const auto r = Snapshot::read(bytes, p);
        REQUIRE_FALSE(r.has_value());
        CHECK_MESSAGE(r.error().find(want) != std::string::npos, r.error());
    };
    const auto patched = [&](std::size_t off, std::uint64_t v, int n) {
        std::vector<std::uint8_t> b = good;
        put_le(b, off, v, n);
        return b;
    };
    const auto check = [&](const std::vector<std::uint8_t>& b, const std::string& want) {
        fails(b, want, prog);
    };

    check({}, "bad magic");
    check(std::vector<std::uint8_t>(good.begin(), good.begin() + 50), "truncated header");
    check(patched(8, 2, 2), "unsupported snapshot version 2");
    check(patched(10, 1, 1), "unknown mode 1");
    check(patched(11, 1, 1), "reserved header bytes");
    check(patched(93, 1, 1), "reserved header bytes");
    check(patched(16, good[16] ^ 1u, 1), "another program file");
    fails(good, "another program file", {container::sha256(bytes_of("other")), cells});
    {
        std::vector<std::uint8_t> longer = cells;
        longer.push_back(core::denormalize(longer.size() % 94, 7));
        fails(good, "cell count", {prog.file_hash, longer});
    }
    check(patched(88, 9, 1), "n 9 out of range");
    check(patched(88, 40, 1), "n 40 out of range");
    check(patched(64, core::pow3[core::kTrits], 8), "register value out of range");
    check(patched(72, core::pow3[30], 8), "C or D is wider");
    check(patched(80, core::pow3[core::kTrits] - core::pow3[30] - 1, 8), "C or D is wider");
    check(patched(96, core::pow3[33], 8), "page count out of range");
    {
        std::vector<std::uint8_t> b = good;
        b.push_back(0);
        check(b, "bytes follow the header");
    }
    check(patched(12, good[12] ^ 1u, 1), "CRC mismatch");
    {
        std::vector<std::uint8_t> b = good;
        b[container::kSnapshotHeaderSize + 1] ^= 0xFF; // the frame header
        const auto r = Snapshot::read(b, prog);
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().starts_with("zstd: "));
    }

    // Bodies: the program fits page 0. Its fill holds non-cells (the encrypted-fill case).
    const auto& fill = vm.memory().fill_pattern();
    std::uint64_t non_cell = 0;
    while (fill[(cells.size() + non_cell) % 6].raw() >= 33 &&
           fill[(cells.size() + non_cell) % 6].raw() <= 126) {
        REQUIRE(++non_cell < 6);
    }
    const std::vector<std::uint8_t> same0 = page_body(0, 0, kPage);
    CHECK(Snapshot::read(forge(good, same0, 1), prog).has_value()); // the program as loaded
    check(forge(good, {}, 0), "the snapshot has 0 pages, the program 1");
    check(forge(good, page_body(1, 0, kPage), 1), "page 0 of the program is missing");
    check(forge(good, page_body(0, 0, kPage + 1), 1), "run past the end of page 0");
    check(forge(good, page_body(0, 0, 100), 1), "truncated or malformed run in page 0");
    check(forge(good, {}, 1), "truncated or malformed body at page 0");
    {
        std::vector<std::uint8_t> b = same0;
        b.push_back(0);
        check(forge(good, b, 1), "1 bytes of data after the last page");
    }
    {
        std::vector<std::uint8_t> b = same0;
        const auto far = page_body(core::pow3[32], 0, kPage); // page 3^32 + 1 does not exist
        b.insert(b.end(), far.begin(), far.end());
        check(forge(good, b, 2), "page index out of range at page 1");
    }
    {
        // Encrypting from the program's end to the first non-cell of the fill.
        std::vector<std::uint8_t> b = page_body(0, 0, cells.size());
        put_varint(b, (non_cell + 1 - 1) << 3 | 1);
        put_varint(b, (kPage - cells.size() - non_cell - 1 - 1) << 3 | 0);
        check(forge(good, b, 1), "encrypted run over a non-cell in page 0");
        // Encrypting the program alone is fine.
        std::vector<std::uint8_t> ok;
        put_varint(ok, 0);
        put_varint(ok, (cells.size() - 1) << 3 | 3);
        put_varint(ok, (kPage - cells.size() - 1) << 3 | 0);
        CHECK(Snapshot::read(forge(good, ok, 1), prog).has_value());
    }
    {
        std::vector<std::uint8_t> b = page_body(0, 7, 1);
        put_varint(b, core::pow3[core::kTrits]);
        put_varint(b, (kPage - 2) << 3 | 0);
        check(forge(good, b, 1), "malformed literal in page 0");
        std::vector<std::uint8_t> v = page_body(0, 7, 1);
        v.insert(v.end(), 10, 0xFF);
        v.push_back(1);
        check(forge(good, v, 1), "malformed literal in page 0");
    }
    {
        // A frame claiming more than its pages can need.
        std::vector<std::uint8_t> big(200'000, 0);
        check(forge(good, big, 1), "too large for its pages");
    }
}

TEST_CASE("compare names the first difference") {
    const auto cells = example("hla", "hla/hello.mhl");
    const SnapshotProgram prog = key(cells);
    Vm vm(cells);
    REQUIRE(vm.run_pure().kind == core::RunResult::Kind::BeforeIo);
    const auto s = Snapshot::read(container::write_snapshot(vm, prog), prog);
    REQUIRE(s.has_value());
    const auto restored = [&] {
        auto v = std::make_unique<Vm>(cells);
        s->restore(*v, cells);
        return v;
    };
    {
        const Vm fresh(cells);
        CHECK(s->compare(fresh, cells).starts_with("the step count is 0"));
    }
    {
        auto v = restored();
        v->set_registers({Value::from_raw(1), v->c(), v->d()});
        CHECK(s->compare(*v, cells).starts_with("A is 0t1"));
    }
    {
        auto v = restored();
        v->set_registers({v->a(), *Value::natural(5), v->d()});
        CHECK(s->compare(*v, cells).starts_with("C is"));
    }
    {
        auto v = restored();
        v->set_registers({v->a(), v->c(), *Value::natural(5)});
        CHECK(s->compare(*v, cells).starts_with("D is"));
    }
    {
        auto v = restored();
        v->memory_for_restore().restore_n(39);
        CHECK(s->compare(*v, cells).starts_with("n is 39"));
    }
    {
        auto v = restored();
        v->poke(*Value::natural(17), *Value::natural(1));
        CHECK(s->compare(*v, cells).starts_with("cell 0t122 is 0t1"));
    }
    {
        auto v = restored();
        const std::uint64_t last = v->memory().page_indices().back();
        v->poke(Value::from_raw(last * kPage + 3), Value::from_raw(1));
        CHECK(s->compare(*v, cells).starts_with("cell "));
        auto w = restored();
        w->memory_for_restore().page_for_write(last + 1);
        CHECK(s->compare(*w, cells) ==
              "page " + std::to_string(last + 1) + " is materialised in only one of them");
    }
}
