#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <rex/graphics/format/ucode.h>

namespace legodimensions::gpu_native {

inline uint32_t ShaderRangeWord(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Every clause contributes, including predicated and unreachable instructions.
inline uint32_t ShaderFloatConstantCount(std::span<const uint8_t> code) {
  using namespace rex::graphics::ucode;
  if (code.empty() || code.size() % 12) return 256;
  size_t boundary = code.size() / 12;
  uint32_t registers = 0;
  bool saw_exec = false;
  for (size_t pair = 0; pair < boundary; ++pair) {
    std::array<uint32_t, 3> words;
    for (size_t i = 0; i < 3; ++i) words[i] = ShaderRangeWord(code.data() + pair * 12 + i * 4);
    ControlFlowInstruction clauses[2];
    UnpackControlFlowInstructions(words.data(), clauses);
    for (const auto& clause : clauses) {
      if (!IsControlFlowOpcodeExec(clause.opcode())) continue;
      const auto& exec = clause.exec;
      const size_t address = exec.address();
      if (exec.count() > 6 || address <= pair || address + exec.count() > code.size() / 12) return 256;
      boundary = std::min(boundary, address);
      saw_exec = true;
      for (uint32_t i = 0; i < exec.count(); ++i) {
        if ((exec.sequence() >> (i * 2)) & 1) continue;
        for (size_t w = 0; w < 3; ++w) words[w] = ShaderRangeWord(code.data() + (address + i) * 12 + w * 4);
        AluInstruction alu;
        std::memcpy(&alu, words.data(), sizeof(alu));
        if (alu.is_const_0_addressed() || alu.is_const_1_addressed()) return 256;
        for (size_t src = 1; src <= 3; ++src)
          if (!alu.src_is_temp(src)) registers = std::max(registers, alu.src_reg(src) + 1);
        const auto scalar = uint32_t(alu.scalar_opcode());
        if (scalar >= uint32_t(AluScalarOpcode::kMulsc0) && scalar <= uint32_t(AluScalarOpcode::kSubsc1))
          registers = std::max(registers, alu.src_reg(3) + 1);
      }
    }
  }
  return saw_exec ? registers : 256;
}

inline std::span<const uint8_t> ShaderInstructionBytes(std::span<const uint8_t> header,
                                                     std::span<const uint8_t> physical) {
  if (header.size() < 36) return {};
  const auto offset = ShaderRangeWord(header.data() + 24);
  if (offset > header.size() - 8) return {};
  const auto start = ShaderRangeWord(header.data() + offset);
  const auto size = ShaderRangeWord(header.data() + offset + 4);
  if (start > physical.size() || size > physical.size() - start) return {};
  return physical.subspan(start, size);
}

} // namespace legodimensions::gpu_native
