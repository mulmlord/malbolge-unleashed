// Self-checking test of the Rust runtime (rv/runtime/rust/malm.rs, docs/rv.md 6), run by
// rvref_pipe_test.sh. With empty stdin it prints "rust_test: N checks passed" and exits with main's
// ExitCode 42; a failed check prints a FAIL line and the status is 1. The first line of stdin can
// choose an ending instead: "panic" panics (the message on stderr, status 101), "error" returns an
// Err from main ("Error: ..." on stderr, status 1), "exit" calls process::exit(7) with output still
// buffered, which exit writes.
#![no_std]
#![no_main]

use core::hint::black_box;
use malm::io;
use malm::prelude::*;
use malm::process::{self, ExitCode};

extern crate alloc;
use alloc::collections::{BTreeMap, VecDeque};
use alloc::rc::Rc;

malm::entry!(main);

static mut CHECKS: u32 = 0;
static mut FAILURES: u32 = 0;

fn check(ok: bool, line: u32, what: &str) {
    unsafe {
        CHECKS += 1;
        if !ok {
            FAILURES += 1;
            println!("FAIL line {line}: {what}");
        }
    }
}

macro_rules! check {
    ($e:expr) => {
        check($e, line!(), stringify!($e))
    };
}

trait Shape {
    fn area(&self) -> u32;
}

struct Square(u32);
struct Rect(u32, u32);

impl Shape for Square {
    fn area(&self) -> u32 {
        self.0 * self.0
    }
}

impl Shape for Rect {
    fn area(&self) -> u32 {
        self.0 * self.1
    }
}

#[repr(align(64))]
struct Aligned(u8);

fn check_alloc() {
    let mut v: Vec<u32> = Vec::new();
    for i in 0..1000 {
        v.push(i); // grows by realloc
    }
    check!(v.len() == 1000 && v.iter().sum::<u32>() == 499_500);
    v.truncate(10);
    v.shrink_to_fit();
    check!(v == [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);

    let shapes: Vec<Box<dyn Shape>> = vec![Box::new(Square(3)), Box::new(Rect(2, 5))];
    check!(shapes.iter().map(|s| s.area()).sum::<u32>() == 19);

    let a = Box::new(Aligned(1));
    check!((&*a as *const Aligned as usize) & 63 == 0 && a.0 == 1);

    // Freed blocks are reused: a thousand 1 KiB allocations, one at a time, hardly grow the heap.
    let start = unsafe { malm::sys::brk(0) };
    for _ in 0..1000 {
        black_box(Vec::<u8>::with_capacity(1024));
    }
    let grown = unsafe { malm::sys::brk(0) } - start;
    check!(grown <= 4096);

    // More than the heap fails without ending the program.
    let mut huge: Vec<u8> = Vec::new();
    check!(huge.try_reserve(black_box(8 << 20)).is_err());
    check!(huge.try_reserve(black_box(4096)).is_ok());

    let rc = Rc::new(String::from("shared"));
    let rc2 = Rc::clone(&rc);
    check!(Rc::strong_count(&rc) == 2 && *rc2 == "shared");
}

fn check_library() {
    let mut words = vec!["pear", "apple", "fig", "banana"];
    words.sort();
    check!(words == ["apple", "banana", "fig", "pear"]);
    check!(words.binary_search(&"fig") == Ok(2));

    let mut counts = BTreeMap::new();
    for c in "mississippi".chars() {
        *counts.entry(c).or_insert(0) += 1;
    }
    let summary: String = counts.iter().map(|(c, n)| format!("{c}{n}")).collect();
    check!(summary == "i4m1p2s4");

    let mut queue: VecDeque<u32> = (1..=5).collect();
    queue.rotate_left(2);
    check!(queue.iter().copied().collect::<Vec<_>>() == [3, 4, 5, 1, 2]);

    let parsed: Result<Vec<i32>, _> = "12 -7 30".split(' ').map(str::parse::<i32>).collect();
    check!(parsed == Ok(vec![12, -7, 30]));
    check!("x1".parse::<u8>().is_err());
    check!("héllo".chars().rev().collect::<String>() == "olléh");
    check!("Hello".to_uppercase() == "HELLO");
}

fn check_arithmetic() {
    let a: u64 = black_box(0x1234_5678_9abc_def0);
    check!(a / 1000 == 1_311_768_467_463_790 && a % 1000 == 320);
    let b: i64 = black_box(-9_000_000_000);
    check!(b / 7 == -1_285_714_285 && b % 7 == -5);
    let c: u128 = black_box(u64::MAX as u128) * 3;
    check!(c.to_string() == "55340232221128654845");
    check!(black_box(-17i32).rem_euclid(5) == 3 && black_box(250u8).checked_add(10).is_none());

    // Soft floating point (compiler-builtins) and its formatting (core).
    let x: f64 = black_box(2.0);
    let y: f32 = black_box(1.5);
    check!(x * 1.25 == 2.5 && (y / 3.0) == 0.5);
    check!(
        format!("{:.3} {} {:e} {}", x / 3.0, y, 1234.5f64, 0.1f64 + 0.2)
            == "0.667 1.5 1.2345e3 0.30000000000000004"
    );
    check!(
        format!(
            "{:>6}|{:<4}|{:^5}|{:#x}|{:+}|{:05}",
            "ab", 7, "c", 255, 3, -42
        ) == "    ab|7   |  c  |0xff|+3|-0042"
    );
    check!(format!("{:?}", (1, "a", Some(2.5))) == "(1, \"a\", Some(2.5))");
}

fn main() -> Result<ExitCode, String> {
    let mut first = String::new();
    io::stdin()
        .read_line(&mut first)
        .map_err(|e| e.to_string())?;
    match first.trim_end() {
        "panic" => {
            let v: Vec<u32> = Vec::new();
            println!("before the panic");
            let _ = v[black_box(3)];
        }
        "error" => return Err(String::from("asked for it")),
        "exit" => {
            print!("buffered ");
            print!("until exit");
            process::exit(7);
        }
        _ => {}
    }
    check_alloc();
    check_library();
    check_arithmetic();
    let (checks, failures) = unsafe { (CHECKS, FAILURES) };
    println!("rust_test: {} checks passed", checks - failures);
    Ok(ExitCode::from(if failures == 0 { 42 } else { 1 }))
}
