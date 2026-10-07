using System.Buffers.Binary;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text.Json;
using DimensionsModManager;

namespace GpuShaderExtract;

// Unlike filename-based extraction, this visits every archive entry. Candidates
// are exact source containers, not CTAB-stripped runtime reconstructions.
internal static class EmbeddedShaderExtractor
{
    private const int Chunk = 2 * 1024 * 1024;
    private const int MaximumHeader = 64 * 1024 * 1024;
    private const int MaximumEntries = 1000000;
    private const int MaximumNames = 250000;
    private const long MaximumDecoded = 64L * 1024 * 1024 * 1024;
    private const long MaximumEntry = 1024L * 1024 * 1024;
    private const long MaximumCandidateWork = 1024L * 1024 * 1024;
    private const long MaximumOutput = 512L * 1024 * 1024;
    private const int MaximumRecords = 100000;
    private static readonly byte[] Magic = { 0x10, 0x2a, 0x11 };
    private static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    private static string Sha(ReadOnlySpan<byte> bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    private static void Line(StreamWriter writer, object value) => writer.WriteLine(JsonSerializer.Serialize(value));
    private sealed class WorkLimit(string message) : IOException(message);
    private sealed class Budget(int seconds)
    {
        private readonly Stopwatch clock = Stopwatch.StartNew();
        internal long Decoded, CandidateBytes, OutputBytes;
        internal int Candidates;
        internal void Check()
        {
            if (clock.Elapsed.TotalSeconds > seconds) throw new WorkLimit("Scan deadline reached");
        }
        internal void Decode(int count)
        {
            Check();
            Decoded += count;
            if (Decoded > MaximumDecoded) throw new WorkLimit("Decoded work exceeds64GiB");
        }
        internal void Candidate(int count)
        {
            Check();
            CandidateBytes += count;
            if (++Candidates > MaximumRecords || CandidateBytes > MaximumCandidateWork)
                throw new WorkLimit("Candidate count/work bound reached");
        }
    }

    internal static int Run(string[] args)
    {
        if (args.Length < 2)
        {
            Console.Error.WriteLine("Usage: gpu-shader-extract extract --embedded <archive-or-directory> <fresh-output> [--max-seconds <1..3600>]");
            return 2;
        }
        try
        {
            int seconds = 300;
            if (args.Length != 2 && (args.Length != 4 || args[2] != "--max-seconds" ||
                !int.TryParse(args[3], out seconds) || seconds < 1 || seconds > 3600))
                throw new ArgumentException("Invalid --max-seconds; expected1..3600");
            return Extract(Path.GetFullPath(args[0]), Path.GetFullPath(args[1]), seconds);
        }
        catch (Exception error) when (error is IOException or InvalidDataException or UnauthorizedAccessException or ArgumentException or InvalidOperationException)
        {
            Console.Error.WriteLine($"Embedded extraction rejected: {error.Message}");
            return 2;
        }
    }

    private static bool IsArchive(string path) => Path.GetExtension(path).Equals(".DAT", StringComparison.OrdinalIgnoreCase) ||
                                                  Path.GetExtension(path).Equals(".DAT2", StringComparison.OrdinalIgnoreCase);

    // DatArchive intentionally reads its metadata into memory. Check allocation,
    // node bounds and expanded path work before invoking that shared API.
    private static (string Sha, bool NamesSupported) Preflight(string path)
    {
        string header = DatArchive.HdrPathFor(path);
        byte[] bytes;
        if (File.Exists(header))
        {
            long length = new FileInfo(header).Length;
            if (length < 12 || length > MaximumHeader) throw new InvalidDataException("Header size outside12B..64MiB");
            bytes = File.ReadAllBytes(header);
        }
        else
        {
            using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
            Span<byte> prefix = stackalloc byte[8]; file.ReadExactly(prefix);
            long offset = BinaryPrimitives.ReadUInt32LittleEndian(prefix);
            if ((offset & 0x80000000) != 0) offset = ((offset ^ 0xffffffff) << 8) + 0x100;
            uint length = BinaryPrimitives.ReadUInt32LittleEndian(prefix[4..]);
            if (length < 8 || length > MaximumHeader - 4 || offset > file.Length || length > file.Length-offset)
                throw new InvalidDataException("Sparse/truncated or oversized embedded header");
            bytes = new byte[(int)length+4]; file.Position = offset; file.ReadExactly(bytes.AsSpan(4));
        }
        bool modern = bytes.Length >= 32 && bytes.AsSpan(4, 8).SequenceEqual(".CC40TAD"u8);
        uint Read(int offset) => modern ? BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan(offset,4)) :
                                          BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(offset,4));
        void Fits(long offset, long size)
        {
            if (offset < 0 || size < 0 || offset > bytes.Length || size > bytes.Length-offset)
                throw new InvalidDataException("Archive metadata outside bounded header");
        }
        int type = unchecked((int)Read(modern ? 12 : 4));
        uint files = Read(modern ? 20 : 8);
        if (files > MaximumEntries) throw new InvalidDataException("Archive exceeds1000000 entries");
        int nodeSize;
        long nodes, blob, blobBytes;
        uint names;
        if (modern)
        {
            uint version = Read(16);
            names = Read(24); blob = 32; blobBytes = Read(28); nodeSize = version >= 2 ? 12 : 10;
            nodes = blob + blobBytes + 4;
            long table = nodes + (long)names * nodeSize + 8;
            Fits(table, (long)files * (type <= -11 ? 16 : type <= -10 ? 12 : 16) + (long)files*4);
        }
        else
        {
            if (type >= 0 || type < -9) throw new InvalidDataException("Unsupported classic archive type");
            long namesOffset = 12 + (long)files*16; Fits(namesOffset,4);
            names = Read((int)namesOffset); nodeSize = type <= -5 ? 12 : 8; nodes = namesOffset+4;
            long relative = nodes+(long)names*nodeSize; Fits(relative,4);
            blob = relative+4; blobBytes = Read((int)relative); Fits(blob+blobBytes,(long)files*4);
        }
        if (names > MaximumNames) throw new InvalidDataException("Archive exceeds250000 name nodes");
        Fits(nodes, (long)names*nodeSize); Fits(blob,blobBytes);
        bool supported = modern || type <= -5 || names == 0;
        if (supported)
        {
            var lengths = new int[(int)names+1]; long expanded = 0;
            for (int i = modern ? 0 : 1; i < names; ++i)
            {
                int n = (int)(nodes+(long)i*nodeSize);
                uint nameOffset = Read(n+(modern ? 0 : 4));
                int parent = modern ? BinaryPrimitives.ReadUInt16BigEndian(bytes.AsSpan(n+4,2)) :
                                      BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(n+8,2));
                if (modern && nameOffset == uint.MaxValue) continue;
                // Preserve the shared reader's classic invalid-node skip rule.
                if (!modern && (nameOffset >= blobBytes || parent > names)) continue;
                if (nameOffset >= blobBytes || parent > names) throw new InvalidDataException("Invalid name node");
                int start = (int)(blob+nameOffset);
                int available = (int)Math.Min(1025, blobBytes-nameOffset);
                int length = bytes.AsSpan(start,available).IndexOf((byte)0);
                if (length < 0) throw new InvalidDataException("Archive name exceeds1024B or is unterminated");
                int full = lengths[parent]+(lengths[parent] == 0 ? 0 : 1)+length;
                if (full > 4096 || (expanded += full) > 16*1024*1024)
                    throw new InvalidDataException("Expanded archive name work exceeds bound");
                int fileId = modern ? BinaryPrimitives.ReadUInt16BigEndian(bytes.AsSpan(n+(nodeSize==12 ? 10 : 8),2)) :
                                      BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(n+10,2));
                bool folder = modern ? fileId == 0 && i != names-1 : fileId == 0 || fileId >= files;
                if (folder) lengths[i] = full;
            }
        }
        return (Sha(bytes), supported);
    }

    private sealed class EntryScan(string pending, Budget budget, byte[] buffer)
    {
        private int carry;
        private long baseOffset;
        internal readonly List<object> Records = new();
        internal readonly Dictionary<string,int> Files = new(StringComparer.Ordinal);
        internal int Rejected;
        internal string DecodedSha = "";
        private long pendingBytes;
        private readonly IncrementalHash hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        internal void Feed(ReadOnlySpan<byte> input, bool eof)
        {
            hash.AppendData(input); input.CopyTo(buffer.AsSpan(carry));
            int length = carry+input.Length, position = 0;
            while (position+ImageShaderExtractor.HeaderBytes <= length)
            {
                budget.Check();
                int magic = buffer.AsSpan(position,length-position).IndexOf(Magic);
                if (magic < 0) { position = Math.Max(position,length-(ImageShaderExtractor.HeaderBytes-1)); break; }
                position += magic;
                var result = ImageShaderExtractor.Parse(buffer.AsSpan(position,length-position), eof, out var layout);
                if (result == ImageShaderExtractor.Result.Incomplete) break;
                if (result == ImageShaderExtractor.Result.Invalid) { ++Rejected; ++position; continue; }
                budget.Candidate(layout.Total);
                if (Records.Count >= 4096) throw new WorkLimit("Entry exceeds4096 candidate starts");
                var full = buffer.AsSpan(position,layout.Total);
                string sha = Sha(full), filename = layout.Stage+"-"+sha+".bin";
                if (Files.TryAdd(filename,layout.Total))
                {
                    pendingBytes += layout.Total;
                    if (pendingBytes > MaximumOutput-budget.OutputBytes)
                        throw new WorkLimit("Provisional entry output exceeds512MiB remaining bound");
                    using var file = new FileStream(Path.Combine(pending,filename),FileMode.CreateNew,FileAccess.Write);
                    file.Write(full);
                }
                Records.Add(new { entry_offset = baseOffset+position, stage = layout.Stage, flags = layout.Flags.ToString("X8"),
                    file = filename, bytes = layout.Total, archive_marker = layout.Marker, container_sha256 = sha,
                    physical = new { offset = layout.PhysicalBase, bytes = layout.PhysicalBytes,
                        sha256 = Sha(full.Slice(layout.PhysicalBase,layout.PhysicalBytes)) },
                    instruction = new { offset = layout.InstructionOffset, bytes = layout.InstructionBytes,
                        sha256 = Sha(full.Slice(layout.InstructionOffset,layout.InstructionBytes)) } });
                ++position; // All starts, including nested/overlapping containers.
            }
            carry = length-position;
            Buffer.BlockCopy(buffer,position,buffer,0,carry); baseOffset += position;
            if (eof) { DecodedSha = Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant(); hash.Dispose(); }
        }
        internal void Dispose() => hash.Dispose();
    }

    private static long ReadEntry(FileStream source, (long offset,uint zsize,uint size) entry, EntryScan scan, Budget budget,
                                  byte[] input, byte[] output, DfltContext decoder)
    {
        var (offset,zsize,size) = entry;
        if (offset < 0 || offset > source.Length || zsize > source.Length-offset)
            throw new InvalidDataException("Sparse/truncated entry range outside DAT");
        if (size > MaximumEntry || zsize > MaximumEntry) throw new InvalidDataException("Entry exceeds1GiB bounded scan");
        source.Position = offset;
        long remaining = zsize, produced = 0;
        byte[] header = new byte[12];
        bool packed = false;
        if (remaining >= 12) { source.ReadExactly(header); source.Position = offset; packed = header.AsSpan(0,4).SequenceEqual("DFLT"u8); }
        while (remaining > 0)
        {
            budget.Check();
            if (!packed)
            {
                int count = (int)Math.Min(Chunk,remaining); source.ReadExactly(input.AsSpan(0,count));
                budget.Decode(count); scan.Feed(input.AsSpan(0,count),false); remaining -= count; produced += count;
            }
            else
            {
                if (remaining < 12) throw new InvalidDataException("Truncated DFLT chunk header");
                source.ReadExactly(header); remaining -= 12;
                int c = BinaryPrimitives.ReadInt32LittleEndian(header.AsSpan(4,4));
                int u = BinaryPrimitives.ReadInt32LittleEndian(header.AsSpan(8,4));
                if (!header.AsSpan(0,4).SequenceEqual("DFLT"u8) || c < 0 || u < 0 || c > Chunk || u > Chunk ||
                    c > remaining || u > size-produced) throw new InvalidDataException("Invalid or oversized DFLT chunk");
                source.ReadExactly(input.AsSpan(0,c)); remaining -= c; budget.Decode(u);
                if (c == u) scan.Feed(input.AsSpan(0,u),false);
                else
                {
                    decoder.Reset();
                    // The actual stored-block decoder uses Input.Length for a
                    // copy bound, not InputLength. An exact bounded copy keeps
                    // previous compressed chunks outside its readable input.
                    decoder.Input = input.AsSpan(0,c).ToArray(); decoder.InputLength = c;
                    // WriteByte saturates at OutputLength. One sentinel byte
                    // makes even a one-byte overlong stream fail the count.
                    decoder.Output = output; decoder.OutputLength = u+1;
                    if (Dflt.DecompressChunk(decoder) != 1 || decoder.OutputOffset != u || decoder.InputOffset > c)
                        throw new InvalidDataException("DFLT decoder rejected chunk or produced wrong byte count");
                    scan.Feed(output.AsSpan(0,u),false);
                }
                produced += u;
            }
        }
        if (produced != size) throw new InvalidDataException("Decoded entry size differs from indexed size");
        scan.Feed(ReadOnlySpan<byte>.Empty,true);
        return produced;
    }

    internal static int Extract(string inputRoot, string outputRoot, int seconds)
    {
        string[] archives = Directory.Exists(inputRoot) ? Directory.EnumerateFiles(inputRoot,"*",SearchOption.TopDirectoryOnly)
            .Where(IsArchive).OrderBy(p=>p,StringComparer.Ordinal).Take(4097).ToArray() :
            File.Exists(inputRoot) && IsArchive(inputRoot) ? new[]{inputRoot} : Array.Empty<string>();
        if (archives.Length == 0 || archives.Length > 4096) throw new ArgumentException("Expected1..4096 DAT/DAT2 archives");
        if (File.Exists(outputRoot) || Directory.Exists(outputRoot)) throw new IOException("Use a fresh output directory");
        string parent = Path.GetDirectoryName(outputRoot)!; Directory.CreateDirectory(parent);
        string temporary = Path.Combine(parent,"."+Path.GetFileName(outputRoot)+".embedded-"+Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temporary);
        var budget = new Budget(seconds); var summaries = new List<object>();
        var unique = new HashSet<string>(StringComparer.Ordinal); int published = 0; bool complete = true, halted = false;
        try
        {
            using (var ledger = new StreamWriter(Path.Combine(temporary,"entries.jsonl")))
            using (var records = new StreamWriter(Path.Combine(temporary,"containers.jsonl")))
            {
                var input = new byte[Chunk]; var unpacked = new byte[Chunk+1]; var decoder = new DfltContext();
                var scanBuffer = new byte[Chunk+2*ImageShaderExtractor.MaximumSectionBytes+4];
                foreach (string path in archives)
                {
                    int files = -1, visited = 0, failed = 0, unnamed = 0; string? failure = null, headerSha = null;
                    bool namesSupported = false; long length = new FileInfo(path).Length; DateTime modified = File.GetLastWriteTimeUtc(path);
                    string headerPath = DatArchive.HdrPathFor(path); long? headerLength = File.Exists(headerPath) ? new FileInfo(headerPath).Length : null;
                    DateTime? headerModified = headerLength is null ? null : File.GetLastWriteTimeUtc(headerPath);
                    if (halted) failure = "Unvisited after global work/deadline limit";
                    else try
                    {
                        budget.Check(); (headerSha,namesSupported) = Preflight(path);
                        var archive = new DatArchive(path); files = archive.FileCount;
                        var names = new Dictionary<int,List<string>>();
                        foreach (var name in archive.EnumerateNames())
                        {
                            budget.Check(); int index = archive.FindEntry(name.Key);
                            if (index < 0 || index >= files) throw new InvalidDataException("Name cannot resolve to an indexed entry");
                            if (!names.TryGetValue(index,out var list)) names[index] = list = new List<string>();
                            list.Add(name.Key);
                        }
                        if (!namesSupported) complete = false;
                        using var source = new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.Read);
                        for (int i = 0; i < files; ++i)
                        {
                            budget.Check(); names.TryGetValue(i,out var foundNames);
                            string[] aliases = foundNames?.OrderBy(n=>n,StringComparer.Ordinal).ToArray() ?? Array.Empty<string>();
                            if (aliases.Length == 0) ++unnamed;
                            (long offset,uint zsize,uint size) entry = default;
                            string pending = Path.Combine(temporary,"pending"); Directory.CreateDirectory(pending);
                            var scan = new EntryScan(pending,budget,scanBuffer);
                            try
                            {
                                entry = archive.GetEntry(i);
                                long decoded = ReadEntry(source,entry,scan,budget,input,unpacked,decoder);
                                // All chunks and the indexed size are validated before ANY entry candidate is published.
                                long additional = scan.Files.Where(p=>!unique.Contains(p.Key)).Sum(p=>(long)p.Value);
                                if (additional > MaximumOutput-budget.OutputBytes) throw new WorkLimit("Unique output exceeds512MiB");
                                foreach (var file in scan.Files)
                                    if (unique.Add(file.Key)) File.Move(Path.Combine(pending,file.Key),Path.Combine(temporary,file.Key));
                                budget.OutputBytes += additional;
                                foreach (object candidate in scan.Records)
                                    Line(records,new { archive = path, entry_index = i, names = aliases, candidate });
                                published += scan.Records.Count;
                                Line(ledger,new { archive = path, entry_index = i, names = aliases, offset = entry.offset,
                                    stored_bytes = entry.zsize, indexed_bytes = entry.size, status = "decoded-scanned", decoded_bytes = decoded,
                                    decoded_sha256 = scan.DecodedSha, structural_candidates = scan.Records.Count, rejected_magic_candidates = scan.Rejected });
                            }
                            catch (Exception error) when (error is IOException or InvalidDataException or ArgumentException or IndexOutOfRangeException)
                            {
                                ++failed; complete = false;
                                Line(ledger,new { archive = path, entry_index = i, names = aliases, offset = entry.offset,
                                    stored_bytes = entry.zsize, indexed_bytes = entry.size, status = "failed-no-candidates-published",
                                    failure = error.Message, withheld_candidates = scan.Records.Count });
                                if (error is WorkLimit) { halted = true; failure = error.Message; }
                            }
                            finally { scan.Dispose(); Directory.Delete(pending,true); }
                            ++visited; if (halted) break;
                        }
                        if (new FileInfo(path).Length != length || File.GetLastWriteTimeUtc(path) != modified ||
                            headerLength is not null && (new FileInfo(headerPath).Length != headerLength || File.GetLastWriteTimeUtc(headerPath) != headerModified))
                            throw new InvalidOperationException("Archive/header changed during scan; no output published");
                    }
                    catch (Exception error) when (error is IOException or InvalidDataException or ArgumentException or IndexOutOfRangeException or OverflowException)
                    {
                        complete = false; failure = error.Message; if (error is WorkLimit) halted = true;
                    }
                    if (failure is not null || visited != files) complete = false;
                    summaries.Add(new { archive = path, bytes = length, modified_utc = modified, header = headerLength is null ? "embedded" : headerPath,
                        header_sha256 = headerSha, header_bytes = headerLength, entries = files < 0 ? (int?)null : files, visited, failed,
                        header_sha256_basis = headerLength is null ? "shared-reader-header-with-four-zero-prefix" : "complete-external-header",
                        unnamed_visited = unnamed, names_inventory = namesSupported ? "FindEntry-CRC-first-with-tree-fallback" : "unavailable-or-unsupported",
                        unvisited = files < 0 ? new { first = (int?)null, count = (int?)null } : new { first = (int?)(visited < files ? visited : null), count = (int?)(files-visited) }, failure });
                }
            }
            File.WriteAllText(Path.Combine(temporary,"embedded-index.json"),JsonSerializer.Serialize(new {
                version = 1, status = complete ? "complete-structural-scan-not-compiled" : "incomplete-structural-scan-not-compiled",
                extractor_assembly = typeof(EmbeddedShaderExtractor).Assembly.Location,
                complete, archives = summaries, candidates = published, unique_containers = unique.Count,
                decoded_bytes_visited = budget.Decoded, candidate_bytes_visited = budget.CandidateBytes, output_bytes = budget.OutputBytes,
                entries_ledger = "entries.jsonl", containers_ledger = "containers.jsonl",
                bounds = new { seconds, chunk_bytes = Chunk, header_bytes = MaximumHeader, entry_bytes = MaximumEntry,
                    decoded_bytes = MaximumDecoded, candidate_work_bytes = MaximumCandidateWork, output_bytes = MaximumOutput, candidates = MaximumRecords },
                limitations = new[] { "Structural candidates are not instruction/compilation validation or campaign coverage.",
                    "All indexed entries are attempted regardless of names. Sparse, oversized, decoder-failed and unvisited entries are explicit.",
                    "DFLT chunk limits are deliberate; other compression is not decoded and no metadata is reconstructed.",
                    "Complete source containers retain reflection and optional physical marker; runtime reflection stripping is a separate transformation.",
                    "Entry decoded SHA256 plus bounded header SHA256 identify scanned source ranges. Whole archive SHA256 is not computed.",
                    "SHA256 identities are an offline origin index; use lego_gpu_microcode_index for renderer placement XXH3 identities." }
            },Json));
            Directory.Move(temporary,outputRoot);
            Console.WriteLine($"embedded structural candidates: {published}; unique: {unique.Count}; complete: {complete}");
            return !complete ? 3 : published == 0 ? 1 : 0;
        }
        catch { if (Directory.Exists(temporary)) Directory.Delete(temporary,true); throw; }
    }
}
