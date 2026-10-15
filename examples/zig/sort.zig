// Sorts the lines of stdin bytewise, like `LC_ALL=C sort` and examples/c/sort.c, with an
// ArrayList and std.mem.sort: lines may hold any bytes, a last line without a newline gets one.
const std = @import("std");
const malm = @import("malm");

fn lessThan(_: void, a: []const u8, b: []const u8) bool {
    return std.mem.lessThan(u8, a, b);
}

pub fn main() !void {
    const gpa = malm.allocator;
    const input = try malm.stdin.allocRemaining(gpa, .unlimited);
    var lines: std.ArrayList([]const u8) = .empty;
    var it = std.mem.splitScalar(u8, input, '\n');
    while (it.next()) |line| {
        try lines.append(gpa, line);
    }
    if (input.len == 0 or input[input.len - 1] == '\n') {
        _ = lines.pop(); // what follows the last newline, or the empty input
    }
    std.mem.sort([]const u8, lines.items, {}, lessThan);
    for (lines.items) |line| {
        try malm.stdout.writeAll(line);
        try malm.stdout.writeByte('\n');
    }
}
