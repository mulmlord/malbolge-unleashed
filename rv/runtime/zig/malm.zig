//! The Zig runtime's library (docs/rv.md 6), the module `malm` that every Zig program built by
//! `malm build` can import: the system calls of docs/rv.md 4, stdin, stdout and stderr as
//! `std.Io.Reader` and `std.Io.Writer` with the C runtime's buffering, an allocator on `brk`, and
//! `exit`. start.zig, the root module, has the entry point and the panic handler.
const std = @import("std");

/// The system calls of docs/rv.md 4, as the machine has them: the result is the value of `a0`, a
/// negated errno value on failure.
pub const sys = struct {
    pub const read_nr = 63;
    pub const write_nr = 64;
    pub const exit_nr = 93;
    pub const brk_nr = 214;

    pub fn syscall3(n: usize, a0: usize, a1: usize, a2: usize) isize {
        return asm volatile ("ecall"
            : [ret] "={a0}" (-> isize),
            : [n] "{a7}" (n),
              [a0] "{a0}" (a0),
              [a1] "{a1}" (a1),
              [a2] "{a2}" (a2),
            : .{ .memory = true });
    }

    /// Reads `buf.len` bytes from `fd` (0, stdin), fewer only at the end of the input.
    pub fn read(fd: i32, buf: []u8) isize {
        return syscall3(read_nr, @bitCast(@as(isize, fd)), @intFromPtr(buf.ptr), buf.len);
    }

    /// Writes `buf` to `fd` (1 or 2, both stdout).
    pub fn write(fd: i32, buf: []const u8) isize {
        return syscall3(write_nr, @bitCast(@as(isize, fd)), @intFromPtr(buf.ptr), buf.len);
    }

    /// Sets the program break to `addr` if it can; returns the break after the call, so
    /// `brk(0)` reads it.
    pub fn brk(addr: usize) usize {
        return @bitCast(syscall3(brk_nr, addr, 0, 0));
    }

    /// Ends the program with exit status `code & 255`, with nothing flushed (`malm.exit` flushes
    /// stdout). Linked with `--defsym=__malm_dump_regs=1`, it dumps the registers first (ecall
    /// 255).
    pub fn exit(code: u8) noreturn {
        __malm_exit(code);
    }

    // In assembly: Zig takes the address of a weak symbol for non-null.
    extern fn __malm_exit(code: u32) callconv(.c) noreturn;
    comptime {
        asm (
            \\    .text
            \\    .weak __malm_dump_regs
            \\    .globl __malm_exit
            \\    .type __malm_exit, @function
            \\__malm_exit:
            \\    lui t0, %hi(__malm_dump_regs)
            \\    addi t0, t0, %lo(__malm_dump_regs)
            \\    beqz t0, 1f
            \\    li a7, 255
            \\    ecall
            \\1:  li a7, 93
            \\    ecall
            \\    .size __malm_exit, . - __malm_exit
        );
    }
};

/// Flushes stdout and ends the program with exit status `code`.
pub fn exit(code: u8) noreturn {
    stdout.flush() catch {};
    sys.exit(code);
}

/// Ends the program at once with status 134 (128 + SIGABRT, as C's abort), losing what stdout
/// has buffered.
pub fn abort() noreturn {
    sys.exit(134);
}

fn writeAll(fd: i32, bytes: []const u8) void {
    if (bytes.len > 0) {
        _ = sys.write(fd, bytes);
    }
}

fn stdoutDrain(w: *std.Io.Writer, data: []const []const u8, splat: usize) std.Io.Writer.Error!usize {
    writeAll(1, w.buffer[0..w.end]);
    w.end = 0;
    var n: usize = 0;
    for (data[0 .. data.len - 1]) |bytes| {
        writeAll(1, bytes);
        n += bytes.len;
    }
    const pattern = data[data.len - 1];
    for (0..splat) |_| {
        writeAll(1, pattern);
    }
    return n + pattern.len * splat;
}

fn stderrDrain(w: *std.Io.Writer, data: []const []const u8, splat: usize) std.Io.Writer.Error!usize {
    stdout.flush() catch {};
    return stdoutDrain(w, data, splat);
}

var stdout_buffer: [1024]u8 = undefined;

/// The standard output, buffered (1024 bytes): written when the buffer is full, by `flush`, before
/// a read from stdin, before a write to stderr and by the exit. `print` flushes after a newline,
/// as the C runtime's line-buffered stdout does.
pub var stdout: std.Io.Writer = .{
    .vtable = &.{ .drain = stdoutDrain },
    .buffer = &stdout_buffer,
};

/// The standard error, unbuffered. It also goes to stdout, which is flushed first.
pub var stderr: std.Io.Writer = .{
    .vtable = &.{ .drain = stderrDrain },
    .buffer = &.{},
};

/// Formats to stdout, as `std.Io.Writer.print`, and flushes it if the text has a newline.
pub fn print(comptime fmt: []const u8, args: anytype) void {
    const start = stdout.end;
    stdout.print(fmt, args) catch {};
    if (std.mem.indexOfScalar(u8, stdout.buffer[0..stdout.end], '\n') != null or stdout.end < start) {
        stdout.flush() catch {};
    }
}

/// Formats to stderr.
pub fn eprint(comptime fmt: []const u8, args: anytype) void {
    stderr.print(fmt, args) catch {};
}

// One byte per `read`: a read waits until it has every byte asked for (docs/rv.md 4), so
// reading ahead would wait for input the program has not asked for yet.
fn stdinStream(r: *std.Io.Reader, w: *std.Io.Writer, limit: std.Io.Limit) std.Io.Reader.StreamError!usize {
    _ = r;
    if (limit == .nothing) {
        return 0;
    }
    stdout.flush() catch {}; // first: `w` may be stdout
    const dest = limit.slice(try w.writableSliceGreedy(1));
    if (sys.read(0, dest[0..1]) != 1) {
        return error.EndOfStream;
    }
    w.advance(1);
    return 1;
}

var stdin_buffer: [4096]u8 = undefined;

/// The standard input. It reads one byte per system call (see above); lines up to 4096 bytes fit
/// its buffer (`takeDelimiter`), longer input can be streamed or read into an allocation.
pub var stdin: std.Io.Reader = .{
    .vtable = &.{ .stream = stdinStream },
    .buffer = &stdin_buffer,
    .seek = 0,
    .end = 0,
};

/// The heap: first-fit with an address-ordered circular free list, as in K&R section 8.7 and the
/// C runtime's malloc. Blocks are a header plus whole 16-byte units; the heap grows through `brk`,
/// at most up to `__stack_size` bytes below the initial stack pointer.
const heap = struct {
    const Header = extern struct {
        next: ?*Header align(16), // next free block (free blocks only)
        units: usize, // size of the block, header included
    };

    const unit = @sizeOf(Header);
    const min_grow_units = 256; // grow the heap by at least 4 KiB at a time

    var base: Header = .{ .next = null, .units = 0 }; // empty list head, size 0
    var freep: ?*Header = null; // where the last search ended

    extern const __stack_size: u8;

    fn limit() usize {
        const reserve = @intFromPtr(&__stack_size);
        return stack_top -| reserve;
    }

    // Extends the heap by `units` units at a unit-aligned address (the program may move the
    // break itself).
    fn more(units: usize) ?*Header {
        const start = std.mem.alignForward(usize, sys.brk(0), unit);
        const size = std.math.mul(usize, units, unit) catch return null;
        const end = std.math.add(usize, start, size) catch return null;
        if (end > limit() or sys.brk(end) != end) {
            return null;
        }
        return @ptrFromInt(start);
    }

    fn grow(units: usize) ?*Header {
        var n = @max(units, min_grow_units);
        var block = more(n);
        if (block == null and n != units) {
            n = units; // not enough room for the minimum, try exactly what is needed
            block = more(n);
        }
        const b = block orelse return null;
        b.units = n;
        free(b);
        return freep;
    }

    fn malloc(size: usize) ?[*]u8 {
        const units = (std.math.add(usize, size, unit - 1) catch return null) / unit + 1;
        if (freep == null) {
            base.next = &base;
            freep = &base;
        }
        var prev = freep.?;
        var p = prev.next.?;
        while (true) {
            if (p.units >= units) {
                if (p.units == units) {
                    prev.next = p.next;
                } else {
                    p.units -= units; // hand out the tail
                    p = @ptrCast(@as([*]Header, @ptrCast(p)) + p.units);
                    p.units = units;
                }
                freep = prev;
                return @ptrCast(@as([*]Header, @ptrCast(p)) + 1);
            }
            if (p == freep.?) {
                p = grow(units) orelse return null;
            }
            prev = p;
            p = p.next.?;
        }
    }

    // `block` is a block's header.
    fn free(block: *Header) void {
        const after = struct {
            fn f(h: *Header) *Header {
                return @ptrCast(@as([*]Header, @ptrCast(h)) + h.units);
            }
        }.f;
        var p = freep.?;
        while (!(@intFromPtr(block) > @intFromPtr(p) and @intFromPtr(block) < @intFromPtr(p.next.?))) {
            if (@intFromPtr(p) >= @intFromPtr(p.next.?) and
                (@intFromPtr(block) > @intFromPtr(p) or @intFromPtr(block) < @intFromPtr(p.next.?)))
            {
                break; // at the end of the list, before its first or after its last block
            }
            p = p.next.?;
        }
        if (after(block) == p.next.?) {
            block.units += p.next.?.units;
            block.next = p.next.?.next;
        } else {
            block.next = p.next;
        }
        if (after(p) == block) {
            p.units += block.units;
            p.next = block.next;
        } else {
            p.next = block;
        }
        freep = p;
    }

    fn header(memory: [*]u8) *Header {
        return @ptrCast(@as([*]Header, @ptrCast(@alignCast(memory))) - 1);
    }

    // Alignments up to 16 come with every block. A larger one gets `alignment` bytes more, and
    // the address of the block's data in the word before the aligned pointer.
    fn alloc(_: *anyopaque, len: usize, alignment: std.mem.Alignment, _: usize) ?[*]u8 {
        const a = alignment.toByteUnits();
        if (a <= unit) {
            return malloc(len);
        }
        const data = malloc(std.math.add(usize, len, a) catch return null) orelse return null;
        const p: [*]u8 = @ptrFromInt(std.mem.alignForward(usize, @intFromPtr(data) + 1, a));
        @as(*align(1) usize, @ptrCast(p - @sizeOf(usize))).* = @intFromPtr(data);
        return p;
    }

    fn blockData(memory: [*]u8, alignment: std.mem.Alignment) [*]u8 {
        if (alignment.toByteUnits() <= unit) {
            return memory;
        }
        return @ptrFromInt(@as(*align(1) const usize, @ptrCast(memory - @sizeOf(usize))).*);
    }

    // In place if the block is big enough.
    fn resize(_: *anyopaque, memory: []u8, alignment: std.mem.Alignment, new_len: usize, _: usize) bool {
        if (alignment.toByteUnits() > unit) {
            return new_len <= memory.len;
        }
        return new_len <= (header(memory.ptr).units - 1) * unit;
    }

    fn remap(ctx: *anyopaque, memory: []u8, alignment: std.mem.Alignment, new_len: usize, ra: usize) ?[*]u8 {
        return if (resize(ctx, memory, alignment, new_len, ra)) memory.ptr else null;
    }

    fn freeMemory(_: *anyopaque, memory: []u8, alignment: std.mem.Alignment, _: usize) void {
        free(header(blockData(memory.ptr, alignment)));
    }

    const vtable: std.mem.Allocator.VTable = .{
        .alloc = alloc,
        .resize = resize,
        .remap = remap,
        .free = freeMemory,
    };
};

/// The general-purpose allocator, on the heap above.
pub const allocator: std.mem.Allocator = .{ .ptr = undefined, .vtable = &heap.vtable };

/// The initial stack pointer, set by start.zig for the heap limit.
pub var stack_top: usize = 0;
