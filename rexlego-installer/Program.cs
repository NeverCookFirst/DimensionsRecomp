using System.Runtime.InteropServices;

namespace RecompSetup;

static class Program
{
    [STAThread]
    static int Main(string[] args)
    {
        using var payload = PayloadSource.Open();

        // The installed updater is this same binary with the payload cut off, so
        // "no payload at all" is what tells the two apart. A payload that is
        // present but incomplete is a damaged download, and still an error.
        if (payload.Entries.Count == 0 || args.Any(IsUpdaterArg)) return Updater.Run(args);

        var missing = payload.MissingRequired();
        if (missing.Count > 0)
        {
            string msg = "This installer is incomplete - it does not contain:\n\n  "
                + string.Join("\n  ", missing)
                + "\n\nDownload the installer again; if it came as a zip, extract it fully first.";
            if (args.Length > 0) { AttachConsole(-1); Console.Error.WriteLine(msg); return 2; }
            ApplicationConfiguration.Initialize();
            MessageBox.Show(msg, WizardForm.AppName + " Setup", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return 2;
        }

        if (args.Length > 0) return Unattended(args, payload);

        ApplicationConfiguration.Initialize();
        Application.Run(new WizardForm(payload));
        return 0;
    }

    /// <summary>
    /// Arguments that mean "act as the updater", whatever this binary carries.
    /// Deliberately NOT "--update": that is also the unattended install's
    /// update-data directory, and matching it here made a command-line install
    /// impossible. The installed updater has no payload, so it takes the
    /// updater route on its own.
    /// </summary>
    static bool IsUpdaterArg(string arg) => arg is "--check" or "--apply" or "--quiet";

    /// <summary>
    /// Headless install, used for automated testing and by people who would
    /// rather script it:
    ///   Setup.exe --game DIR --update DIR|FILE --install DIR [--dlc DIR]
    ///             [--no-toypad] [--no-mods] [--russian] [--save-converter] [--no-updater] [--no-shortcut]
    /// Runs the exact same InstallJob as the wizard, prints progress to the
    /// console it was started from.
    /// </summary>
    static int Unattended(string[] args, PayloadSource payload)
    {
        AttachConsole(-1);
        var o = new InstallOptions();
        for (int i = 0; i < args.Length; i++)
        {
            string? Next() => i + 1 < args.Length ? args[++i] : null;
            switch (args[i])
            {
                case "--game": o.GameDir = Next() ?? ""; break;
                case "--update": o.UpdatePath = Next() ?? ""; break;
                case "--dlc": o.DlcPath = Next(); break;
                case "--install": o.InstallDir = Next() ?? ""; break;
                case "--no-toypad": o.IncludeToypad = false; break;
                case "--no-mods": o.IncludeMods = false; break;
                case "--russian": o.IncludeRussian = true; break;
                case "--save-converter": o.IncludeSaveConverter = true; break;
                case "--no-updater": o.IncludeUpdater = false; break;
                case "--no-shortcut": o.DesktopShortcut = false; break;
                default:
                    Console.Error.WriteLine("usage: Setup.exe --game DIR --update DIR|FILE --install DIR [--dlc DIR] [--no-toypad] [--no-mods] [--russian] [--save-converter] [--no-updater] [--no-shortcut]");
                    return 2;
            }
        }

        string? err = Validation.CheckGameDir(o.GameDir, out var info)
                      ?? Validation.CheckUpdate(o.UpdatePath, out _)
                      ?? (o.InstallDir == "" ? "--install is required" : null)
                      ?? (o.IncludeMods && !payload.HasMods ? "payload has no mods/modcli - pass --no-mods" : null)
                      ?? (o.IncludeToypad && !payload.HasToypad ? "payload has no toypad app - pass --no-toypad" : null)
                      ?? (o.IncludeRussian && !payload.HasRussian ? "payload has no Russian translation" : null)
                      ?? (o.IncludeRussian && !o.IncludeMods ? "--russian needs the mods component" : null)
                      ?? (o.IncludeSaveConverter && !payload.HasSaveConverter ? "payload has no save converter" : null);
        if (err is not null) { Console.Error.WriteLine("error: " + err); return 1; }
        o.InstallDir = Path.GetFullPath(o.InstallDir);
        Console.WriteLine($"game: {info!.TitleId:X8} media {info.MediaId:X8} v{info.VersionString}");

        var dlc = Validation.ScanDlc(o.DlcPath);
        foreach (var b in Validation.ScanDlc(Path.Combine(o.GameDir, "5752084B", "00000002")))
            if (!dlc.Any(d => d.Name.Equals(b.Name, StringComparison.OrdinalIgnoreCase))) dlc.Add(b);
        Console.WriteLine($"dlc: {dlc.Count} package(s)");
        foreach (var d in dlc) Console.WriteLine($"   {d.DisplayName}  [{(d.IsPackageFile ? "package" : "folder")}]");

        string last = "";
        // Progress<T> would post to a sync context and there is none here, so
        // the callback runs inline on the worker thread instead.
        var job = new InstallJob(o, payload, new InlineProgress<InstallProgress>(p =>
        {
            if (p.Status == last) return;
            last = p.Status;
            Console.WriteLine($"[{p.Fraction * 100,5:0.0}%] {p.Status}");
        }), CancellationToken.None);
        try
        {
            job.RunAsync(dlc).GetAwaiter().GetResult();
            Console.WriteLine("done: " + job.ReadmePath);
            return 0;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine("error: " + e.Message);
            return 1;
        }
    }

    sealed class InlineProgress<T> : IProgress<T>
    {
        readonly Action<T> handler;
        public InlineProgress(Action<T> handler) => this.handler = handler;
        public void Report(T value) => handler(value);
    }

    [DllImport("kernel32.dll")]
    static extern bool AttachConsole(int pid);
}
