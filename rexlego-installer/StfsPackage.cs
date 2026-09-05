using System.Buffers.Binary;
using System.Text;

namespace RecompSetup;

/// <summary>
/// Read-only STFS container reader (CON / LIVE / PIRS): the format Title
/// Updates and marketplace DLC arrive in. The recomp wants these as plain
/// folders, so the installer extracts them. This is a straight port of the
/// SDK's StfsContainerDevice (rexglue-sdk/src/filesystem/devices), including
/// the hash-table walk; the layout constants come from stfs_xbox.h.
/// </summary>
public sealed class StfsPackage : IDisposable
{
    public const uint ContentTypeMarketplace = 0x00000002;   // DLC
    public const uint ContentTypeInstaller = 0x000B0000;     // Title Update

    const int BlockSize = 0x1000;
    const uint EndOfChain = 0xFFFFFF;
    static readonly uint[] BlocksPerHashLevel = { 170, 28900, 4913000 };

    public sealed record Entry(string Path, bool IsDirectory, uint StartBlock, uint Length, uint AllocatedBlocks);

    public string Magic { get; }
    public uint ContentType { get; }
    public Xex2Info Execution { get; }
    public string DisplayName { get; }
    public string TitleName { get; }
    public IReadOnlyList<Entry> Entries => entries;

    readonly FileStream file;
    readonly uint headerSize;
    readonly bool readOnlyFormat;
    readonly bool rootActiveIndex;
    readonly uint totalBlockCount;
    readonly uint blocksPerHashTable;
    readonly uint[] blockStep = new uint[2];
    readonly Dictionary<long, byte[]> hashTables = new();
    readonly List<Entry> entries = new();

    /// <summary>True if the file starts with a CON/LIVE/PIRS magic.</summary>
    public static bool LooksLikePackage(string path)
    {
        try
        {
            using var f = File.OpenRead(path);
            if (f.Length < 0x971A) return false;
            Span<byte> m = stackalloc byte[4];
            f.ReadExactly(m);
            string s = Encoding.ASCII.GetString(m);
            return s is "CON " or "LIVE" or "PIRS";
        }
        catch { return false; }
    }

    public StfsPackage(string path)
    {
        file = File.OpenRead(path);
        byte[] h = new byte[0x971A];
        file.ReadExactly(h);

        Magic = Encoding.ASCII.GetString(h, 0, 4);
        if (Magic is not ("CON " or "LIVE" or "PIRS"))
            throw new InvalidDataException("Not an STFS package (bad magic).");

        headerSize = BE32(h, 0x340);
        ContentType = BE32(h, 0x344);
        Execution = new Xex2Info(BE32(h, 0x354), BE32(h, 0x358), BE32(h, 0x35C), BE32(h, 0x360));

        uint volumeType = BE32(h, 0x3A9);
        if (volumeType != 0)
            throw new InvalidDataException("SVOD packages (multi-file game discs) are not supported here.");

        // StfsVolumeDescriptor at 0x379
        if (h[0x379] != 0x24) throw new InvalidDataException("Bad STFS volume descriptor.");
        byte flags = h[0x37B];
        readOnlyFormat = (flags & 1) != 0;
        rootActiveIndex = (flags & 2) != 0;
        ushort fileTableBlockCount = BinaryPrimitives.ReadUInt16LittleEndian(h.AsSpan(0x37C));
        uint fileTableBlockNumber = LE24(h, 0x37E);
        totalBlockCount = BE32(h, 0x395);

        blocksPerHashTable = readOnlyFormat ? 1u : 2u;
        blockStep[0] = BlocksPerHashLevel[0] + blocksPerHashTable;
        blockStep[1] = BlocksPerHashLevel[1] + (BlocksPerHashLevel[0] + 1) * blocksPerHashTable;

        DisplayName = Utf16Be(h, 0x411, 128);
        TitleName = Utf16Be(h, 0x1691, 64);

        ReadDirectory(fileTableBlockNumber, fileTableBlockCount);
    }

    void ReadDirectory(uint tableBlock, int tableBlockCount)
    {
        var all = new List<string>();
        byte[] block = new byte[BlockSize];
        for (int n = 0; n < tableBlockCount; n++)
        {
            file.Position = BlockToOffset(tableBlock);
            file.ReadExactly(block);
            for (int m = 0; m < 0x40; m++)
            {
                var e = block.AsSpan(m * 0x40, 0x40);
                if (e[0] == 0) break;
                byte fl = e[0x28];
                int nameLen = fl & 0x3F;
                bool isDir = (fl & 0x80) != 0;
                string name = Encoding.ASCII.GetString(e[..Math.Min(nameLen, 40)]);
                ushort parent = BinaryPrimitives.ReadUInt16BigEndian(e[0x32..]);
                string path = parent == 0xFFFF ? name : all[parent] + "\\" + name;
                uint alloc = LE24(e, 0x2C);
                uint start = LE24(e, 0x2F);
                uint length = BinaryPrimitives.ReadUInt32BigEndian(e[0x34..]);
                all.Add(path);
                entries.Add(new Entry(path, isDir, start, length, alloc));
            }
            uint next = GetBlockHashNext(tableBlock);
            if (next == EndOfChain) break;
            tableBlock = next;
        }
    }

    /// <summary>Writes one file entry to disk, reporting bytes as they land.</summary>
    public void Extract(Entry entry, string destPath, Action<long>? progress = null)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(destPath)!);
        using var output = File.Create(destPath);
        byte[] buf = new byte[BlockSize];
        uint block = entry.StartBlock;
        long remaining = entry.Length;
        while (remaining > 0 && block != EndOfChain)
        {
            int n = (int)Math.Min(BlockSize, remaining);
            file.Position = BlockToOffset(block);
            file.ReadExactly(buf, 0, n);
            output.Write(buf, 0, n);
            remaining -= n;
            progress?.Invoke(n);
            block = GetBlockHashNext(block);
        }
        if (remaining > 0)
            throw new InvalidDataException($"{entry.Path}: block chain ended {remaining} bytes early (corrupt package?).");
    }

    long DataStart => (headerSize + 0xFFFu) & ~0xFFFu;

    long BlockToOffset(ulong blockIndex)
    {
        ulong base_ = BlocksPerHashLevel[0];
        ulong block = blockIndex;
        for (int i = 0; i < 3; i++)
        {
            block += ((blockIndex + base_) / base_) * blocksPerHashTable;
            if (blockIndex < base_) break;
            base_ *= BlocksPerHashLevel[0];
        }
        return DataStart + (long)(block << 12);
    }

    uint HashBlockNumber(uint blockIndex, int level)
    {
        if (level == 0)
        {
            if (blockIndex < BlocksPerHashLevel[0]) return 0;
            uint block = (blockIndex / BlocksPerHashLevel[0]) * blockStep[0];
            block += ((blockIndex / BlocksPerHashLevel[1]) + 1) * blocksPerHashTable;
            if (blockIndex < BlocksPerHashLevel[1]) return block;
            return block + blocksPerHashTable;
        }
        if (level == 1)
        {
            if (blockIndex < BlocksPerHashLevel[1]) return blockStep[0];
            return (blockIndex / BlocksPerHashLevel[1]) * blockStep[1] + blocksPerHashTable;
        }
        return blockStep[1];
    }

    long HashBlockOffset(uint blockIndex, int level) => DataStart + ((long)HashBlockNumber(blockIndex, level) << 12);

    static bool ActiveIndex(byte[] table, uint record) => (table[record * 0x18 + 0x14] & 0x40) != 0;

    /// <summary>The level-0 hash entry's "next block" for a data block.</summary>
    uint GetBlockHashNext(uint blockIndex)
    {
        long lv0 = HashBlockOffset(blockIndex, 0);
        if (!hashTables.ContainsKey(lv0))
        {
            long secondary = rootActiveIndex ? BlockSize : 0;
            if (readOnlyFormat)
            {
                // Single backing block per table, no secondary copies anywhere.
                secondary = 0;
            }
            else if (totalBlockCount > BlocksPerHashLevel[0])
            {
                long lv1 = HashBlockOffset(blockIndex, 1);
                if (!hashTables.ContainsKey(lv1))
                {
                    if (totalBlockCount > BlocksPerHashLevel[1])
                    {
                        long lv2 = HashBlockOffset(blockIndex, 2);
                        if (!hashTables.ContainsKey(lv2)) ReadTableAt(lv2, lv2 + secondary);
                        uint rec2 = (blockIndex / BlocksPerHashLevel[1]) % BlocksPerHashLevel[0];
                        secondary = ActiveIndex(hashTables[lv2], rec2) ? BlockSize : 0;
                    }
                    ReadTableAt(lv1, lv1 + secondary);
                }
                uint rec1 = (blockIndex / BlocksPerHashLevel[0]) % BlocksPerHashLevel[0];
                secondary = ActiveIndex(hashTables[lv1], rec1) ? BlockSize : 0;
            }
            ReadTableAt(lv0, lv0 + secondary);
        }
        uint rec = blockIndex % BlocksPerHashLevel[0];
        byte[] t = hashTables[lv0];
        int at = (int)(rec * 0x18 + 0x14);
        return BinaryPrimitives.ReadUInt32BigEndian(t.AsSpan(at)) & 0xFFFFFF;
    }

    // Tables are cached under their primary offset even when the secondary
    // copy was the one read - same as the SDK, which keys by the primary.
    void ReadTableAt(long key, long actualOffset)
    {
        byte[] t = new byte[BlockSize];
        file.Position = actualOffset;
        file.ReadExactly(t);
        hashTables[key] = t;
    }

    static uint BE32(ReadOnlySpan<byte> b, int at) => BinaryPrimitives.ReadUInt32BigEndian(b[at..]);
    static uint LE24(ReadOnlySpan<byte> b, int at) => (uint)(b[at] | (b[at + 1] << 8) | (b[at + 2] << 16));

    static string Utf16Be(byte[] b, int at, int chars)
    {
        var sb = new StringBuilder();
        for (int i = 0; i < chars; i++)
        {
            char c = (char)BinaryPrimitives.ReadUInt16BigEndian(b.AsSpan(at + i * 2));
            if (c == 0) break;
            sb.Append(c);
        }
        return sb.ToString();
    }

    public void Dispose() => file.Dispose();
}
