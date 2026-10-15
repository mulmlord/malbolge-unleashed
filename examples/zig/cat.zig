// Copies stdin to stdout, byte for byte.
const malm = @import("malm");

pub fn main() !void {
    _ = try malm.stdin.streamRemaining(&malm.stdout);
}
