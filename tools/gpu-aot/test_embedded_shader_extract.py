"""Actual DAT/HDR/DFLT + extractor CLI regression, with permuted name-tree/CRC indices."""
import argparse
from html import escape
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--dotnet', default='dotnet')
parser.add_argument('--negative-control', choices=('name-tree', 'early-publish', 'decoded-count', 'overlong-output'))
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
sources = list((root / 'tools/gpu-shader-extract').glob('*.cs'))
if args.negative_control:
    source = root / 'tools/gpu-shader-extract' / ('Program.cs' if args.negative_control == 'name-tree' else 'EmbeddedShaderExtractor.cs')
    text = source.read_text()
    if args.negative_control == 'name-tree':
        old, new = 'int entryIndex = archive.FindEntry(internalPath);', 'int entryIndex = nameTreeIndex;'
    elif args.negative_control == 'early-publish':
        old = 'if (produced != size) throw new InvalidDataException("Decoded entry size differs from indexed size");'
        new = '// negative control: omit actual final decoded-size guard'
    elif args.negative_control == 'decoded-count':
        old = 'if (Dflt.DecompressChunk(decoder) != 1 || decoder.OutputOffset != u || decoder.InputOffset > c)'
        new = 'if (Dflt.DecompressChunk(decoder) != 1)'
    else:
        old, new = 'decoder.OutputLength = u+1;', 'decoder.OutputLength = u;'
    if text.count(old) != 1:
        raise RuntimeError('Actual extraction guard changed; update negative control')
    altered = output / source.name
    altered.write_text(text.replace(old, new))
    sources = [altered if p == source else p for p in sources]
sources += [root / 'DimensionsModManager/DatArchive.cs', root / 'DimensionsModManager/Dflt.cs']
items = '\n'.join(f'<Compile Include="{escape(str(p), quote=True)}" />' for p in sources)
(output / 'fixture.csproj').write_text(f'''<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>
<OutputType>Exe</OutputType><TargetFramework>net8.0</TargetFramework><ImplicitUsings>enable</ImplicitUsings>
<Nullable>enable</Nullable><EnableDefaultCompileItems>false</EnableDefaultCompileItems><StartupObject>EmbeddedFixtures</StartupObject>
</PropertyGroup><ItemGroup>{items}<Compile Include="Fixture.cs" /></ItemGroup></Project>''')
(output / 'Fixture.cs').write_text(r'''
using System.Buffers.Binary;
using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using GpuShaderExtract;
using DimensionsModManager;

internal static class EmbeddedFixtures {
 static int checks;
 static void Check(bool value,string message) {if(!value)throw new Exception(message);++checks;}
 static void Be(byte[] b,int off,uint v)=>BinaryPrimitives.WriteUInt32BigEndian(b.AsSpan(off,4),v);
 static void Le(byte[] b,int off,uint v)=>BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(off,4),v);
 static string Sha(byte[] b)=>Convert.ToHexString(SHA256.HashData(b)).ToLowerInvariant();
 static byte[] Shader(bool vertex=false) {
  byte[] b=new byte[288];Be(b,0,vertex?0x102a1111u:0x102a1100u);Be(b,4,256);Be(b,8,32);Be(b,24,36);
  Be(b,36,0);Be(b,40,12);for(int i=256;i<b.Length;++i)b[i]=(byte)(i+0x91);return b;
 }
 static byte[] Join(params byte[][] values){var b=new byte[values.Sum(v=>v.Length)];int p=0;foreach(var v in values){v.CopyTo(b,p);p+=v.Length;}return b;}
 static byte[] Packed(byte[] bytes,bool compress=false){byte[] payload=bytes;if(compress){using var o=new MemoryStream();using(var d=new DeflateStream(o,CompressionLevel.SmallestSize,true))d.Write(bytes);payload=o.ToArray();}
  var h=new byte[12];"DFLT"u8.CopyTo(h);Le(h,4,(uint)payload.Length);Le(h,8,(uint)bytes.Length);return Join(h,payload);}
 static byte[] Stored(byte[] actual,uint declared){var block=new byte[actual.Length+3];block[0]=5;BinaryPrimitives.WriteUInt16LittleEndian(block.AsSpan(1,2),(ushort)actual.Length);actual.CopyTo(block,3);
  var h=new byte[12];"DFLT"u8.CopyTo(h);Le(h,4,(uint)block.Length);Le(h,8,declared);return Join(h,block);}
 static byte[] Fixed(byte[] actual,uint declared){var bits=new List<int>{1,1,0};
  void Symbol(int value){int length=value<=143?8:value<=255?9:7;int code=value<=143?0x30+value:value<=255?0x190+value-144:value-256;for(int i=length-1;i>=0;--i)bits.Add((code>>i)&1);}
  foreach(byte b in actual)Symbol(b);Symbol(256);byte[] payload=new byte[(bits.Count+7)/8];for(int i=0;i<bits.Count;++i)payload[i/8]|=(byte)(bits[i]<<(i%8));
  var h=new byte[12];"DFLT"u8.CopyTo(h);Le(h,4,(uint)payload.Length);Le(h,8,declared);return Join(h,payload);}
 record Entry(byte[] Data,uint Size,long? Offset=null);
 static string Archive(string root,string name,Entry[] entries,params (string Name,int Index)[] names){
  string path=Path.Combine(root,name+".DAT");using var dat=File.Create(path);
  var offsets=new long[entries.Length];for(int i=0;i<entries.Length;++i){offsets[i]=entries[i].Offset??dat.Position;dat.Write(entries[i].Data);}
  byte[] blob=Encoding.ASCII.GetBytes(string.Join('\0',names.Select(n=>n.Name))+"\0");int nodes=32+blob.Length+4,table=nodes+names.Length*12+8,crc=table+entries.Length*16;
  var h=new byte[crc+entries.Length*4];".CC40TAD"u8.CopyTo(h.AsSpan(4));Be(h,12,unchecked((uint)-11));Be(h,16,2);Be(h,20,(uint)entries.Length);Be(h,24,(uint)names.Length);Be(h,28,(uint)blob.Length);blob.CopyTo(h,32);
  int nOff=0;for(int i=0;i<names.Length;++i){Be(h,nodes+i*12,(uint)nOff);h[nodes+i*12+11]=1;nOff+=Encoding.ASCII.GetByteCount(names[i].Name)+1;Be(h,crc+names[i].Index*4,DatArchive.PathCrc32(names[i].Name));}
  for(int i=0;i<entries.Length;++i){BinaryPrimitives.WriteUInt64BigEndian(h.AsSpan(table+i*16,8),(ulong)offsets[i]);Be(h,table+i*16+8,(uint)entries[i].Data.Length);Be(h,table+i*16+12,entries[i].Size);}
  File.WriteAllBytes(Path.ChangeExtension(path,".HDR"),h);return path;
 }
 static int Cli(params string[] args)=>(int)typeof(GpuShaderExtract.Program).GetMethod("Main",BindingFlags.NonPublic|BindingFlags.Static)!.Invoke(null,new object[]{args})!;
 static JsonElement Read(string p)=>JsonDocument.Parse(File.ReadAllText(p)).RootElement.Clone();
 static JsonElement[] Rows(string p)=>File.ReadLines(p).Select(l=>JsonDocument.Parse(l).RootElement.Clone()).ToArray();
 static int Main(string[] args){try{Run(args);return 0;}catch(Exception e){Console.Error.WriteLine(e);return 1;}}
 static void Run(string[] args){string root=Path.Combine(Path.GetFullPath(args[0]),Guid.NewGuid().ToString("N"));Directory.CreateDirectory(root);
  var ps=Shader();var vs=Shader(true);string normal=Path.Combine(root,"normal");Directory.CreateDirectory(normal);
  string archive=Archive(normal,"PATCH",new[]{new Entry(Array.Empty<byte>(),0),new Entry(Array.Empty<byte>(),0),new Entry(ps,(uint)ps.Length),new Entry(vs,(uint)vs.Length)},("named_nxg.360_shaders",3),("resource.res",2));
  var parsed=new DatArchive(archive);Check(parsed.EnumerateNames().First().Value==0,"fixture lacks sequential name-tree position");
  Check(parsed.FindEntry("named_nxg.360_shaders")==3&&parsed.FindEntry("resource.res")==2,"fixture CRC permutation not resolved");
  string namedOut=Path.Combine(root,"named-output");Check(Cli("extract",normal,namedOut)==0,"standard extract failed");
  Check(File.ReadAllBytes(Path.Combine(namedOut,"PATCH","named_nxg.360_shaders")).SequenceEqual(vs),"standard extractor read tree position instead of CRC file index");
  string all=Path.Combine(root,"all");Check(Cli("extract","--embedded",normal,all)==0,"all-entry extract failed");
  var index=Read(Path.Combine(all,"embedded-index.json"));Check(index.GetProperty("complete").GetBoolean()&&index.GetProperty("candidates").GetInt32()==2,"all-entry coverage missing");
  var rows=Rows(Path.Combine(all,"containers.jsonl"));Check(rows.Length==2,"wrong origin count");
  foreach(var row in rows){int i=row.GetProperty("entry_index").GetInt32();Check(row.GetProperty("names")[0].GetString()==(i==3?"named_nxg.360_shaders":"resource.res"),"embedded label used tree index");var c=row.GetProperty("candidate");byte[] expected=i==3?vs:ps;
   Check(File.ReadAllBytes(Path.Combine(all,c.GetProperty("file").GetString()!)).SequenceEqual(expected),"container bytes altered");Check(c.GetProperty("container_sha256").GetString()==Sha(expected),"full SHA wrong");Check(c.GetProperty("physical").GetProperty("sha256").GetString()==Sha(expected[256..]),"physical SHA wrong");Check(c.GetProperty("instruction").GetProperty("sha256").GetString()==Sha(expected[256..268]),"instruction SHA wrong");}
  Check(Rows(Path.Combine(all,"entries.jsonl")).Length==4,"unnamed/zero entries absent");Check(Cli("extract","--embedded",normal,all)==2,"fresh-output guard missing");
  // Unnamed entries, a chunk seam, nested header starts, actual DEFLATE, and duplicate source origins.
  const int seam=2*1024*1024-2;var seamBytes=new byte[seam+ps.Length];ps.CopyTo(seamBytes,seam);
  var outer=new byte[640];Be(outer,0,0x102a1100);Be(outer,4,608);Be(outer,8,32);Be(outer,24,36);Be(outer,40,12);ps.CopyTo(outer,96);
  string mixed=Path.Combine(root,"mixed");Directory.CreateDirectory(mixed);
  Archive(mixed,"GAME",new[]{new Entry(seamBytes,(uint)seamBytes.Length),new Entry(Packed(vs),(uint)vs.Length),new Entry(Packed(ps,true),(uint)ps.Length),new Entry(outer,(uint)outer.Length)});
  string mixedOut=Path.Combine(root,"mixed-output");Check(Cli("extract","--embedded",mixed,mixedOut)==0,"valid raw/DFLT entries rejected");
  rows=Rows(Path.Combine(mixedOut,"containers.jsonl"));Check(rows.Length==5,"seam/nested/DFLT origins missing");Check(rows.Any(r=>r.GetProperty("candidate").GetProperty("entry_offset").GetInt64()==seam),"boundary candidate missed");Check(rows.Count(r=>r.GetProperty("entry_index").GetInt32()==3)==2,"nested candidate skipped");
  Check(Read(Path.Combine(mixedOut,"embedded-index.json")).GetProperty("unique_containers").GetInt32()==3,"full-byte duplicate accounting wrong");
  // Failed entries cannot leak provisional candidates, including a fully-readable size mismatch.
  string bad=Path.Combine(root,"bad");Directory.CreateDirectory(bad);var badTail=Join(Packed(Join(vs,new byte[64])),new byte[]{1,2,3});
  var seeded=Join(new byte[64],ps);
  // Actual custom final stored block returns1 after writing one byte, even
  // though its DFLT wrapper declares352. Reused output still contains PS@64.
  Archive(bad,"SPARSE",new[]{new Entry(badTail,352),new Entry(ps,(uint)ps.Length+1),new Entry(vs,(uint)vs.Length,10000000),new Entry(ps,(uint)ps.Length),new Entry(Stored(seeded,352),352),new Entry(Stored(new byte[]{0x41},352),352),new Entry(Fixed(Join(seeded,new byte[]{0x99}),352),352)});
  string badOut=Path.Combine(root,"bad-output");Check(Cli("extract","--embedded",bad,badOut)==3,"incomplete scan reported success");
  rows=Rows(Path.Combine(badOut,"containers.jsonl"));Check(rows.Length==2&&rows.All(r=>r.GetProperty("entry_index").GetInt32() is 3 or 4),"malformed entry candidates published before end validation");
  var ledger=Rows(Path.Combine(badOut,"entries.jsonl"));Check(ledger.Length==7&&ledger.Count(r=>r.GetProperty("status").GetString()=="failed-no-candidates-published")==5,"sparse/decoder/size failures not explicit");
  Check(ledger[5].GetProperty("failure").GetString()!.Contains("wrong byte count"),"short decoder output accepted and stale bytes sampled");
  Check(ledger[6].GetProperty("failure").GetString()!.Contains("wrong byte count"),"overlong fixed-Huffman stream accepted after saturated writes");
  Check(ledger[0].GetProperty("withheld_candidates").GetInt32()==1,"malformed tail did not exercise provisional candidate withholding");
  Check(!Read(Path.Combine(badOut,"embedded-index.json")).GetProperty("complete").GetBoolean(),"partial ledger lost status");
  // An unreadable header retains an explicit unknown inventory, while another archive completes.
  File.WriteAllBytes(Path.Combine(bad,"BROKEN.DAT"),new byte[8]);string failedHeader=Path.Combine(root,"failed-header");Check(Cli("extract","--embedded",bad,failedHeader)==3,"bad embedded header accepted");
  var summary=Read(Path.Combine(failedHeader,"embedded-index.json"));Check(summary.GetProperty("archives").GetArrayLength()==2&&summary.GetProperty("archives")[0].GetProperty("failure").ValueKind==JsonValueKind.String,"unvisited archive/header absent");
  string empty=Path.Combine(root,"empty");Directory.CreateDirectory(empty);Archive(empty,"EMPTY",new[]{new Entry(Array.Empty<byte>(),0)});Check(Cli("extract","--embedded",empty,Path.Combine(root,"empty-out"))==1,"valid zero candidates is not exit1");
  Check(Cli("extract","--embedded",empty,Path.Combine(root,"invalid-options"),"--max-seconds","0")==2,"invalid deadline accepted");
  Console.WriteLine($"actual archive/CLI extraction checks: {checks}");
 }
}
''')
subprocess.run([args.dotnet, 'build', str(output/'fixture.csproj'), '-o', str(output/'bin'), '-m:1',
                '--disable-build-servers', '-p:UseSharedCompilation=false', '-v:q'], check=True, timeout=45)
run = subprocess.run([args.dotnet, str(output/'bin/fixture.dll'), str(output/'data')],
                     timeout=25, capture_output=True, text=True)
print(run.stdout, end='')
print(run.stderr, end='')
if args.negative_control:
    oracle = ('standard extractor read tree position instead of CRC file index'
              if args.negative_control == 'name-tree' else
              'malformed entry candidates published before end validation')
    if run.returncode == 0 or oracle not in run.stderr:
        raise RuntimeError('Negative control did not fail at the required extraction oracle')
    print(f'{args.negative_control} negative control rejected as expected')
else:
    run.check_returncode()
