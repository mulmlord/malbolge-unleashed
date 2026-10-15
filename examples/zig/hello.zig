// Hello world in Zig on malm's Zig runtime (docs/rv.md 6).
const malm = @import("malm");

pub fn main() void {
    malm.print("Hello, world!\n", .{});
}
