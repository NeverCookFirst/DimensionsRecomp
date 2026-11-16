"""Compile and exercise the extractor's actual output-path helper without assets."""

import argparse
from html import escape
from pathlib import Path
import subprocess


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
parser.add_argument("--dotnet", default="dotnet")
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
helper = root / "tools/gpu-shader-extract/ArchiveOutputPath.cs"
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
project = output / "extract-paths.csproj"
project.write_text(f'''<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <ImplicitUsings>enable</ImplicitUsings>
    <Nullable>enable</Nullable>
    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>
  </PropertyGroup>
  <ItemGroup>
    <Compile Include="{escape(str(helper), quote=True)}" Link="ArchiveOutputPath.cs" />
    <Compile Include="Program.cs" />
  </ItemGroup>
</Project>
''')
(output / "Program.cs").write_text(r'''
using GpuShaderExtract;

string root = Path.GetFullPath(Path.Combine(args[0], "archive with spaces"));
StringComparison comparison = OperatingSystem.IsWindows()
    ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
int checks = 0;
void Valid(string input, string expected) {
    string actual = ArchiveOutputPath.Resolve(root, input);
    string destination = Path.GetFullPath(Path.Combine(root,
        expected.Replace('/', Path.DirectorySeparatorChar)));
    if (!string.Equals(actual, destination, comparison))
        throw new Exception($"Unexpected destination for {input}: {actual}");
    checks++;
}
void Reject(string input) {
    try { ArchiveOutputPath.Resolve(root, input); }
    catch (InvalidDataException) { checks++; return; }
    throw new Exception($"Unsafe path accepted: {input}");
}
Valid("shaders/default.pso", "shaders/default.pso");
Valid(@"shaders\default.pso", "shaders/default.pso");
Valid("shader folders/my shader.pso", "shader folders/my shader.pso");
Valid("/shaders/default.pso", "shaders/default.pso");
Valid(@"\shaders\default.pso", "shaders/default.pso");
Valid(@"/\\shaders/default.pso", "shaders/default.pso");
Valid("shaders/../default.pso", "default.pso");
Valid("shaders/./default.pso", "shaders/default.pso");
Reject("../escape.pso");
Reject(@"..\escape.pso");
Reject("shaders/../../escape.pso");
Reject(@"shaders\..\..\escape.pso");
Reject(@"shaders/..\../escape.pso");
Reject("/../escape.pso");
Reject(@"\..\escape.pso");
Reject("../archive with spaces-other/escape.pso");
Reject("C:/escape.pso");
Reject(@"C:\escape.pso");
Reject("c:escape.pso");
Reject(@"\\?\C:\escape.pso");
Reject("/C:/escape.pso");
Reject("shaders/default.pso:stream");
Reject("shaders/invalid\0.pso");
Reject(".");
Reject("shaders/..");
Reject("");
Reject(@"/\\/");
if (Directory.Exists(args[0]))
    throw new Exception("Path validation must not create output directories");
Console.WriteLine($"PASS: {checks} actual extractor path checks on {(OperatingSystem.IsWindows() ? "Windows" : "Unix")}");
''')
subprocess.run([args.dotnet, "build", str(project), "-c", "Release", "-o", str(output / "bin"),
                "--disable-build-servers", "--ignore-failed-sources", "-m:1",
                "-p:UseSharedCompilation=false", "-p:NuGetAudit=false", "--nologo"],
               check=True, timeout=45)
subprocess.run([args.dotnet, str(output / "bin/extract-paths.dll"),
                str(output / "path-validation-does-not-write")], check=True, timeout=10)
