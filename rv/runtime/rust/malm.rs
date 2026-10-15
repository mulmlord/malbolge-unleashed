//! The Rust runtime (docs/rv.md 6): the crate `malm` that `malm build` compiles for
//! riscv32im-unknown-none-elf and links every Rust program with. It has the entry point, the system
//! calls of docs/rv.md 4, a global allocator on `brk`, stdin, stdout and stderr with the C
//! runtime's buffering, `print!` and the like, `process::exit` and the panic handler.
//!
//! A program is `#![no_std]` and `#![no_main]`, and names its `main` with `malm::entry!`:
//!
//! ```ignore
//! #![no_std]
//! #![no_main]
//! use malm::prelude::*;
//!
//! malm::entry!(main);
//!
//! fn main() {
//!     println!("Hello, world!");
//! }
//! ```
#![no_std]

extern crate alloc;

use core::cell::UnsafeCell;

/// A global of the runtime. The guest has one hart and no interrupts, so nothing runs concurrently;
/// a reference into one never lives across a call that could reach the same global.
struct Global<T>(UnsafeCell<T>);

unsafe impl<T> Sync for Global<T> {}

impl<T> Global<T> {
    const fn new(value: T) -> Self {
        Global(UnsafeCell::new(value))
    }

    #[allow(clippy::mut_from_ref)]
    unsafe fn get(&self) -> &mut T {
        unsafe { &mut *self.0.get() }
    }

    fn ptr(&self) -> *mut T {
        self.0.get()
    }
}

// _start: clears .bss, aligns sp to 16 (the ilp32 ABI) and calls __malm_start with it, as crt0.S
// does for C (there are no static constructors in Rust). __malm_exit dumps the registers (ecall 255)
// if the program is linked with --defsym=__malm_dump_regs=1, then ends it (ecall 93).
core::arch::global_asm!(
    r#"
    .section .text.init, "ax", @progbits
    .globl _start
    .type _start, @function
_start:
    la t0, __bss_start
    la t1, __bss_end
    j 2f
1:  sw zero, 0(t0)
    sw zero, 4(t0)
    sw zero, 8(t0)
    sw zero, 12(t0)
    addi t0, t0, 16
2:  bltu t0, t1, 1b
    andi sp, sp, -16
    mv a0, sp
    call __malm_start
    .size _start, . - _start

    .text
    .weak __malm_dump_regs
    .globl __malm_exit
    .type __malm_exit, @function
__malm_exit:
    lui t0, %hi(__malm_dump_regs)
    addi t0, t0, %lo(__malm_dump_regs)
    beqz t0, 1f
    li a7, 255
    ecall
1:  li a7, 93
    ecall
    .size __malm_exit, . - __malm_exit
"#
);

unsafe extern "C" {
    fn __malm_exit(code: i32) -> !;
    fn __malm_main() -> i32;
    static __stack_size: u8;
}

static STACK_TOP: Global<usize> = Global::new(0);

#[unsafe(no_mangle)]
extern "C" fn __malm_start(sp: usize) -> ! {
    unsafe { *STACK_TOP.get() = sp };
    process::exit(unsafe { __malm_main() })
}

/// The system calls of docs/rv.md 4, as the machine has them: the result is the value of `a0`, a
/// negated errno value on failure.
pub mod sys {
    use core::arch::asm;

    pub const SYS_READ: usize = 63;
    pub const SYS_WRITE: usize = 64;
    pub const SYS_EXIT: usize = 93;
    pub const SYS_BRK: usize = 214;

    /// `ecall` number `n` with three arguments.
    ///
    /// # Safety
    /// The call may read or write guest memory as the arguments say, or end the program.
    #[inline]
    pub unsafe fn syscall3(n: usize, a0: usize, a1: usize, a2: usize) -> isize {
        let r: isize;
        unsafe {
            asm!("ecall", inlateout("a0") a0 => r, in("a1") a1, in("a2") a2, in("a7") n,
                 options(nostack));
        }
        r
    }

    /// Reads up to `buf.len()` bytes from `fd` (0, stdin): fewer only at the end of the input.
    pub fn read(fd: i32, buf: &mut [u8]) -> isize {
        unsafe { syscall3(SYS_READ, fd as usize, buf.as_mut_ptr() as usize, buf.len()) }
    }

    /// Writes `buf` to `fd` (1 or 2, both stdout).
    pub fn write(fd: i32, buf: &[u8]) -> isize {
        unsafe { syscall3(SYS_WRITE, fd as usize, buf.as_ptr() as usize, buf.len()) }
    }

    /// Ends the program with exit status `code & 255`, with nothing flushed (process::exit
    /// flushes stdout).
    pub fn exit(code: i32) -> ! {
        unsafe { super::__malm_exit(code) }
    }

    /// Sets the program break to `addr` if it can; returns the break after the call, so `brk(0)`
    /// reads it.
    ///
    /// # Safety
    /// Moving the break under memory in use frees it; the global allocator owns the heap.
    pub unsafe fn brk(addr: usize) -> usize {
        unsafe { syscall3(SYS_BRK, addr, 0, 0) as usize }
    }
}

/// The global allocator: first-fit with an address-ordered circular free list, as in K&R section
/// 8.7 and the C runtime's malloc. Blocks are a header plus whole 16-byte units; the heap grows
/// through `brk`, at most up to `__stack_size` bytes below the initial stack pointer.
mod heap {
    use super::{Global, STACK_TOP, sys};
    use core::alloc::{GlobalAlloc, Layout};
    use core::ptr::{self, null_mut};

    #[repr(C, align(16))]
    struct Header {
        next: *mut Header, // next free block (free blocks only)
        units: usize,      // size of the block, header included
    }

    const UNIT: usize = core::mem::size_of::<Header>();
    const MIN_GROW_UNITS: usize = 256; // grow the heap by at least 4 KiB at a time

    struct State {
        base: Header,       // empty list head, size 0
        freep: *mut Header, // where the last search ended, null before the first allocation
    }

    static STATE: Global<State> = Global::new(State {
        base: Header {
            next: null_mut(),
            units: 0,
        },
        freep: null_mut(),
    });

    fn heap_limit() -> usize {
        let reserve = &raw const super::__stack_size as usize;
        let top = unsafe { *STACK_TOP.get() };
        top.saturating_sub(reserve)
    }

    // Extends the heap by `units` units at a unit-aligned address (the program may move the break
    // itself).
    unsafe fn more(units: usize) -> *mut Header {
        let start = unsafe { sys::brk(0) }.next_multiple_of(UNIT);
        let Some(end) = units.checked_mul(UNIT).and_then(|n| n.checked_add(start)) else {
            return null_mut();
        };
        if end > heap_limit() || unsafe { sys::brk(end) } != end {
            return null_mut();
        }
        start as *mut Header
    }

    // The allocator's functions reach the state through a raw pointer: the free list points into
    // it (`base`), so a `&mut State` would alias the list's pointers.
    unsafe fn grow(s: *mut State, units: usize) -> *mut Header {
        let mut n = units.max(MIN_GROW_UNITS);
        let mut block = unsafe { more(n) };
        if block.is_null() && n != units {
            n = units; // not enough room for the minimum, try exactly what is needed
            block = unsafe { more(n) };
        }
        if block.is_null() {
            return null_mut();
        }
        unsafe {
            (*block).units = n;
            free(s, block);
            (*s).freep
        }
    }

    unsafe fn malloc(s: *mut State, size: usize) -> *mut Header {
        let Some(units) = size.checked_add(UNIT - 1).map(|n| n / UNIT + 1) else {
            return null_mut();
        };
        unsafe {
            if (*s).freep.is_null() {
                (*s).base.next = &raw mut (*s).base;
                (*s).freep = &raw mut (*s).base;
            }
            let mut prev = (*s).freep;
            let mut p = (*prev).next;
            loop {
                if (*p).units >= units {
                    if (*p).units == units {
                        (*prev).next = (*p).next;
                    } else {
                        (*p).units -= units; // hand out the tail
                        p = p.add((*p).units);
                        (*p).units = units;
                    }
                    (*s).freep = prev;
                    return p.add(1);
                }
                if p == (*s).freep {
                    p = grow(s, units);
                    if p.is_null() {
                        return null_mut();
                    }
                }
                prev = p;
                p = (*p).next;
            }
        }
    }

    // `block` is a block's header.
    unsafe fn free(s: *mut State, block: *mut Header) {
        unsafe {
            let mut p = (*s).freep;
            while !(block > p && block < (*p).next) {
                if p >= (*p).next && (block > p || block < (*p).next) {
                    break; // at the end of the list, before its first or after its last block
                }
                p = (*p).next;
            }
            if block.add((*block).units) == (*p).next {
                (*block).units += (*(*p).next).units;
                (*block).next = (*(*p).next).next;
            } else {
                (*block).next = (*p).next;
            }
            if p.add((*p).units) == block {
                (*p).units += (*block).units;
                (*p).next = (*block).next;
            } else {
                (*p).next = block;
            }
            (*s).freep = p;
        }
    }

    struct Allocator;

    // Alignments up to 16 come with every block. A larger one gets `align` bytes more, and the
    // address of the block's data in the word before the aligned pointer.
    unsafe impl GlobalAlloc for Allocator {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            let s = STATE.ptr();
            if layout.align() <= UNIT {
                return unsafe { malloc(s, layout.size()) } as *mut u8;
            }
            let Some(size) = layout.size().checked_add(layout.align()) else {
                return null_mut();
            };
            let data = unsafe { malloc(s, size) } as *mut u8;
            if data.is_null() {
                return data;
            }
            let p = unsafe { data.add(layout.align() - data as usize % layout.align()) };
            unsafe { (p as *mut *mut u8).sub(1).write(data) };
            p
        }

        unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
            let data = if layout.align() <= UNIT {
                ptr
            } else {
                unsafe { (ptr as *mut *mut u8).sub(1).read() }
            };
            unsafe { free(STATE.ptr(), (data as *mut Header).sub(1)) }
        }

        unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
            if layout.align() <= UNIT {
                let have = (unsafe { (*(ptr as *mut Header).sub(1)).units } - 1) * UNIT;
                if new_size <= have {
                    return ptr;
                }
            }
            let new =
                unsafe { self.alloc(Layout::from_size_align_unchecked(new_size, layout.align())) };
            if !new.is_null() {
                unsafe {
                    ptr::copy_nonoverlapping(ptr, new, layout.size().min(new_size));
                    self.dealloc(ptr, layout);
                }
            }
            new
        }
    }

    #[global_allocator]
    static ALLOCATOR: Allocator = Allocator;
}

/// stdin, stdout and stderr, buffered as the C runtime's streams (docs/rv.md 6). Reading and
/// writing cannot fail: the machine ends the program on a host I/O error.
pub mod io {
    use super::{Global, sys};
    use alloc::string::String;
    use alloc::vec::Vec;
    use core::fmt;
    use core::str::Utf8Error;

    const BUF_SIZE: usize = 1024;

    struct Buffer {
        len: usize,
        bytes: [u8; BUF_SIZE],
    }

    static STDOUT: Global<Buffer> = Global::new(Buffer {
        len: 0,
        bytes: [0; BUF_SIZE],
    });

    fn write_all(fd: i32, bytes: &[u8]) {
        if !bytes.is_empty() {
            sys::write(fd, bytes);
        }
    }

    /// The standard output, line-buffered: written when the buffer (1024 bytes) is full, after a
    /// newline, before a read from stdin, before a write to stderr and by `process::exit`.
    #[derive(Clone, Copy)]
    pub struct Stdout;

    /// The standard error, unbuffered. It also goes to stdout, which is flushed first.
    #[derive(Clone, Copy)]
    pub struct Stderr;

    /// The standard input, unbuffered: a read waits until it has every byte asked for (docs/rv.md
    /// 4), so reading ahead would wait for input the program has not asked for yet.
    #[derive(Clone, Copy)]
    pub struct Stdin;

    pub fn stdout() -> Stdout {
        Stdout
    }

    pub fn stderr() -> Stderr {
        Stderr
    }

    pub fn stdin() -> Stdin {
        Stdin
    }

    impl Stdout {
        pub fn write(&mut self, bytes: &[u8]) {
            let b = unsafe { STDOUT.get() };
            let mut newline = false;
            for &c in bytes {
                if b.len == BUF_SIZE {
                    write_all(1, &b.bytes);
                    b.len = 0;
                }
                b.bytes[b.len] = c;
                b.len += 1;
                newline |= c == b'\n';
            }
            if newline {
                self.flush();
            }
        }

        pub fn flush(&mut self) {
            let b = unsafe { STDOUT.get() };
            write_all(1, &b.bytes[..b.len]);
            b.len = 0;
        }
    }

    impl Stderr {
        pub fn write(&mut self, bytes: &[u8]) {
            Stdout.flush();
            write_all(2, bytes);
        }
    }

    impl fmt::Write for Stdout {
        fn write_str(&mut self, s: &str) -> fmt::Result {
            self.write(s.as_bytes());
            Ok(())
        }
    }

    impl fmt::Write for Stderr {
        fn write_str(&mut self, s: &str) -> fmt::Result {
            self.write(s.as_bytes());
            Ok(())
        }
    }

    impl Stdin {
        /// Reads `buf.len()` bytes, fewer only at the end of the input; returns how many.
        pub fn read(&mut self, buf: &mut [u8]) -> usize {
            if buf.is_empty() {
                return 0;
            }
            Stdout.flush();
            sys::read(0, buf).max(0) as usize
        }

        /// The next byte, or None at the end of the input.
        pub fn read_byte(&mut self) -> Option<u8> {
            let mut b = [0];
            (self.read(&mut b) == 1).then_some(b[0])
        }

        /// The bytes of stdin, one `read` each.
        pub fn bytes(self) -> impl Iterator<Item = u8> {
            let mut stdin = self;
            core::iter::from_fn(move || stdin.read_byte())
        }

        /// Appends the rest of the input to `buf`; returns the number of bytes read.
        pub fn read_to_end(&mut self, buf: &mut Vec<u8>) -> usize {
            let start = buf.len();
            loop {
                let len = buf.len();
                buf.resize(len + 256, 0);
                let n = self.read(&mut buf[len..]);
                buf.truncate(len + n);
                if n < 256 {
                    return buf.len() - start;
                }
            }
        }

        /// Appends the rest of the input to `buf` if it is UTF-8; returns the number of bytes
        /// read. On an error `buf` is unchanged and the input consumed.
        pub fn read_to_string(&mut self, buf: &mut String) -> Result<usize, Utf8Error> {
            let mut bytes = Vec::new();
            let n = self.read_to_end(&mut bytes);
            buf.push_str(core::str::from_utf8(&bytes)?);
            Ok(n)
        }

        /// Appends the next line, with its newline if it has one, to `buf` if it is UTF-8; returns
        /// the number of bytes read, 0 at the end of the input. On an error `buf` is unchanged and
        /// the line consumed.
        pub fn read_line(&mut self, buf: &mut String) -> Result<usize, Utf8Error> {
            let mut line = Vec::new();
            while let Some(b) = self.read_byte() {
                line.push(b);
                if b == b'\n' {
                    break;
                }
            }
            buf.push_str(core::str::from_utf8(&line)?);
            Ok(line.len())
        }

        /// The lines of the input without their newlines (and a `\r` before one), as `read_line`
        /// reads them.
        pub fn lines(self) -> impl Iterator<Item = Result<String, Utf8Error>> {
            let mut stdin = self;
            core::iter::from_fn(move || {
                let mut line = String::new();
                match stdin.read_line(&mut line) {
                    Ok(0) => None,
                    Ok(_) => {
                        if line.ends_with('\n') {
                            line.pop();
                            if line.ends_with('\r') {
                                line.pop();
                            }
                        }
                        Some(Ok(line))
                    }
                    Err(e) => Some(Err(e)),
                }
            })
        }
    }

    #[doc(hidden)]
    pub fn _print(args: fmt::Arguments) {
        let _ = fmt::Write::write_fmt(&mut Stdout, args);
    }

    #[doc(hidden)]
    pub fn _eprint(args: fmt::Arguments) {
        let _ = fmt::Write::write_fmt(&mut Stderr, args);
    }
}

/// Ending the program, and what `main` may return.
pub mod process {
    use core::fmt;

    /// Flushes stdout and ends the program with exit status `code & 255`.
    pub fn exit(code: i32) -> ! {
        super::io::stdout().flush();
        super::sys::exit(code)
    }

    /// Ends the program at once with status 134 (128 + SIGABRT, as C's abort), losing what stdout
    /// has buffered.
    pub fn abort() -> ! {
        super::sys::exit(134)
    }

    /// An exit status for `main` to return.
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    pub struct ExitCode(u8);

    impl ExitCode {
        pub const SUCCESS: ExitCode = ExitCode(0);
        pub const FAILURE: ExitCode = ExitCode(1);
    }

    impl From<u8> for ExitCode {
        fn from(code: u8) -> Self {
            ExitCode(code)
        }
    }

    /// What `main` may return, as in std: `()` is status 0, an `ExitCode` its value, `Err(e)`
    /// prints `Error: {e:?}` on stderr and is status 1.
    pub trait Termination {
        fn report(self) -> i32;
    }

    impl Termination for () {
        fn report(self) -> i32 {
            0
        }
    }

    impl Termination for ExitCode {
        fn report(self) -> i32 {
            self.0.into()
        }
    }

    impl<T: Termination, E: fmt::Debug> Termination for Result<T, E> {
        fn report(self) -> i32 {
            match self {
                Ok(t) => t.report(),
                Err(e) => {
                    crate::eprintln!("Error: {e:?}");
                    1
                }
            }
        }
    }
}

/// A panic prints `panicked at file:line:column:` and its message on stderr, after what stdout has
/// buffered, and ends the program with status 101, as std does.
#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    match info.location() {
        Some(l) => eprintln!(
            "panicked at {}:{}:{}:\n{}",
            l.file(),
            l.line(),
            l.column(),
            info.message()
        ),
        None => eprintln!("panicked:\n{}", info.message()),
    }
    sys::exit(101)
}

/// Names the program's `main`, a function without arguments that returns `()`, a
/// `process::ExitCode` or a `Result` (`process::Termination`): `malm::entry!(main);`.
#[macro_export]
macro_rules! entry {
    ($main:path) => {
        #[unsafe(no_mangle)]
        extern "C" fn __malm_main() -> i32 {
            $crate::process::Termination::report($main())
        }
    };
}

/// As std's `print!`: to stdout.
#[macro_export]
macro_rules! print {
    ($($arg:tt)*) => { $crate::io::_print(format_args!($($arg)*)) };
}

/// As std's `println!`: to stdout.
#[macro_export]
macro_rules! println {
    () => { $crate::io::_print(format_args!("\n")) };
    ($($arg:tt)*) => { $crate::io::_print(format_args!("{}\n", format_args!($($arg)*))) };
}

/// As std's `eprint!`: to stderr.
#[macro_export]
macro_rules! eprint {
    ($($arg:tt)*) => { $crate::io::_eprint(format_args!($($arg)*)) };
}

/// As std's `eprintln!`: to stderr.
#[macro_export]
macro_rules! eprintln {
    () => { $crate::io::_eprint(format_args!("\n")) };
    ($($arg:tt)*) => { $crate::io::_eprint(format_args!("{}\n", format_args!($($arg)*))) };
}

/// What most programs use: the macros, and `alloc`'s strings, vectors and boxes (std's prelude
/// has them too).
pub mod prelude {
    pub use crate::{eprint, eprintln, print, println};
    pub use alloc::borrow::ToOwned;
    pub use alloc::boxed::Box;
    pub use alloc::string::{String, ToString};
    pub use alloc::vec::Vec;
    pub use alloc::{format, vec};
}
