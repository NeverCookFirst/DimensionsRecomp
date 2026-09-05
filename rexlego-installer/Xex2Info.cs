using System.Buffers.Binary;

namespace RecompSetup;

/// <summary>
/// The identity of an Xbox 360 executable (.xex or .xexp patch): the
/// XEX2 optional header 0x00040006 "execution info". This is what decides
/// whether the recomp will run at all - the C++ was generated from one exact
/// build, so the box region is irrelevant and only these numbers count.
/// </summary>
public sealed record Xex2Info(uint MediaId, uint Version, uint BaseVersion, uint TitleId)
{
    const uint Magic = 0x58455832;          // "XEX2"
    const uint ExecutionInfoKey = 0x00040006;

    public string VersionString => FormatVersion(Version);
    public string BaseVersionString => FormatVersion(BaseVersion);

    public static string FormatVersion(uint v) =>
        $"{v >> 28}.{(v >> 24) & 0xF}.{(v >> 8) & 0xFFFF}.{v & 0xFF}";

    /// <summary>Reads the header of a .xex/.xexp. Returns null if it is not a XEX2.</summary>
    public static Xex2Info? Read(string path)
    {
        using var f = File.OpenRead(path);
        if (f.Length < 0x18) return null;
        Span<byte> head = stackalloc byte[0x18];
        f.ReadExactly(head);
        if (BinaryPrimitives.ReadUInt32BigEndian(head) != Magic) return null;

        uint headerSize = BinaryPrimitives.ReadUInt32BigEndian(head[0x08..]);
        uint count = BinaryPrimitives.ReadUInt32BigEndian(head[0x14..]);
        if (headerSize > f.Length || count > 0x1000) return null;

        byte[] header = new byte[headerSize];
        f.Position = 0;
        f.ReadExactly(header);

        for (int i = 0; i < count; i++)
        {
            int at = 0x18 + i * 8;
            if (at + 8 > header.Length) break;
            uint key = BinaryPrimitives.ReadUInt32BigEndian(header.AsSpan(at));
            uint value = BinaryPrimitives.ReadUInt32BigEndian(header.AsSpan(at + 4));
            if (key != ExecutionInfoKey) continue;
            if (value + 0x10 > header.Length) return null;
            var e = header.AsSpan((int)value);
            return new Xex2Info(
                BinaryPrimitives.ReadUInt32BigEndian(e),
                BinaryPrimitives.ReadUInt32BigEndian(e[4..]),
                BinaryPrimitives.ReadUInt32BigEndian(e[8..]),
                BinaryPrimitives.ReadUInt32BigEndian(e[12..]));
        }
        return null;
    }
}
