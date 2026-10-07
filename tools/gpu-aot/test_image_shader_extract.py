"""Asset-free tests linking the actual .NET image extractor and CLI sources."""
import argparse
from html import escape
from pathlib import Path
import subprocess
try:
    import resource
except ImportError:
    pass
else:
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--dotnet', default='dotnet')
parser.add_argument('--negative-control', nargs='?', const='metadata', choices=('metadata', 'nested'),
                    help='Verify the fixture rejects bypassed metadata bounds or skipped nested starts')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
sources = list((root / 'tools/gpu-shader-extract').glob('*.cs'))
if args.negative_control:
    helper = root / 'tools/gpu-shader-extract/ImageShaderExtractor.cs'
    text = helper.read_text()
    if args.negative_control == 'metadata':
        guard = '''if (!ConstantsFit(virtualSection, constants) ||
            !DefinitionsFit(virtualSection, definitions, physicalBytes)) return Result.Invalid;'''
        replacement = 'if (false) return Result.Invalid;'
    else:
        guard = '                        ++position;'
        replacement = '                        position += layout.Total;'
    if text.count(guard) != 1:
        raise RuntimeError('Actual metadata guard changed; update the focused negative control')
    altered = output / 'ImageShaderExtractor.cs'
    altered.write_text(text.replace(guard, replacement))
    sources = [altered if path == helper else path for path in sources]
sources += [root / 'DimensionsModManager/DatArchive.cs', root / 'DimensionsModManager/Dflt.cs']
items = '\n'.join(f'<Compile Include="{escape(str(path), quote=True)}" />' for path in sources)
(output / 'fixture.csproj').write_text(f'''<Project Sdk="Microsoft.NET.Sdk">
<PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net8.0</TargetFramework>
<ImplicitUsings>enable</ImplicitUsings><Nullable>enable</Nullable>
<EnableDefaultCompileItems>false</EnableDefaultCompileItems><StartupObject>ImageFixtures</StartupObject></PropertyGroup>
<ItemGroup>{items}<Compile Include="Fixture.cs" /></ItemGroup></Project>''')
(output / 'Fixture.cs').write_text(r'''
using System.Buffers.Binary;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;
using GpuShaderExtract;

internal static class ImageFixtures {
 static int checks;
 static void Check(bool value, string message) { if (!value) throw new Exception(message); ++checks; }
 static void Word(byte[] bytes, int offset, uint value) => BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan(offset,4), value);
 static string Sha(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
 static byte[] Container(bool vertex, bool marker, bool constants=false, bool definitions=false) {
  const int v=256,p=32; var b=new byte[v+p+(marker?4:0)];
  Word(b,0,vertex?0x102a1111u:0x102a1100u);Word(b,4,v);Word(b,8,p);Word(b,24,36);
  Word(b,36,0);Word(b,40,12);
  if(marker)Word(b,v,p);
  for(int i=0;i<p;++i)b[v+(marker?4:0)+i]=(byte)(0x91+i);
  if(constants){Word(b,16,96);Word(b,112,1);Word(b,116,28);Word(b,128,48);
   "name\0"u8.CopyTo(b.AsSpan(148));}
  if(definitions){Word(b,20,160);Word(b,180,0x01000004);Word(b,184,16);
   Word(b,192,0x23200001);Word(b,196,0x01020304);}
  return b;
 }
 static void Valid(byte[] b) => Check(ImageShaderExtractor.Parse(b,true,out var l)==ImageShaderExtractor.Result.Valid&&l.Total==b.Length,"valid container rejected");
 static void Invalid(byte[] b) => Check(ImageShaderExtractor.Parse(b,true,out _)==ImageShaderExtractor.Result.Invalid,"malformed container accepted");
 static int Cli(params string[] args) => (int)typeof(GpuShaderExtract.Program).GetMethod("Main",BindingFlags.NonPublic|BindingFlags.Static)!.Invoke(null,new object[]{args})!;
 static int Main(string[] args) { try { Run(args); return 0; }
  catch(Exception error) {Console.Error.WriteLine(error.Message);return 1;} }
 static void Run(string[] args) {
  string root=Path.Combine(Path.GetFullPath(args[0]),Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);
  foreach(bool vertex in new[]{false,true})foreach(bool marker in new[]{false,true})
    foreach(bool ct in new[]{false,true})foreach(bool defs in new[]{false,true})Valid(Container(vertex,marker,ct,defs));
  var basic=Container(false,false);
  for(int n=0;n<basic.Length;++n)Invalid(basic[..n]);
  foreach((int offset,uint value) in new (int,uint)[]{(0,0),(4,uint.MaxValue),(8,uint.MaxValue),(8,7),(24,0),
      (24,uint.MaxValue),(28,1),(32,1),(36,uint.MaxValue),(40,7),(40,33),(16,250),(20,252)}) {
   var bad=(byte[])basic.Clone();Word(bad,offset,value);
   Check(ImageShaderExtractor.Parse(bad,true,out _)==ImageShaderExtractor.Result.Invalid,$"malformed field {offset}/{value} accepted");
  }
  var interpBad=(byte[])basic.Clone();Word(interpBad,4,96);Word(interpBad,56,31u<<5);Invalid(interpBad);
  var vertexBad=Container(true,true);Word(vertexBad,64,uint.MaxValue);Invalid(vertexBad);
  var ctBad=Container(false,false,true);Word(ctBad,116,uint.MaxValue);Invalid(ctBad);
  ctBad=Container(false,false,true);Word(ctBad,128,uint.MaxValue);Invalid(ctBad);
  ctBad=Container(false,false,true);Array.Fill(ctBad,(byte)0xff,148,256-148);Invalid(ctBad);
  var defsBad=Container(false,false,false,true);Word(defsBad,184,25);Invalid(defsBad);
  defsBad=Container(false,false,false,true);Word(defsBad,192,0x2320ffff);Invalid(defsBad);
  defsBad=Container(false,false,false,true);Array.Fill(defsBad,(byte)0xff,188,256-188);Invalid(defsBad);
  // A magic/header crossing the2MiB read boundary plus duplicate and bad neighbors.
  var marked=Container(true,true,true,true);
  const int seam=2*1024*1024-2;
  byte[] image=new byte[seam+marked.Length+basic.Length+marked.Length+36];
  marked.CopyTo(image,seam);basic.CopyTo(image,seam+marked.Length);marked.CopyTo(image,seam+marked.Length+basic.Length);
  byte[] invalid=Container(false,false);Word(invalid,4,uint.MaxValue);invalid.AsSpan(0,36).CopyTo(image.AsSpan(image.Length-36));
  string file=Path.Combine(root,"mapped.bin");File.WriteAllBytes(file,image);
  string proof=Path.Combine(root,"proof.json");File.WriteAllText(proof,JsonSerializer.Serialize(new{mapped_image=new{sha256=Sha(image),bytes=image.Length,@base="0x82000000"}}));
  string target=Path.Combine(root,"extracted");
  Check(Cli("extract","--image",file,target,"--image-base","82000000","--image-provenance",proof)==0,"actual CLI failed");
  using(var index=JsonDocument.Parse(File.ReadAllText(Path.Combine(target,"image-index.json")))) {
   var value=index.RootElement;Check(value.GetProperty("candidates").GetInt32()==3,"stream candidates lost");
   Check(value.GetProperty("unique_containers").GetInt32()==2,"duplicate full identity not collapsed");
   Check(value.GetProperty("image").GetProperty("sha256").GetString()==Sha(image),"input SHA wrong");
   var rows=value.GetProperty("records").EnumerateArray().ToArray();
   Check(rows[0].GetProperty("image_offset").GetInt64()==seam,"boundary offset wrong");
   Check(rows[0].GetProperty("guest_address").GetString()==(0x82000000UL+seam).ToString("X8"),"guest address wrong");
   foreach(var row in rows){byte[] original=row.GetProperty("stage").GetString()=="vs"?marked:basic;
    byte[] extracted=File.ReadAllBytes(Path.Combine(target,row.GetProperty("file").GetString()!));
    Check(extracted.SequenceEqual(original),"full container/marker bytes changed");
    Check(row.GetProperty("container_prefix8").GetString()==Convert.ToHexString(original.AsSpan(0,8)),"full container prefix wrong");
    foreach(string section in new[]{"physical","instruction"}){var range=row.GetProperty(section);
     int start=range.GetProperty("offset").GetInt32(),size=range.GetProperty("bytes").GetInt32();
     Check(range.GetProperty("sha256").GetString()==Sha(original.AsSpan(start,size).ToArray()),"section identity wrong");
     Check(range.GetProperty("prefix8").GetString()==Convert.ToHexString(original.AsSpan(start,8)),"placement prefix wrong");}
   }
  }
  Check(File.ReadAllBytes(file).SequenceEqual(image),"source image mutated");
  // Header metadata bounds alone cannot distinguish an enclosing candidate
  // from an actual nested or partially overlapping complete shader container.
  foreach(int start in new[]{96,196}) {
   byte[] inner=new byte[128];Word(inner,0,0x102a1100);Word(inner,4,96);Word(inner,8,32);
   Word(inner,24,36);Word(inner,40,12);for(int i=0;i<32;++i)inner[96+i]=(byte)(0x91+i);
   byte[] overlap=new byte[Math.Max(basic.Length,start+inner.Length)];basic.CopyTo(overlap,0);inner.CopyTo(overlap,start);
   Valid(overlap[..basic.Length]);Valid(inner);
   string overlapFile=Path.Combine(root,"overlap"+start+".bin"),overlapOut=Path.Combine(root,"overlap"+start);
   File.WriteAllBytes(overlapFile,overlap);Check(ImageShaderExtractor.Extract(overlapFile,overlapOut,0x82000000,null)==2,"nested/overlap starts skipped");
   using var overlapIndex=JsonDocument.Parse(File.ReadAllText(Path.Combine(overlapOut,"image-index.json")));
   var rows=overlapIndex.RootElement.GetProperty("records").EnumerateArray().ToArray();
   Check(rows.Select(r=>r.GetProperty("image_offset").GetInt32()).SequenceEqual(new[]{0,start}),"nested/overlap offsets wrong");
   Check(rows[1].GetProperty("container_sha256").GetString()==Sha(inner),"nested/overlap bytes changed");
  }
  Check(Cli("extract","--image",file,target,"--image-base","82000000")==2,"existing output overwritten");
  Check(Cli("extract","--image",file,Path.Combine(root,"hex-prefix"),"--image-base","0x82000000")==0,"CLI0xbase differs from loader provenance");
  Check(Cli("extract","--image",file,Path.Combine(root,"no-base"),"--image-base","bad")==2,"guessed invalid base");
  Check(Cli("extract","--image",file,Path.Combine(root,"overflow"),"--image-base","FFFF0000")==2,"guest range overflow accepted");
  foreach(string badProof in new[]{"{}","[]","{\"mapped_image\":{\"bytes\":1}}",
     "{\"mapped_image\":{\"sha256\":\"wrong\",\"bytes\":3,\"base\":\"82000000\"}}"}) {
   File.WriteAllText(proof,badProof);string reject=Path.Combine(root,"bad-proof-"+checks);
   Check(Cli("extract","--image",file,reject,"--image-base","82000000","--image-provenance",proof)==2,"bad provenance accepted");
   Check(!Directory.Exists(reject),"partial output published on provenance failure");
  }
  Console.WriteLine($"PASS: {checks} actual parser/stream/CLI checks; no game assets, compilation or rendering claim.");
 }
}
''')
subprocess.run([args.dotnet, 'build', str(output/'fixture.csproj'), '-c', 'Release', '-o', str(output/'bin'),
                '--disable-build-servers', '--ignore-failed-sources', '-m:1', '-p:UseSharedCompilation=false',
                '-p:NuGetAudit=false', '--nologo'], check=True, timeout=45)
result = subprocess.run([args.dotnet, str(output/'bin/fixture.dll'), str(output/'data')],
                        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
print(result.stdout, end='')
print(result.stderr, end='')
if args.negative_control:
    oracle = 'malformed' if args.negative_control == 'metadata' else 'nested/overlap starts skipped'
    if result.returncode == 0 or oracle not in result.stderr:
        raise RuntimeError('Altered production source did not fail at its independent fixture oracle')
    print(f'PASS: actual {args.negative_control} negative control failed as expected; no core dump.')
else:
    result.check_returncode()
