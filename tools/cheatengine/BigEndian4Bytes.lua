--[[ Cheat Engine custom type: "Big Endian 4 Bytes"
     Value type dropdown -> "Define new custom type (Lua)" -> paste this. ]]
typeName = "Big Endian 4 Bytes"
bytecount = 4

function bytesToValue(b1, b2, b3, b4)
  return b1 * 0x1000000 + b2 * 0x10000 + b3 * 0x100 + b4
end

function valueToBytes(value)
  value = math.floor(value) % 0x100000000
  return math.floor(value / 0x1000000) % 0x100,
         math.floor(value / 0x10000) % 0x100,
         math.floor(value / 0x100) % 0x100,
         value % 0x100
end
