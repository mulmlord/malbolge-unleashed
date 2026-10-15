-- Self-checking test of Lua's interpreter on picolibc (rv/runtime/lua, docs/rv.md 6), run by
-- rvref_pipe_test.sh. With empty stdin it prints "lua_test: N checks passed" and exits with
-- os.exit(42); a failed check prints a FAIL line and the status is 1. The first line of stdin can
-- choose an ending instead: "panic" indexes nil (the error with a traceback on stderr, status 1),
-- "error" calls error() (the same), "exit" calls os.exit(7) with output still buffered, which exit
-- writes.

local checks, failures = 0, 0

local function check(ok)
  checks = checks + 1
  if not ok then
    failures = failures + 1
    print("FAIL line " .. debug.getinfo(2, "l").currentline)
  end
end

local first = io.read("l")
if first == "panic" then
  print("before the panic")
  local t = nil
  return t.field
elseif first == "error" then
  error("asked for it")
elseif first == "exit" then
  io.write("buffered ")
  io.write("until exit")
  os.exit(7)
end

-- Integers are 64-bit, floats double precision (both in software on rv32im).
check(math.maxinteger == 0x7fffffffffffffff)
check(math.maxinteger + 1 == math.mininteger)
check(123456789 * 987654321 == 121932631112635269)
check(7 // 2 == 3 and -7 // 2 == -4 and -7 % 3 == 2)
check(1 / 4 == 0.25 and math.type(2 ^ 10) == "float" and 2 ^ 10 == 1024)
check(tostring(0.1 + 0.2) == "0.30000000000000004" and tostring(1e100) == "1e+100")
check(tostring(10 // 0.0) == "inf" and tostring(-(0 / 0)) ~= nil)
check(string.format("%.3f %g %5.1f", math.pi, 1e20, 2.3) == "3.142 1e+20   2.3")
check(math.abs(math.sqrt(2) ^ 2 - 2) < 1e-15 and math.floor(-3.5) == -4)
check(math.tointeger(3.0) == 3 and math.tointeger(3.5) == nil)
check(tonumber("0x10") == 16 and tonumber("1e2") == 100.0 and tonumber("z", 36) == 35)
check(tonumber("12abc") == nil and math.ult(1, -1))

-- Strings, patterns, string.pack and utf8.
check(("hello"):upper() == "HELLO" and ("x"):rep(3, "-") == "x-x-x")
check(select(2, ("a,b,,c"):gsub(",", ";")) == 3)
local key, value = ("key = value"):match("^(%w+)%s*=%s*(%w+)$")
check(key == "key" and value == "value")
check(string.format("%d %s %q", 42, "x", "a\nb") == '42 x "a\\\nb"')
check(("%5s|%-5s|"):format("ab", "cd") == "   ab|cd   |")
check(string.unpack("<i4", string.pack("<i4", -2)) == -2 and #string.pack("<d", 1.5) == 8)
check(#"\u{e4}" == 2 and utf8.len("\u{e4}x") == 2 and utf8.char(228) == "\u{e4}")
check(("\0a\0"):len() == 3 and ("a\0b"):find("\0", 1, true) == 2)

-- Tables, sorting, closures, varargs, metatables, coroutines.
local squares = {}
for i = 1, 100 do
  squares[i] = i * i
end
check(#squares == 100 and squares[10] == 100)
table.sort(squares, function(a, b) return a > b end)
check(squares[1] == 10000 and squares[100] == 1)
check(table.concat({ 1, 2, 3 }, ",") == "1,2,3")
check(select("#", table.unpack({ 1, nil, 3 }, 1, 3)) == 3)
local function counter()
  local n = 0
  return function()
    n = n + 1
    return n
  end
end
local count = counter()
count()
check(count() == 2)
local V = setmetatable({}, {
  __add = function() return "added" end,
  __index = function(_, k) return k .. "!" end,
})
check(V + V == "added" and V.x == "x!")
local co = coroutine.wrap(function(a)
  local b = coroutine.yield(a + 1)
  return b * 2
end)
check(co(1) == 2 and co(5) == 10)

-- Errors, load, the collector.
local ok, err = pcall(error, { code = 3 })
check(not ok and err.code == 3)
ok, err = pcall(function() return nil + 1 end)
check(not ok and err:find("attempt to perform arithmetic on a nil value") ~= nil)
check(select(2, pcall(error, "plain", 0)) == "plain")
check(load("return 1 + ...")(41) == 42 and load("syntax error") == nil)
for i = 1, 5000 do
  local _ = { i }
end
collectgarbage()
check(collectgarbage("count") > 0)
check(arg[0] == "lua_test.lua")

-- The machine: no files, no clock, no shell, no environment. The rest of stdin is still there.
check(io.open("file", "r") == nil and select(2, os.remove("file")) ~= nil)
check(os.execute() == false and os.getenv("HOME") == nil)
check(select(2, pcall(os.time)):find("cannot be represented") ~= nil)
check(io.read("a") == "" and io.read("l") == nil)

print("lua_test: " .. (checks - failures) .. " checks passed")
os.exit(failures == 0 and 42 or 1)
