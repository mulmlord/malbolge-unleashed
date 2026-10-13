#include "rv/emu.hpp"

#include <array>
#include <format>
#include <functional>
#include <tuple>
#include <utility>
#include <vector>

namespace malm::rv {
namespace {

using sem::Writer;

std::string num(std::uint32_t v) { return std::format("0x{:X}", v); }

// The 256 labels of a switch: f(b) for every low byte b.
std::vector<std::string> table(const std::function<std::string(unsigned)>& f) {
    std::vector<std::string> labels;
    for (unsigned b = 0; b < 256; ++b) {
        labels.push_back(f(b));
    }
    return labels;
}

// A switch on funct3 (insn >> 12, the low 3 bits); `targets` by funct3.
void funct3(Writer& w, const std::array<std::string_view, 8>& targets) {
    w.stmt("srl ix, insn, 12");
    w.switch_on("ix", table([&](unsigned b) { return std::string(targets[b % 8]); }));
}

// A switch on funct7 (insn >> 25): `zero` for 0, `alt` for 0x20 and `one` for 0x01 (if given);
// anything else is illegal.
void funct7(Writer& w, std::string_view zero, std::string_view alt, std::string_view one = {}) {
    w.stmt("srl ix, insn, 25");
    std::vector<std::string> labels(0x21, std::string(sem::kFaultIllegal));
    labels[0] = zero;
    labels[0x20] = alt;
    if (!one.empty()) {
        labels[1] = one;
    }
    w.switch_on("ix", labels);
    w.stmt(std::format("jmp {}", sem::kFaultIllegal));
}

void declarations(Writer& w, const EmuOptions& opt) {
    w.comment("guest registers (x0 reads as 0) and the decoder's");
    std::string regs = "reg x1";
    for (unsigned i = 2; i < 32; ++i) {
        regs += i == 2 ? std::format(", x2 = {}", num(opt.ram_size)) : std::format(", x{}", i);
    }
    w.stmt(regs);
    w.stmt(std::format("reg pc = {}, insn, a, ix, tg, b7, lk, lkw = next",
                       num(opt.image ? opt.image->entry : 0)));
    sem::declare_runtime(w, opt.image ? opt.image->brk : 0);
    // Guest RAM: one region at address 0, the image's words as its initial values.
    sem::declare_ram(w, opt.ram_size, opt.image ? &*opt.image : nullptr);
}

// Fetch, dispatch on the opcode (the low 7 bits; bit 7 is rd's lowest), and the end of an
// instruction: write rd, pc += 4.
void main_loop(Writer& w, const sem::Config& c) {
    w.label("fetch");
    w.stmt("load insn, [pc]");
    w.switch_on("insn", table([](unsigned b) -> std::string {
                    switch (b & 0x7F) {
                    case 0x03:
                        return "op_load";
                    case 0x0F:
                        return "op_misc";
                    case 0x13:
                        return "op_imm";
                    case 0x17:
                        return "op_auipc";
                    case 0x23:
                        return "op_store";
                    case 0x33:
                        return "op_op";
                    case 0x37:
                        return "op_lui";
                    case 0x63: // bit 7 is bit 11 of the branch offset
                        return b & 0x80 ? "op_branch1" : "op_branch0";
                    case 0x67:
                        return "op_jalr";
                    case 0x6F:
                        return "op_jal";
                    case 0x73:
                        return "op_system";
                    default:
                        return std::string(sem::kFaultIllegal);
                    }
                }));
    w.blank();

    w.comment("wb: x[rd] := r, then continue at lkw (normally next)");
    w.label("wb");
    w.stmt("srl ix, insn, 7");
    w.switch_on("ix", table([](unsigned b) { return std::format("wb{}", b % 32); }));
    w.label("wb0");
    w.stmt("jmp lkw");
    for (unsigned i = 1; i < 32; ++i) {
        w.label(std::format("wb{}", i));
        w.stmt(std::format("mov x{}, r", i));
        w.stmt("jmp lkw");
    }
    w.blank();

    // pc += 4. Inside a 256-byte block that flips a few bits of the low byte (a transform, about
    // 150 steps; the add is 1750). The last word of a block carries into the next byte, and the
    // block's word at ram_size - 4 may step out of guest RAM: both take the add and the check.
    w.comment("next: pc += 4");
    w.label("next");
    const unsigned last = (c.ram_size - 4) % 256;
    w.switch_on("pc", table([&](unsigned b) {
                    return b % 4 == 0 && b < 252 && b != last ? std::format("next{}", b)
                                                              : std::string("next_carry");
                }));
    for (unsigned b = 0; b < 252; b += 4) {
        if (b != last) {
            w.label(std::format("next{}", b));
            w.stmt(std::format("xor pc, pc, {}", num(b ^ (b + 4))));
            w.stmt("jmp fetch");
        }
    }
    w.label("next_carry");
    w.stmt("add pc, pc, 4");
    sem::check_in_ram(w, c, "pc", sem::kFaultFetch);
    w.stmt("jmp fetch");
    w.blank();

    // Operands: a := x[rs1], b := x[rs2]; return to lk.
    for (const auto& [name, reg, shift] :
         {std::tuple{"rs1", "a", 15}, std::tuple{"rs2", "b", 20}}) {
        w.comment(std::format("{}: {} := x[{}]", name, reg, name));
        w.label(name);
        w.stmt(std::format("srl ix, insn, {}", shift));
        w.switch_on("ix", table([&](unsigned b) { return std::format("{}_{}", name, b % 32); }));
        for (unsigned i = 0; i < 32; ++i) {
            w.label(std::format("{}_{}", name, i));
            w.stmt(i == 0 ? std::format("mov {}, 0", reg) : std::format("mov {}, x{}", reg, i));
            w.stmt("jmp lk");
        }
        w.blank();
    }

    // A jump to tg (jal, jalr, a taken branch): a misaligned target faults, changing nothing;
    // then lj: jal and jalr write rd; then pc := tg, and a target outside RAM faults at the fetch.
    w.comment("jump: pc := tg if aligned, after lk (rd := pc + 4 for jal, jalr)");
    w.label("jump");
    w.switch_on("tg", table([](unsigned b) {
                    return std::string(b % 4 == 0 ? "jump_ok" : sem::kFaultMisaligned);
                }));
    w.label("jump_ok");
    w.stmt("jmp lk");
    w.label("jump_link");
    w.stmt("add r, pc, 4");
    w.stmt("mov lkw, jump_set");
    w.stmt("jmp wb");
    w.label("jump_set");
    w.stmt("mov lkw, next");
    w.label("jump_go");
    w.stmt("mov pc, tg");
    sem::check_in_ram(w, c, "pc", sem::kFaultFetch);
    w.stmt("jmp fetch");
}

void instructions(Writer& w) {
    using sem::Alu;
    using sem::Cond;
    using sem::Width;
    const std::string_view ill = sem::kFaultIllegal;

    w.comment("lui, auipc: r := imm << 12 (+ pc)");
    w.label("op_lui");
    w.stmt("and r, insn, 0xFFFFF000");
    w.stmt("jmp wb");
    w.label("op_auipc");
    w.stmt("and r, insn, 0xFFFFF000");
    sem::alu(w, Alu::Add, "r", "r", "pc");
    w.stmt("jmp wb");
    w.blank();

    w.comment("OP-IMM: b := imm_i");
    w.label("op_imm");
    w.call("lk", "rs1", "op_imm_a");
    w.stmt("sra b, insn, 20");
    funct3(w, {"addi", "slli", "slti", "sltiu", "xori", "srxi", "ori", "andi"});
    for (const auto& [label, op] : {std::pair{"addi", Alu::Add},
                                    {"slti", Alu::Slt},
                                    {"sltiu", Alu::Sltu},
                                    {"xori", Alu::Xor},
                                    {"ori", Alu::Or},
                                    {"andi", Alu::And},
                                    {"slli_ok", Alu::Sll},
                                    {"srli", Alu::Srl},
                                    {"srai", Alu::Sra}}) {
        w.label(label);
        sem::alu(w, op, "r", "a", "b"); // the shifts use b's low 5 bits, the shift amount
        w.stmt("jmp wb");
    }
    w.label("slli");
    w.stmt("srl ix, insn, 25");
    w.switch_on("ix", {"slli_ok"});
    w.stmt(std::format("jmp {}", ill));
    w.label("srxi");
    funct7(w, "srli", "srai");
    w.blank();

    w.comment("OP: register-register, funct7 0x00, 0x20 (sub, sra) or 0x01 (RV32M)");
    w.label("op_op");
    w.call("lk", "rs1", "op_op_a");
    w.call("lk", "rs2", "op_op_b");
    funct7(w, "op_base", "op_alt", "op_m");
    w.label("op_base");
    funct3(w, {"i_add", "i_sll", "i_slt", "i_sltu", "i_xor", "i_srl", "i_or", "i_and"});
    w.label("op_alt");
    funct3(w, {"i_sub", ill, ill, ill, ill, "i_sra", ill, ill});
    w.label("op_m");
    funct3(w, {"i_mul", "i_mulh", "i_mulhsu", "i_mulhu", "i_div", "i_divu", "i_rem", "i_remu"});
    for (const auto& [label, op] : {std::pair{"add", Alu::Add},
                                    {"sub", Alu::Sub},
                                    {"sll", Alu::Sll},
                                    {"slt", Alu::Slt},
                                    {"sltu", Alu::Sltu},
                                    {"xor", Alu::Xor},
                                    {"srl", Alu::Srl},
                                    {"sra", Alu::Sra},
                                    {"or", Alu::Or},
                                    {"and", Alu::And},
                                    {"mul", Alu::Mul},
                                    {"mulh", Alu::Mulh},
                                    {"mulhsu", Alu::Mulhsu},
                                    {"mulhu", Alu::Mulhu},
                                    {"div", Alu::Div},
                                    {"divu", Alu::Divu},
                                    {"rem", Alu::Rem},
                                    {"remu", Alu::Remu}}) {
        w.label(std::format("i_{}", label));
        sem::alu(w, op, "r", "a", "b");
        w.stmt("jmp wb");
    }
    w.blank();

    w.comment("LOAD: ea := x[rs1] + imm_i");
    w.label("op_load");
    w.call("lk", "rs1", "op_load_a");
    w.stmt("sra b, insn, 20");
    sem::alu(w, Alu::Add, "ea", "a", "b");
    funct3(w, {"lb", "lh", "lw", ill, "lbu", "lhu", ill, ill});
    for (const auto& [label, width, sign] :
         {std::tuple{"lb", Width::Byte, true}, std::tuple{"lh", Width::Half, true},
          std::tuple{"lw", Width::Word, false}, std::tuple{"lbu", Width::Byte, false},
          std::tuple{"lhu", Width::Half, false}}) {
        w.label(label);
        sem::load(w, width, sign, "wb");
    }
    w.blank();

    w.comment("STORE: ea := x[rs1] + imm_s, b = x[rs2]");
    w.label("op_store");
    w.call("lk", "rs1", "op_store_a");
    w.call("lk", "rs2", "op_store_b");
    w.stmt("sra ea, insn, 20"); // imm[11:5] in bits 5..11, sign above; bits 0..4 are rs2
    w.stmt("and ea, ea, -32");
    w.stmt("srl u, insn, 7");
    w.stmt("and u, u, 31");
    w.stmt("or ea, ea, u");
    sem::alu(w, Alu::Add, "ea", "a", "ea");
    funct3(w, {"sb", "sh", "sw", ill, ill, ill, ill, ill});
    for (const auto& [label, width] :
         {std::pair{"sb", Width::Byte}, {"sh", Width::Half}, {"sw", Width::Word}}) {
        w.label(label);
        sem::store(w, width, "next");
    }
    w.blank();

    w.comment("BRANCH: b7 := bit 11 of the offset (the opcode's switch saw it)");
    w.label("op_branch1");
    w.stmt("mov b7, 0x800");
    w.stmt("jmp op_branch");
    w.label("op_branch0");
    w.stmt("mov b7, 0");
    w.label("op_branch");
    w.call("lk", "rs1", "op_branch_a");
    w.call("lk", "rs2", "op_branch_b");
    funct3(w, {"beq", "bne", ill, ill, "blt", "bge", "bltu", "bgeu"});
    for (const auto& [label, cond] : {std::pair{"beq", Cond::Eq},
                                      {"bne", Cond::Ne},
                                      {"blt", Cond::Lt},
                                      {"bge", Cond::Ge},
                                      {"bltu", Cond::Ltu},
                                      {"bgeu", Cond::Geu}}) {
        w.label(label);
        sem::branch(w, cond, "a", "b", "taken");
        w.stmt("jmp next");
    }
    // imm_b: bits 5..10 and the sign from insn >> 20, bits 1..4 from insn >> 7, bit 11 = b7.
    w.label("taken");
    w.stmt("sra tg, insn, 20");
    w.stmt("and tg, tg, 0xFFFFF7E0");
    w.stmt("srl u, insn, 7");
    w.stmt("and u, u, 0x1E");
    w.stmt("or tg, tg, u");
    w.stmt("or tg, tg, b7");
    sem::alu(w, Alu::Add, "tg", "pc", "tg");
    w.stmt("mov lk, jump_go");
    w.stmt("jmp jump");
    w.blank();

    w.comment("jal: imm_j = sign << 20 | insn[19:12] << 12 | insn[20] << 11 | insn[30:21] << 1");
    w.label("op_jal");
    w.stmt("sra tg, insn, 11");
    w.stmt("and tg, tg, 0xFFF00000");
    w.stmt("and u, insn, 0xFF000");
    w.stmt("or tg, tg, u");
    w.stmt("srl u, insn, 9");
    w.stmt("and u, u, 0x800");
    w.stmt("or tg, tg, u");
    w.stmt("srl u, insn, 20");
    w.stmt("and u, u, 0x7FE");
    w.stmt("or tg, tg, u");
    sem::alu(w, Alu::Add, "tg", "pc", "tg");
    w.stmt("mov lk, jump_link");
    w.stmt("jmp jump");
    w.label("op_jalr");
    funct3(w, {"jalr", ill, ill, ill, ill, ill, ill, ill});
    w.label("jalr");
    w.call("lk", "rs1", "op_jalr_a");
    w.stmt("sra tg, insn, 20");
    sem::alu(w, Alu::Add, "tg", "a", "tg");
    w.stmt("and tg, tg, -2");
    w.stmt("mov lk, jump_link");
    w.stmt("jmp jump");
    w.blank();

    w.comment("MISC-MEM: fence and fence.i are no-ops");
    w.label("op_misc");
    funct3(w, {"next", "next", ill, ill, ill, ill, ill, ill});
    w.blank();

    w.comment("SYSTEM: ecall, ebreak");
    w.label("op_system");
    w.stmt("xor t, insn, 0x73");
    w.stmt("bz t, ecall_insn");
    w.stmt("xor t, insn, 0x100073");
    w.stmt(std::format("bz t, {}", sem::kFaultEbreak));
    w.stmt(std::format("jmp {}", ill));
    w.label("ecall_insn");
    sem::ecall(w, "next");
}

} // namespace

std::string emu_source(const EmuOptions& opt) {
    const sem::Config c{opt.ram_size, opt.harness};
    Writer w;
    w.comment(std::format("rv-emu: the RV32IM emulator (docs/rv.md 8), generated by malm rvemu; "
                          "guest RAM {} bytes",
                          opt.ram_size));
    declarations(w, opt);
    w.blank();
    main_loop(w, c);
    w.blank();
    instructions(w);
    w.blank();
    sem::runtime(w, c);
    return w.text();
}

} // namespace malm::rv
