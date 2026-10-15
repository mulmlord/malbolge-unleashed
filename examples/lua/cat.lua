-- Copies stdin to stdout, byte for byte.
while true do
  local block = io.read(4096)
  if not block then break end
  io.write(block)
end
