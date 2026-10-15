//! The root module of every Zig program built by `malm build` (docs/rv.md 6): the entry point,
//! which calls the program's `main` (the module `program`), and the panic handler. The library is
//! the module `malm` (malm.zig).
const std = @import("std");
const malm = @import("malm");
const program = @import("program");

// _start: clears .bss, aligns sp to 16 (the ilp32 ABI) and calls start with it, as crt0.S does
// for C.
export fn _start() linksection(".text.init") callconv(.naked) noreturn {
    asm volatile (
        \\    la t0, __bss_start
        \\    la t1, __bss_end
        \\    j 2f
        \\1:  sw zero, 0(t0)
        \\    sw zero, 4(t0)
        \\    sw zero, 8(t0)
        \\    sw zero, 12(t0)
        \\    addi t0, t0, 16
        \\2:  bltu t0, t1, 1b
        \\    andi sp, sp, -16
        \\    mv a0, sp
        \\    call %[start]
        :
        : [start] "s" (&start),
    );
}

fn start(sp: usize) callconv(.c) noreturn {
    malm.stack_top = sp;
    malm.exit(callMain());
}

// main returns as in std: void (status 0), a u8 (the status), or either with an error union, whose
// error prints `error: Name` on stderr and is status 1.
inline fn callMain() u8 {
    const Return = @typeInfo(@TypeOf(program.main)).@"fn".return_type.?;
    switch (@typeInfo(Return)) {
        .void, .noreturn => {
            program.main();
            return 0;
        },
        .int => return program.main(),
        .error_union => {
            const result = program.main() catch |err| {
                malm.eprint("error: {s}\n", .{@errorName(err)});
                return 1;
            };
            return if (@TypeOf(result) == void) 0 else result;
        },
        else => @compileError("main must return void, u8, !void or !u8"),
    }
}

/// A panic prints `panic: ` and its message on stderr, after what stdout has buffered, and ends
/// the program with status 134, as std's abort after a panic does.
pub const panic = std.debug.FullPanic(struct {
    fn panic(msg: []const u8, _: ?usize) noreturn {
        malm.eprint("panic: {s}\n", .{msg});
        malm.abort();
    }
}.panic);
