#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "rv/elf.hpp"
#include "rv_test_util.hpp"

using namespace rvtest;
using malm::rv::load_elf;

namespace {

ElfSpec minimal() {
    ElfSpec spec;
    spec.segments.push_back({0x1000, words({li(a0, 0), li(a7, 93), kEcall}), 0});
    return spec;
}

std::string error_of(const ElfSpec& spec, std::uint32_t ram = malm::rv::kDefaultRamSize) {
    const auto r = load_elf(build_elf(spec), ram);
    REQUIRE_FALSE(r.has_value());
    return r.error();
}

} // namespace

TEST_CASE("elf: loads segments, entry and initial break") {
    ElfSpec spec = minimal();
    // Out of vaddr order, with .bss past the file data, plus a non-LOAD header that is ignored and
    // an empty LOAD that is skipped.
    spec.segments.insert(spec.segments.begin(), {0x3000, {1, 2, 3}, 0x100, 6});
    spec.segments.push_back({0x8000, {9, 9}, 0, 4, 0x70000003}); // PT_RISCV_ATTRIBUTES
    spec.segments.push_back({0x9000, {}, 0, 6});
    const auto r = load_elf(build_elf(spec));
    REQUIRE(r.has_value());
    CHECK(r->entry == 0x1000);
    REQUIRE(r->segments.size() == 2);
    CHECK(r->segments[0].vaddr == 0x1000);
    CHECK(r->segments[0].exec);
    CHECK(r->segments[0].data == words({li(a0, 0), li(a7, 93), kEcall}));
    CHECK(r->segments[1].vaddr == 0x3000);
    CHECK(r->segments[1].memsz == 0x100);
    CHECK(r->segments[1].data == std::vector<std::uint8_t>{1, 2, 3});
    CHECK_FALSE(r->segments[1].exec);
    CHECK(r->brk == 0x3100);
}

TEST_CASE("elf: a segment may end exactly at the end of RAM") {
    ElfSpec spec = minimal();
    spec.segments.push_back({0xff00, {0}, 0x100, 6});
    const auto r = load_elf(build_elf(spec), 0x10000);
    REQUIRE(r.has_value());
    CHECK(r->brk == 0x10000);
    CHECK(error_of(spec, 0xfff0).find("does not fit in guest RAM") != std::string::npos);
}

TEST_CASE("elf: header checks") {
    CHECK(load_elf(std::vector<std::uint8_t>{0x7f, 'E', 'L', 'F'}).error() == "not an ELF file");
    std::vector<std::uint8_t> bad = build_elf(minimal());
    bad[1] = 'X';
    CHECK(load_elf(bad).error() == "not an ELF file");

    ElfSpec spec = minimal();
    spec.elf_class = 2;
    CHECK(error_of(spec) == "not a 32-bit ELF file");
    spec = minimal();
    spec.data = 2;
    CHECK(error_of(spec) == "not a little-endian ELF file");
    spec = minimal();
    spec.type = 3; // ET_DYN
    CHECK(error_of(spec) == "not an executable (ET_EXEC) ELF file");
    spec = minimal();
    spec.machine = 62; // x86-64
    CHECK(error_of(spec) == "not a RISC-V ELF file");
    spec = minimal();
    spec.flags = 0x1;
    CHECK(error_of(spec).starts_with("compressed instructions (RVC) are not supported"));
    spec.flags = 0x2; // single-float ABI
    CHECK(error_of(spec).starts_with("hardware floating-point ABI is not supported"));
    spec.flags = 0x8;
    CHECK(error_of(spec) == "the RV32E base ISA is not supported");
    spec.flags = 0x10; // EF_RISCV_TSO: harmless
    CHECK(load_elf(build_elf(spec)).has_value());
    spec = minimal();
    spec.phentsize = 56;
    CHECK(error_of(spec) == "unexpected program header size");
}

TEST_CASE("elf: program header and segment bounds") {
    std::vector<std::uint8_t> truncated = build_elf(minimal());
    truncated.resize(60); // inside the first program header
    CHECK(load_elf(truncated).error() == "program headers extend past the end of the file");

    std::vector<std::uint8_t> file = build_elf(minimal());
    put32(file, 52 + 16, 0x100); // p_filesz past the end of the file (and past p_memsz)
    put32(file, 52 + 20, 0x100);
    CHECK(load_elf(file).error() == "segment 0 extends past the end of the file");
    put32(file, 52 + 16, 0x10);
    put32(file, 52 + 20, 0x8);
    CHECK(load_elf(file).error() == "segment 0 has p_filesz > p_memsz");

    ElfSpec spec = minimal();
    spec.segments[0].vaddr = 0xfffffff0; // vaddr + memsz wraps around
    spec.entry = 0xfffffff0;
    CHECK(error_of(spec).starts_with("segment 0 does not fit in guest RAM (1048576 bytes)"));

    spec = minimal();
    spec.segments.push_back({0x1008, {0}, 0, 6});
    CHECK(error_of(spec) == "loadable segments overlap");

    spec = minimal();
    spec.segments[0].type = 4; // PT_NOTE only
    CHECK(error_of(spec) == "no loadable segments");
}

TEST_CASE("elf: the entry point is 4-aligned inside an executable segment") {
    const std::string msg = "entry point is not 4-aligned inside an executable segment";
    ElfSpec spec = minimal();
    spec.entry = 0x1002;
    CHECK(error_of(spec) == msg);
    spec.entry = 0x100c; // just past the segment
    CHECK(error_of(spec) == msg);
    spec = minimal();
    spec.segments[0].flags = 6; // RW, not X
    CHECK(error_of(spec) == msg);
    spec = minimal();
    spec.entry = 0x1008;
    CHECK(load_elf(build_elf(spec)).has_value());
}
