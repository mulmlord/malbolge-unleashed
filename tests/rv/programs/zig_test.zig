// Self-checking test of the Zig runtime (rv/runtime/zig, docs/rv.md 6), run by
// rvref_pipe_test.sh. With empty stdin it prints "zig_test: N checks passed" and exits with main's
// 42; a failed check prints a FAIL line and the status is 1. The first line of stdin can choose an
// ending instead: "panic" indexes out of bounds (the panic on stderr, status 134), "error" returns
// an error from main ("error: AskedForIt" on stderr, status 1), "exit" calls malm.exit(7) with
// output still buffered, which exit writes.
const std = @import("std");
const malm = @import("malm");

// `value`, which the optimizer cannot see through.
fn hide(comptime T: type, value: T) T {
    var v = value;
    const p: *volatile T = &v;
    return p.*;
}

var checks: u32 = 0;
var failures: u32 = 0;

fn check(ok: bool, src: std.builtin.SourceLocation) void {
    checks += 1;
    if (!ok) {
        failures += 1;
        malm.print("FAIL line {d}\n", .{src.line});
    }
}

const Shape = union(enum) {
    square: u32,
    rect: struct { w: u32, h: u32 },

    fn area(self: Shape) u32 {
        return switch (self) {
            .square => |s| s * s,
            .rect => |r| r.w * r.h,
        };
    }
};

fn checkAlloc() !void {
    const gpa = malm.allocator;
    var v: std.ArrayList(u32) = .empty;
    defer v.deinit(gpa);
    for (0..1000) |i| {
        try v.append(gpa, @intCast(i)); // grows by remap or a new block
    }
    var sum: u32 = 0;
    for (v.items) |x| sum += x;
    check(v.items.len == 1000 and sum == 499_500, @src());
    v.shrinkAndFree(gpa, 10);
    check(std.mem.eql(u32, v.items, &.{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 }), @src());

    const shapes = try gpa.alloc(Shape, 2);
    defer gpa.free(shapes);
    shapes[0] = .{ .square = 3 };
    shapes[1] = .{ .rect = .{ .w = 2, .h = 5 } };
    check(shapes[0].area() + shapes[1].area() == 19, @src());

    const aligned = try gpa.alignedAlloc(u8, .@"64", 100);
    check(@intFromPtr(aligned.ptr) % 64 == 0, @src());
    gpa.free(aligned);

    // Freed blocks are reused: a thousand 1 KiB allocations, one at a time, hardly grow the heap.
    const start = malm.sys.brk(0);
    for (0..1000) |_| {
        const block = try gpa.alloc(u8, 1024);
        block[0] = 1;
        gpa.free(block);
    }
    check(malm.sys.brk(0) - start <= 4096, @src());

    // More than the heap fails without ending the program.
    check(gpa.alloc(u8, 8 << 20) == error.OutOfMemory, @src());

    var map: std.StringHashMapUnmanaged(u32) = .empty;
    defer map.deinit(gpa);
    for ([_][]const u8{ "a", "b", "a", "c", "a" }) |k| {
        const e = try map.getOrPut(gpa, k);
        e.value_ptr.* = if (e.found_existing) e.value_ptr.* + 1 else 1;
    }
    check(map.count() == 3 and map.get("a").? == 3, @src());
}

fn lessThan(_: void, a: []const u8, b: []const u8) bool {
    return std.mem.lessThan(u8, a, b);
}

fn checkLibrary() !void {
    var words = [_][]const u8{ "pear", "apple", "fig", "banana" };
    std.mem.sort([]const u8, &words, {}, lessThan);
    check(std.mem.eql(u8, words[0], "apple") and std.mem.eql(u8, words[3], "pear"), @src());

    var it = std.mem.tokenizeScalar(u8, "12 -7  30", ' ');
    var total: i32 = 0;
    while (it.next()) |t| total += try std.fmt.parseInt(i32, t, 10);
    check(total == 35, @src());
    check(std.fmt.parseInt(u8, "x1", 10) == error.InvalidCharacter, @src());
    check(std.fmt.parseInt(u8, "300", 10) == error.Overflow, @src());

    var buf: [64]u8 = undefined;
    check(std.mem.eql(u8, try std.fmt.bufPrint(&buf, "{s:>6}|{d:<4}|{x}|{d:0>5}", .{ "ab", 7, 255, 42 }), "    ab|7   |ff|00042"), @src());
    check(std.ascii.eqlIgnoreCase("Hello", "hELLO"), @src());
}

fn checkArithmetic() !void {
    const a: u64 = hide(u64, 0x1234_5678_9abc_def0);
    check(a / 1000 == 1_311_768_467_463_790 and a % 1000 == 320, @src());
    const b: i64 = hide(i64, -9_000_000_000);
    check(@divTrunc(b, 7) == -1_285_714_285 and @rem(b, 7) == -5, @src());
    const c: u128 = @as(u128, hide(u64, std.math.maxInt(u64))) * 3;
    var buf: [64]u8 = undefined;
    check(std.mem.eql(u8, try std.fmt.bufPrint(&buf, "{d}", .{c}), "55340232221128654845"), @src());
    check(std.math.add(u8, hide(u8, 250), 10) == error.Overflow, @src());

    // Soft floating point (compiler-rt), its formatting and parsing, and math.
    const x: f64 = hide(f64, 2.0);
    const y: f32 = hide(f32, 1.5);
    check(x * 1.25 == 2.5 and y / 3.0 == 0.5, @src());
    check(std.mem.eql(u8, try std.fmt.bufPrint(&buf, "{d:.3} {d} {e}", .{ x / 3.0, y, @as(f64, 1234.5) }), "0.667 1.5 1.2345e3"), @src());
    check(try std.fmt.parseFloat(f64, "2.5e3") == 2500.0, @src());
    check(@sqrt(x) > 1.414 and @sqrt(x) < 1.415, @src());
}

pub fn main() !u8 {
    var line_buf: [16]u8 = undefined;
    var line: std.Io.Writer = .fixed(&line_buf);
    _ = malm.stdin.streamDelimiterLimit(&line, '\n', .limited(15)) catch {};
    const first = line.buffered();
    if (std.mem.eql(u8, first, "panic")) {
        malm.print("before the panic\n", .{});
        const v = [_]u32{ 1, 2, 3 };
        const i = hide(usize, 5);
        return @intCast(v[i]);
    }
    if (std.mem.eql(u8, first, "error")) {
        return error.AskedForIt;
    }
    if (std.mem.eql(u8, first, "exit")) {
        malm.print("buffered ", .{});
        malm.print("until exit", .{});
        malm.exit(7);
    }
    try checkAlloc();
    try checkLibrary();
    try checkArithmetic();
    malm.print("zig_test: {d} checks passed\n", .{checks - failures});
    return if (failures == 0) 42 else 1;
}
