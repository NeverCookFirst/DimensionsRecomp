"""End-to-end XenosRecomp depth-signature regression using synthetic microcode.

No game assets required. Fixtures and generated outputs go in the supplied
scratch directory, never in the shipping shader corpus/cache.
"""
import argparse
import os
from pathlib import Path
import re
import struct
import subprocess


def fixture(*, depth=False, reflection_depth=False, predicated=False,
            scalar=False, constant_one=False):
    data = bytearray(152)
    struct.pack_into(">9I", data, 0, 0x102A1100, 128, 24, 0, 36, 0, 80, 0, 0)
    struct.pack_into(">8I", data, 36, 32, 28, 0, 0, 0, 0, 0, 0)
    struct.pack_into(">8I", data, 80, 0, 24, 0, 0xFF00, 0, 0, 0,
                     0x10 if reflection_depth else 1)
    # One EXEC_END at CF0 executes the ALU at instruction slot 1; CF1 is NOP.
    struct.pack_into(">3I", data, 128, 1 | (1 << 12), 2 << 12, 0)
    vector_mask = 0 if scalar else (1 if depth else 15)
    scalar_mask = 1 if scalar or constant_one else 0
    alu0 = (61 if depth else 0) | (1 << 15)
    alu0 |= (vector_mask << 16) | (scalar_mask << 20)
    alu1 = ((1 << 28) | (1 << 27)) if predicated else 0
    # MAX r1, r1; scalar ADD r1, r1. Register operands, no texture/constants.
    alu2 = 1 | (1 << 8) | (1 << 16) | (2 << 24) | (7 << 29)
    struct.pack_into(">3I", data, 140, alu0, alu1, alu2)
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("common", type=Path)
    parser.add_argument("scratch", type=Path)
    args = parser.parse_args()
    inputs = args.scratch / "input"
    hlsl_dir = args.scratch / "hlsl"
    inputs.mkdir(parents=True, exist_ok=True)
    cases = {
        "color_only": {},
        "reflection_only": {"reflection_depth": True},
        "depth_vector": {"depth": True},
        "depth_scalar": {"depth": True, "scalar": True},
        "depth_one": {"depth": True, "constant_one": True},
        "depth_predicated": {"depth": True, "predicated": True},
    }
    for name, options in cases.items():
        (inputs / (name + ".bin")).write_bytes(fixture(**options))
    env = os.environ.copy()
    env["XENOS_RECOMP_DXIL_ONLY"] = "1"
    env.pop("XENOS_RECOMP_ONLY_HASH", None)
    subprocess.run([str(args.compiler.resolve()), str(inputs),
                    str(args.scratch / "test-cache.cpp"), str(args.common),
                    str(hlsl_dir)], env=env, check=True)
    for name, options in cases.items():
        source = (hlsl_dir / (name + ".hlsl")).read_text()
        has_depth = options.get("depth", False)
        assert bool(re.search(r"\boDepth\s*:\s*SV_Depth", source)) == has_depth, name
        assert ("oDepth = 0.0;" in source) == has_depth, name
        if has_depth:
            assert "oDepth.x =" in source, name
        else:
            assert "oDepth" not in source, name
    print(f"Depth export regression passed: {len(cases)} HLSL/DXIL fixtures")


if __name__ == "__main__":
    main()
