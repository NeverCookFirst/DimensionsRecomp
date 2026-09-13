--[[ Cheat Engine custom type: "Big Endian 2 Bytes" ]]
typeName = "Big Endian 2 Bytes"
bytecount = 2

function bytesToValue(b1, b2)
  return b1 * 0x100 + b2
end

function valueToBytes(value)
  value = math.floor(value) % 0x10000
  return math.floor(value / 0x100) % 0x100, value % 0x100
end
