-- Sorts the lines of stdin bytewise, like `LC_ALL=C sort` and examples/c/sort.c: lines may hold
-- any bytes, a last line without a newline gets one.
local lines = {}
for line in io.lines() do
  lines[#lines + 1] = line
end
table.sort(lines)
for _, line in ipairs(lines) do
  io.write(line, "\n")
end
