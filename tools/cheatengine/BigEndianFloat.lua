--[[ Cheat Engine custom type: "Big Endian Float"
     Health, positions and timers in this title are floats. ]]
typeName = "Big Endian Float"
bytecount = 4

function bytesToValue(b1, b2, b3, b4)
  return byteTableToFloat({b4, b3, b2, b1})
end

function valueToBytes(value)
  local t = floatToByteTable(value)
  return t[4], t[3], t[2], t[1]
end
