using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

namespace RecompSetup;

/// <summary>
/// A plain-text record of what the installer and the updater saw and did, so a
/// bug report can carry more than the one line a message box shows. Lives in
/// %LOCALAPPDATA%\DimensionsRecompiled, which exists before any install folder
/// does; a finished install gets a copy beside the game. Logging never throws.
/// </summary>
public static class SetupLog
{
    static readonly object gate = new();
    static string? lastLine;

    public static string FilePath { get; private set; } = "";

    /// <summary>What to append to an error message shown to the user.</summary>
    public static string Hint => FilePath.Length == 0 ? ""
        : $"\n\nDetails are in {FilePath}\nPlease attach that file when you report the problem.";

    public static string Version =>
        Assembly.GetEntryAssembly()?.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion
            .Split('+')[0] ?? "?";

    /// <param name="name">"setup" or "updater" - each keeps its own file.</param>
    public static void Start(string name, string[] args)
    {
        try
        {
            string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                                      "DimensionsRecompiled");
            Directory.CreateDirectory(dir);
            FilePath = Path.Combine(dir, name + ".log");
            // One previous run is kept; beyond that the file only grows.
            if (File.Exists(FilePath) && new FileInfo(FilePath).Length > 1024 * 1024)
                File.Move(FilePath, FilePath + ".old", true);
        }
        catch { FilePath = ""; return; }
        Write("");
        Write($"==== {WizardForm.AppName} {name} {Version} ====");
        Write($"OS: {RuntimeInformation.OSDescription} ({RuntimeInformation.OSArchitecture})");
        Write($"exe: {Environment.ProcessPath}");
        if (args.Length > 0) Write("args: " + string.Join(' ', args));
    }

    public static void Write(string line)
    {
        if (FilePath.Length == 0) return;
        lock (gate)
        {
            try
            {
                File.AppendAllText(FilePath,
                    (line.Length == 0 ? "" : $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] {line}") + Environment.NewLine,
                    Encoding.UTF8);
            }
            catch { /* a log that can't be written is not worth failing over */ }
        }
    }

    /// <summary>Like <see cref="Write"/>, but drops a line equal to the previous one.</summary>
    public static void WriteOnce(string line)
    {
        lock (gate)
        {
            if (line == lastLine) return;
            lastLine = line;
        }
        Write(line);
    }

    public static void Error(string what, Exception e) => Write($"ERROR {what}: {e}");

    /// <summary>Leaves a copy in the install folder, where people look first.</summary>
    public static void CopyTo(string dir)
    {
        if (FilePath.Length == 0) return;
        lock (gate)
        {
            try { File.Copy(FilePath, Path.Combine(dir, Path.GetFileName(FilePath)), true); }
            catch { }
        }
    }
}
