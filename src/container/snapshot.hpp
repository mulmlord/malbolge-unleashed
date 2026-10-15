// Startup snapshots (docs/snapshot.md): the state of an unleashed-mode VM after the steps that
// depend on the program alone (Vm::run_pure), saved next to the program so that a run can start
// from it instead of executing those steps. An accelerator only: restoring a snapshot gives the
// state that running the steps gives, so the program's behaviour is the same with or without it.
// The program file stays pure (spec 4); the snapshot is a separate file, tied to the program by
// the SHA-256 of the program file.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "container/sha256.hpp"
#include "core/result.hpp"
#include "core/vm.hpp"

namespace malm::container {

inline constexpr std::string_view kSnapshotMagic = "MALMSNAP";
inline constexpr std::uint16_t kSnapshotVersion = 1;
inline constexpr std::size_t kSnapshotHeaderSize = 112;

// The program a snapshot belongs to: the SHA-256 of the program file (a .malm or text, byte for
// byte) and its cells.
struct SnapshotProgram {
    Sha256 file_hash;
    std::span<const std::uint8_t> cells;
};

// Encodes the state of `vm`, a VM of `prog.cells` that has neither halted nor faulted.
std::vector<std::uint8_t> write_snapshot(const core::Vm& vm, const SnapshotProgram& prog);

// A snapshot that read() has validated, for one program.
class Snapshot {
  public:
    // Validates everything (docs/snapshot.md), also against the program: hash, cell count,
    // registers and n within the spec's invariants, pages that cover the program, cells that
    // decode. The error is a one-line reason.
    static core::Result<Snapshot, std::string> read(std::span<const std::uint8_t> bytes,
                                                    const SnapshotProgram& prog);

    std::uint64_t steps() const noexcept { return steps_; }
    std::uint64_t pages() const noexcept { return pages_; }

    // Restores the snapshot into `vm`. Precondition: a VM of the program read() validated against
    // that has not run.
    void restore(core::Vm& vm, std::span<const std::uint8_t> cells) const;

    // The first difference between the state of `vm` (a VM of the same program) and the
    // snapshot, or an empty string if they are the same: registers, step count, n, the set of
    // materialised pages and every cell in them.
    std::string compare(const core::Vm& vm, std::span<const std::uint8_t> cells) const;

  private:
    Snapshot() = default;

    core::Registers regs_{};
    std::uint64_t steps_ = 0;
    unsigned n_ = 0;
    std::uint64_t pages_ = 0;
    std::vector<std::uint8_t> body_; // decompressed and validated
};

} // namespace malm::container
