using System.Buffers.Binary;
using System.Globalization;
using System.Security.Cryptography;
using System.Text.Json;

namespace GpuShaderExtract;

// This reads an already decoded/mapped image, not XEX compressed/encrypted bytes.
// Bounds describe the actual ShaderContainer/Shader/CTAB layouts used by the
// recomp compiler. Passing them establishes structural candidates, not valid DXIL.
internal static class ImageShaderExtractor
{
    internal const int HeaderBytes = 0x24;
    internal const int MaximumSectionBytes = 4 * 1024 * 1024;
    private const int MaximumContainerBytes = 2 * MaximumSectionBytes + 4;
    private const int ChunkBytes = 2 * 1024 * 1024;
    private const long MaximumImageBytes = 512L * 1024 * 1024;
    private const int MaximumCandidates = 10000;
    private static readonly byte[] Magic = { 0x10, 0x2a, 0x11 };

    internal readonly record struct Layout(int Total, int PhysicalBase, int PhysicalBytes,
                                           int InstructionOffset, int InstructionBytes, string Stage,
                                           uint Flags, bool Marker);
    internal enum Result { Invalid, Incomplete, Valid }

    private static uint Be(ReadOnlySpan<byte> bytes, int offset) =>
        BinaryPrimitives.ReadUInt32BigEndian(bytes.Slice(offset, 4));

    private static bool Fits(ulong offset, ulong count, ulong end) =>
        offset <= end && count <= end - offset;

    private static bool ParseBase(string? text, out uint value)
    {
        value = 0;
        if (text is not null && text.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) text = text[2..];
        return text?.Length == 8 && uint.TryParse(text, NumberStyles.AllowHexSpecifier,
                                                CultureInfo.InvariantCulture, out value);
    }

    internal static Result Parse(ReadOnlySpan<byte> bytes, bool eof, out Layout layout)
    {
        layout = default;
        if (bytes.Length < HeaderBytes) return eof ? Result.Invalid : Result.Incomplete;
        uint flags = Be(bytes, 0), virtualBytes = Be(bytes, 4), physicalBytes = Be(bytes, 8);
        uint shader = Be(bytes, 24);
        bool pixel = (flags & 1) == 0;
        uint metadataBytes = pixel ? 32u : 36u;
        if ((flags & 0xffffff00) != 0x102a1100 || Be(bytes, 28) != 0 || Be(bytes, 32) != 0 ||
            virtualBytes < HeaderBytes || virtualBytes > MaximumSectionBytes ||
            physicalBytes < 8 || physicalBytes > MaximumSectionBytes ||
            shader < HeaderBytes || !Fits(shader, metadataBytes, virtualBytes)) return Result.Invalid;
        // Carry enough bytes to distinguish the optional archive marker. At EOF,
        // a marker-less container may end exactly at virtual + physical bytes.
        int minimum = checked((int)(virtualBytes + physicalBytes));
        if (bytes.Length < minimum + 4 && !eof) return Result.Incomplete;
        if (bytes.Length < minimum) return Result.Invalid;
        bool marker = Fits(virtualBytes, 4, (ulong)bytes.Length) && Be(bytes, (int)virtualBytes) == physicalBytes;
        int physicalBase = (int)virtualBytes + (marker ? 4 : 0);
        int total = physicalBase + (int)physicalBytes;
        if (bytes.Length < total) return eof ? Result.Invalid : Result.Incomplete;
        uint instruction = Be(bytes, (int)shader), instructionBytes = Be(bytes, (int)shader + 4);
        if (instructionBytes < 8 || instructionBytes % 4 != 0 ||
            !Fits(instruction, instructionBytes, physicalBytes)) return Result.Invalid;
        ReadOnlySpan<byte> virtualSection = bytes[..(int)virtualBytes];
        uint interpolators = (Be(bytes, (int)shader + 20) >> 5) & 31;
        ulong elements = pixel ? interpolators :
            (ulong)Be(bytes, (int)shader + 24) + Be(bytes, (int)shader + 28) + interpolators;
        if (!Fits((ulong)shader + metadataBytes, elements * 4, virtualBytes)) return Result.Invalid;
        uint constants = Be(bytes, 16), definitions = Be(bytes, 20);
        if (!ConstantsFit(virtualSection, constants) ||
            !DefinitionsFit(virtualSection, definitions, physicalBytes)) return Result.Invalid;
        layout = new Layout(total, physicalBase, (int)physicalBytes,
                            physicalBase + (int)instruction, (int)instructionBytes,
                            pixel ? "ps" : "vs", flags, marker);
        return Result.Valid;
    }

    private static bool ConstantsFit(ReadOnlySpan<byte> bytes, uint offset)
    {
        if (offset == 0) return true; // Reflection is genuinely optional.
        if (offset < HeaderBytes || !Fits(offset, 32, (ulong)bytes.Length)) return false;
        int table = (int)offset + 4; // ConstantInfo offsets are relative to ConstantTable.
        uint count = Be(bytes, table + 12), info = Be(bytes, table + 16);
        if (count > 4096 || !Fits((ulong)table + info, (ulong)count * 20, (ulong)bytes.Length)) return false;
        for (uint i = 0; i < count; ++i)
        {
            int entry = checked(table + (int)info + (int)i * 20);
            ulong name = (ulong)table + Be(bytes, entry);
            if (!Fits(name, 1, (ulong)bytes.Length)) return false;
            int available = Math.Min(1024, bytes.Length - (int)name);
            if (bytes.Slice((int)name, available).IndexOf((byte)0) < 0) return false;
        }
        return true;
    }

    private static bool DefinitionsFit(ReadOnlySpan<byte> bytes, uint offset, uint physicalBytes)
    {
        if (offset == 0) return true;
        if (offset < HeaderBytes || !Fits(offset, 24, (ulong)bytes.Length)) return false;
        int current = (int)offset + 20;
        while (Fits((ulong)current, 4, (ulong)bytes.Length) && Be(bytes, current) != 0)
        {
            if (!Fits((ulong)current, 8, (ulong)bytes.Length)) return false;
            uint count = BinaryPrimitives.ReadUInt16BigEndian(bytes.Slice(current + 2, 2));
            ulong values = ((ulong)count + 3) / 4 * 16;
            if (!Fits(Be(bytes, current + 4), values, physicalBytes)) return false;
            current += 8;
        }
        if (!Fits((ulong)current, 4, (ulong)bytes.Length)) return false;
        current += 4;
        while (Fits((ulong)current, 4, (ulong)bytes.Length) && Be(bytes, current) != 0)
        {
            uint count = BinaryPrimitives.ReadUInt16BigEndian(bytes.Slice(current + 2, 2));
            // The real compiler reads count values at +4 and advances 2+count words.
            ulong advance = 8UL + (ulong)count * 4;
            if (!Fits((ulong)current, advance, (ulong)bytes.Length)) return false;
            current += (int)advance;
        }
        return Fits((ulong)current, 4, (ulong)bytes.Length);
    }

    private static string Sha(ReadOnlySpan<byte> bytes) =>
        Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    internal static int Run(string[] args)
    {
        if (args.Length < 4)
        {
            Console.Error.WriteLine("Usage: gpu-shader-extract extract --image <mapped-image> <output-dir> --image-base <8hex> [--image-provenance <json>]");
            return 2;
        }
        try
        {
            string image = Path.GetFullPath(args[0]), output = Path.GetFullPath(args[1]);
            uint? imageBase = null;
            string? provenancePath = null;
            for (int i = 2; i < args.Length; ++i)
            {
                string option = args[i];
                if (++i >= args.Length) throw new ArgumentException($"Missing value for {option}");
                if (option == "--image-base" && imageBase is null && ParseBase(args[i], out uint value)) imageBase = value;
                else if (option == "--image-provenance" && provenancePath is null) provenancePath = Path.GetFullPath(args[i]);
                else throw new ArgumentException($"Unknown, repeated or invalid option: {option}");
            }
            if (imageBase is null) throw new ArgumentException("--image-base is required; image load address is never guessed");
            return Extract(image, output, imageBase.Value, provenancePath) == 0 ? 1 : 0;
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or
                                          ArgumentException or JsonException or InvalidDataException)
        {
            Console.Error.WriteLine($"Image extraction rejected: {exception.Message}");
            return 2;
        }
    }

    internal static int Extract(string image, string output, uint imageBase, string? provenancePath)
    {
        var before = new FileInfo(image);
        long imageBytes = before.Length;
        DateTime modified = before.LastWriteTimeUtc;
        if (imageBytes < HeaderBytes || imageBytes > MaximumImageBytes ||
            (ulong)imageBase + (ulong)imageBytes > 0x100000000UL)
            throw new InvalidDataException("Image is empty, exceeds512MiB, or escapes its32-bit guest range");
        if (Directory.Exists(output) || File.Exists(output)) throw new IOException("Use a fresh output directory");
        JsonElement? suppliedProvenance = null;
        if (provenancePath is not null)
        {
            if (new FileInfo(provenancePath).Length > 4*1024*1024) throw new InvalidDataException("Provenance exceeds4MiB");
            using var document = JsonDocument.Parse(File.ReadAllBytes(provenancePath));
            suppliedProvenance = document.RootElement.Clone();
        }
        string parent = Path.GetDirectoryName(output)!;
        Directory.CreateDirectory(parent);
        string temporary = Path.Combine(parent, "." + Path.GetFileName(output) + ".image-extract-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temporary);
        try
        {
            var buffer = new byte[ChunkBytes + MaximumContainerBytes];
            int carry = 0, rejected = 0, count = 0;
            long fileOffset = 0, candidateBytes = 0;
            var records = new List<object>();
            var unique = new HashSet<string>(StringComparer.Ordinal);
            using var imageHash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            using (var source = new FileStream(image, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                bool eof;
                do
                {
                    int read = source.Read(buffer, carry, Math.Min(ChunkBytes, buffer.Length-carry));
                    imageHash.AppendData(buffer.AsSpan(carry, read));
                    eof = read == 0;
                    int length = carry + read, position = 0;
                    while (position + HeaderBytes <= length)
                    {
                        int magic = buffer.AsSpan(position, length-position).IndexOf(Magic);
                        if (magic < 0) { position = Math.Max(position, length-(HeaderBytes-1)); break; }
                        position += magic;
                        Result status = Parse(buffer.AsSpan(position, length-position), eof, out Layout layout);
                        if (status == Result.Incomplete) break;
                        if (status == Result.Invalid) { ++position; ++rejected; continue; }
                        if (++count > MaximumCandidates) throw new InvalidDataException("Image exceeds10000 structural candidates");
                        candidateBytes += layout.Total;
                        if (candidateBytes > MaximumImageBytes)
                            throw new InvalidDataException("Overlapping candidate bytes exceed512MiB work/output bound");
                        var full = buffer.AsSpan(position, layout.Total);
                        string sha = Sha(full), filename = layout.Stage + "-" + sha + ".bin";
                        if (unique.Add(filename)) File.WriteAllBytes(Path.Combine(temporary, filename), full.ToArray());
                        var physical = full.Slice(layout.PhysicalBase, layout.PhysicalBytes);
                        var instruction = full.Slice(layout.InstructionOffset, layout.InstructionBytes);
                        records.Add(new { image_offset = fileOffset+position,
                            guest_address = ((ulong)imageBase+(ulong)fileOffset+(ulong)position).ToString("X8"),
                            stage = layout.Stage, flags = layout.Flags.ToString("X8"), bytes = layout.Total,
                            archive_marker = layout.Marker, file = filename, container_sha256 = sha,
                            container_prefix8 = Convert.ToHexString(full[..8]),
                            physical = new { offset = layout.PhysicalBase, bytes = layout.PhysicalBytes,
                                             sha256 = Sha(physical), prefix8 = Convert.ToHexString(physical[..8]) },
                            instruction = new { offset = layout.InstructionOffset, bytes = layout.InstructionBytes,
                                                sha256 = Sha(instruction), prefix8 = Convert.ToHexString(instruction[..8]) } });
                        // Visit every possible header start, including containers
                        // embedded in a structurally-valid enclosing candidate.
                        ++position;
                    }
                    carry = length-position;
                    Buffer.BlockCopy(buffer, position, buffer, 0, carry);
                    fileOffset += position;
                } while (!eof);
            }
            string imageSha = Convert.ToHexString(imageHash.GetHashAndReset()).ToLowerInvariant();
            before.Refresh();
            if (before.Length != imageBytes || before.LastWriteTimeUtc != modified)
                throw new IOException("Image changed during extraction");
            if (suppliedProvenance is JsonElement proof)
            {
                if (proof.ValueKind != JsonValueKind.Object ||
                    !proof.TryGetProperty("mapped_image", out var mapped) || mapped.ValueKind != JsonValueKind.Object ||
                    !mapped.TryGetProperty("sha256", out var sha) || sha.ValueKind != JsonValueKind.String || sha.GetString() != imageSha ||
                    !mapped.TryGetProperty("bytes", out var size) || size.ValueKind != JsonValueKind.Number ||
                    !size.TryGetInt64(out long recordedSize) || recordedSize != imageBytes ||
                    !mapped.TryGetProperty("base", out var address) || address.ValueKind != JsonValueKind.String ||
                    !ParseBase(address.GetString(), out uint recordedBase) || recordedBase != imageBase)
                    throw new InvalidDataException("Supplied mapped_image provenance differs from actual image/base");
            }
            var index = new { version = 1, status = "structural-candidates-not-compiled", image = new {
                    path = image, bytes = imageBytes, sha256 = imageSha, guest_base = imageBase.ToString("X8") },
                supplied_loader_provenance = suppliedProvenance,
                supplied_loader_provenance_identity_matched = suppliedProvenance is not null,
                candidates = count, candidate_bytes_visited = candidateBytes,
                unique_containers = unique.Count, rejected_magic_candidates = rejected,
                trailing_unexamined_bytes = carry, records,
                limits = new[] { "Input must already be faithfully decoded and patched. This tool does not decode XEX or authenticate a provenance author.",
                                "Structural bounds do not establish instruction semantics, successful compilation, or campaign shader coverage.",
                                "Complete container bytes and optional physical markers are preserved; metadata is never reconstructed.",
                                "Every possible header start is searched, including overlapping/nested containers; truncated trailing bytes cannot contain a complete header.",
                                "SHA256/prefix8 identities are an offline origin index. Generate the renderer placement XXH3 index with lego_gpu_microcode_index." } };
            File.WriteAllText(Path.Combine(temporary, "image-index.json"), JsonSerializer.Serialize(index, new JsonSerializerOptions { WriteIndented = true }));
            Directory.Move(temporary, output);
            Console.WriteLine($"image structural candidates: {count}; unique containers: {unique.Count}; sha256: {imageSha}");
            return count;
        }
        catch
        {
            if (Directory.Exists(temporary)) Directory.Delete(temporary, true);
            throw;
        }
    }
}
