"""Prepare isolated GetTextureGradients translation without installing shaders."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('base_source', type=Path)
p.add_argument('scratch', type=Path)
a = p.parse_args()
source = a.scratch.resolve()/'source'
source.mkdir(parents=True, exist_ok=True)
manifest = {}
for path in a.base_source.iterdir():
    if path.suffix in ('.cpp', '.h') or path.name == 'CMakeLists.txt':
        shutil.copyfile(path, source/path.name)
        manifest[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
compiler = (source/'shader_recompiler.cpp').read_text()

def replace_once(old, new):
    global compiler
    if compiler.count(old) != 1:
        raise ValueError('Expected one source anchor: ' + old[:100])
    compiler = compiler.replace(old, new)

replace_once('if (instr.opcode != FetchOpcode::TextureFetch && instr.opcode != FetchOpcode::GetTextureWeights)\n        return;',
    'if (instr.opcode != FetchOpcode::TextureFetch && instr.opcode != FetchOpcode::GetTextureWeights &&\n'
    '        instr.opcode != FetchOpcode::GetTextureGradients)\n        return;')
replace_once('    std::string constName;\n', '''    if (instr.opcode == FetchOpcode::GetTextureGradients)
    {
        if (!isPixelShader || instr.srcRegisterAm || instr.dstRegisterAm)
            throw std::runtime_error("Gradient candidate: vertex/relative addressing unsupported");
        bool writesGradient = false;
        for (size_t lane = 0; lane < 4; ++lane)
            writesGradient |= getDestSwizzle(instr.dstSwizzle, lane) <= FetchDestinationSwizzle::W;
        if (writesGradient)
        {
            indent();
            print("r{}.", instr.dstRegister);
            printDstSwizzle(instr.dstSwizzle, false);
            out += " = float4(ddx_coarse(";
            printSrcRegister(1);
            out += "), ddy_coarse(";
            printSrcRegister(1);
            out += "), ddx_coarse(";
            print("r{}.{}", instr.srcRegister, SWIZZLES[(instr.srcSwizzle >> 2) & 3]);
            out += "), ddy_coarse(";
            print("r{}.{}", instr.srcRegister, SWIZZLES[(instr.srcSwizzle >> 2) & 3]);
            out += ")).";
            printDstSwizzle(instr.dstSwizzle, true);
            out += ";\\n";
        }
        printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);
        if (instr.isPredicated)
        {
            --indentation;
            indent();
            out += "}\\n";
        }
        return;
    }

    std::string constName;
''')
(source/'shader_recompiler.cpp').write_text(compiler)
cmake = (source/'CMakeLists.txt').read_text().replace('alpha_compiler', 'gradient_compiler')
(source/'CMakeLists.txt').write_text(cmake)
(a.scratch/'source-proof.json').write_text(json.dumps({
    'base_sha256': manifest, 'shared_bytes': 624,
    'semantics': 'coarse derivatives in dx(x),dy(x),dx(y),dy(y) order; source/destination swizzle and predication',
    'installed': False, 'game_launched': False}, indent=2)+'\n')
print(source)
