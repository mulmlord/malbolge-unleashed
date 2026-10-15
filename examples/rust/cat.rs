// Copies stdin to stdout, byte for byte.
#![no_std]
#![no_main]
use malm::io;

malm::entry!(main);

fn main() {
    let mut out = io::stdout();
    for b in io::stdin().bytes() {
        out.write(&[b]);
    }
}
