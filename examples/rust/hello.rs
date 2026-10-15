// Hello world in Rust on malm's Rust runtime (docs/rv.md 6).
#![no_std]
#![no_main]
use malm::prelude::*;

malm::entry!(main);

fn main() {
    println!("Hello, world!");
}
