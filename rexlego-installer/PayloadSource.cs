using System.IO.Compression;
using System.Text;

namespace RecompSetup;

/// <summary>One file waiting to be installed. Path is relative, '/'-separated.</summary>
public sealed record PayloadEntry(string Path, long Length);

/// <summary>
/// Where the files this installer lays down come from. Two shapes are
/// supported, checked in this order:
///
///   1. a ZIP appended to our own .exe, ending with the 16-byte footer
///      "RXPAYLD1" + int64 length - what testers get, a single file;
///   2. a "payload" folder next to the .exe - the development layout, and what
///      build-installer.ps1 assembles before appending it.
///
/// Appending after a single-file apphost is safe: the bundle header offset is
/// patched into the host at publish time, not searched for from the end of the
/// file (verified by running an exe with junk appended).
/// </summary>
public abstract class PayloadSource : IDisposable
{
    public const string FooterMagic = "RXPAYLD1";
    public const int FooterLength = 16;

    public abstract IReadOnlyList<PayloadEntry> Entries { get; }
    public abstract void CopyTo(PayloadEntry entry, string destPath, Action<long>? progress);
    public virtual void Dispose() { }

    public static PayloadSource Open()
    {
        string exe = Environment.ProcessPath ?? Path.Combine(AppContext.BaseDirectory, "Setup.exe");
        var appended = TryOpenAppended(exe);
        if (appended is not null) return appended;
        return new FolderPayload(Path.Combine(AppContext.BaseDirectory, "payload"));
    }

    /// <summary>A .rxp update pack, or any zip in the same layout.</summary>
    public static PayloadSource OpenFile(string zipPath)
    {
        var file = new FileStream(zipPath, FileMode.Open, FileAccess.Read, FileShare.Read);
        return new ZipPayload(file, 0, file.Length);
    }

    /// <summary>
    /// Writes this executable with the payload cut back off, which is exactly
    /// the binary dotnet publish produced. That copy is what gets installed as
    /// the updater: same code, no 175 MB of payload behind it, and nothing extra
    /// for anyone to download.
    /// </summary>
    public static bool TryWriteHostWithoutPayload(string destPath)
    {
        string exe = Environment.ProcessPath ?? "";
        if (!File.Exists(exe)) return false;
        using var input = new FileStream(exe, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        if (input.Length < FooterLength + 22) return false;
        input.Position = input.Length - FooterLength;
        byte[] footer = new byte[FooterLength];
        input.ReadExactly(footer);
        if (Encoding.ASCII.GetString(footer, 0, 8) != FooterMagic) return false;
        long hostLength = input.Length - FooterLength - BitConverter.ToInt64(footer, 8);
        if (hostLength <= 0) return false;

        Directory.CreateDirectory(Path.GetDirectoryName(destPath)!);
        input.Position = 0;
        using var output = new FileStream(destPath, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20);
        byte[] buf = new byte[1 << 20];
        long left = hostLength;
        while (left > 0)
        {
            int n = input.Read(buf, 0, (int)Math.Min(buf.Length, left));
            if (n <= 0) return false;
            output.Write(buf, 0, n);
            left -= n;
        }
        return true;
    }

    static PayloadSource? TryOpenAppended(string exePath)
    {
        FileStream? file = null;
        try
        {
            file = new FileStream(exePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            if (file.Length < FooterLength + 22) { file.Dispose(); return null; }
            file.Position = file.Length - FooterLength;
            byte[] footer = new byte[FooterLength];
            file.ReadExactly(footer);
            if (Encoding.ASCII.GetString(footer, 0, 8) != FooterMagic) { file.Dispose(); return null; }
            long length = BitConverter.ToInt64(footer, 8);
            long offset = file.Length - FooterLength - length;
            if (length <= 0 || offset <= 0) { file.Dispose(); return null; }
            return new ZipPayload(file, offset, length);
        }
        catch
        {
            file?.Dispose();
            return null;
        }
    }

    // ---- what the rest of the installer asks about the payload --------------

    public const string GamepadDb = "gamecontrollerdb.txt";   // optional

    public static readonly string[] GameFiles =
        { "legodimensions.exe", "rexruntime.dll", "rexgpu-xenos.dll", "FiraSans-Regular.ttf", "achievement_unlocked.wav" };

    public IEnumerable<PayloadEntry> Under(string prefix) =>
        Entries.Where(e => e.Path.StartsWith(prefix + "/", StringComparison.OrdinalIgnoreCase));

    public long SizeUnder(string prefix) => Under(prefix).Sum(e => e.Length);

    public PayloadEntry? Find(string path) =>
        Entries.FirstOrDefault(e => e.Path.Equals(path, StringComparison.OrdinalIgnoreCase));

    /// <summary>The files without which there is nothing to install.</summary>
    public List<string> MissingRequired() =>
        GameFiles.Where(f => Find("game/" + f) is null).Select(f => "game/" + f).ToList();

    public bool HasMods => Under("mods").Any() && Find("modcli/modcli.exe") is not null;
    public bool HasUpdater => Find("updater/rexupdate.exe") is not null;
    public bool HasToypad => Under("toypad").Any(e => e.Path.EndsWith(".exe", StringComparison.OrdinalIgnoreCase));
    public bool HasSaveConverter => Under("saveconverter").Any(e => e.Path.EndsWith(".exe", StringComparison.OrdinalIgnoreCase));

    protected static void Pump(Stream input, string destPath, Action<long>? progress)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(destPath)!);
        using var output = new FileStream(destPath, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20);
        byte[] buf = new byte[1 << 20];
        int n;
        while ((n = input.Read(buf, 0, buf.Length)) > 0)
        {
            output.Write(buf, 0, n);
            progress?.Invoke(n);
        }
    }
}

sealed class FolderPayload : PayloadSource
{
    readonly string root;
    readonly List<PayloadEntry> entries = new();

    public FolderPayload(string root)
    {
        this.root = root;
        if (!Directory.Exists(root)) return;
        foreach (var file in Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories))
            entries.Add(new PayloadEntry(Path.GetRelativePath(root, file).Replace('\\', '/'), new FileInfo(file).Length));
    }

    public override IReadOnlyList<PayloadEntry> Entries => entries;

    public override void CopyTo(PayloadEntry entry, string destPath, Action<long>? progress)
    {
        string src = Path.Combine(root, entry.Path.Replace('/', Path.DirectorySeparatorChar));
        using var input = new FileStream(src, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20, FileOptions.SequentialScan);
        Pump(input, destPath, progress);
    }
}

sealed class ZipPayload : PayloadSource
{
    readonly FileStream file;
    readonly ZipArchive zip;
    readonly List<PayloadEntry> entries = new();
    // Windows PowerShell's Compress-Archive writes entry names with backslashes,
    // .NET's ZipFile writes forward slashes. Normalise for our own paths and
    // keep this map so the lookup works whichever tool built the archive.
    readonly Dictionary<string, ZipArchiveEntry> byPath = new(StringComparer.OrdinalIgnoreCase);

    public ZipPayload(FileStream file, long offset, long length)
    {
        this.file = file;
        zip = new ZipArchive(new SubStream(file, offset, length), ZipArchiveMode.Read);
        foreach (var e in zip.Entries)
        {
            string path = e.FullName.Replace('\\', '/');
            if (path.EndsWith('/')) continue;          // directory marker
            entries.Add(new PayloadEntry(path, e.Length));
            byPath[path] = e;
        }
    }

    public override IReadOnlyList<PayloadEntry> Entries => entries;

    public override void CopyTo(PayloadEntry entry, string destPath, Action<long>? progress)
    {
        if (!byPath.TryGetValue(entry.Path, out var e))
            throw new FileNotFoundException("Not in the payload: " + entry.Path);
        using var input = e.Open();
        Pump(input, destPath, progress);
    }

    public override void Dispose() { zip.Dispose(); file.Dispose(); }
}

/// <summary>
/// A read-only, seekable window onto part of a file. ZipArchive seeks to the
/// end of its stream to find the central directory, so a pre-positioned
/// FileStream will not do.
/// </summary>
sealed class SubStream : Stream
{
    readonly Stream inner;
    readonly long start, length;
    long position;

    public SubStream(Stream inner, long start, long length)
    {
        this.inner = inner; this.start = start; this.length = length;
    }

    public override bool CanRead => true;
    public override bool CanSeek => true;
    public override bool CanWrite => false;
    public override long Length => length;

    public override long Position
    {
        get => position;
        set => position = Math.Clamp(value, 0, length);
    }

    public override int Read(byte[] buffer, int offset, int count)
    {
        if (position >= length) return 0;
        inner.Position = start + position;
        int n = inner.Read(buffer, offset, (int)Math.Min(count, length - position));
        position += n;
        return n;
    }

    public override long Seek(long offset, SeekOrigin origin) => Position = origin switch
    {
        SeekOrigin.Begin => offset,
        SeekOrigin.Current => position + offset,
        _ => length + offset,
    };

    public override void Flush() { }
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
}
