// Sorts the lines of stdin bytewise, like `LC_ALL=C sort` and examples/c/sort.c, with a Vec and
// sort_unstable: lines may hold any bytes, a last line without a newline gets one.
#![no_std]
#![no_main]
use malm::io;
use malm::prelude::*;

malm::entry!(main);

fn main() {
    let mut input = Vec::new();
    io::stdin().read_to_end(&mut input);
    let mut lines: Vec<&[u8]> = input.split(|&b| b == b'\n').collect();
    if input.last().is_none_or(|&b| b == b'\n') {
        lines.pop(); // what follows the last newline, or the empty input
    }
    lines.sort_unstable();
    let mut out = io::stdout();
    for line in lines {
        out.write(line);
        out.write(b"\n");
    }
}
