using System.Buffers.Binary;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace RecompSetup;

/// <summary>What the recomp was generated from. Anything else will not run.</summary>
public static class KnownGame
{
    public const uint TitleId = 0x5752084B;
    public const uint MediaId = 0x72B1DD2A;
    // SHA-256 of Title Update 23's Default.xexp. The .xexp carries no execution
    // info of its own, so the hash is the identity check.
    public const string UpdateXexpSha256 = "a5169f508036d024312537d3ce64c07bf0680a87d9e0a282ff80461cf4a646a9";
    public const long UpdateXexpSize = 11114496;
    // TU24 exists and is what most "latest title update" links now hand out. Its
    // container has the SAME file name as TU23 (tu00000003_00000000), so people
    // cannot tell them apart - hence naming it in the error instead of just
    // saying no. The recomp is generated from the TU23 image and cannot use it.
    public const string Update24XexpSha256 = "272f465c020017917168d6fc4a0cf5b7a6e71c591ff77d7af207561913719a34";
    public const long Tu23PackageSize = 2069790720;
    public const long Tu24PackageSize = 2127081472;
    public static readonly string[] UpdateRequiredFiles = { "Default.xexp", "PATCH.DAT", "PATCH.HDR" };
    public static readonly string[] GameRequiredFiles = { "Default.xex", "GAME.DAT", "GAME.HDR" };
}

public sealed class InstallOptions
{
    public string GameDir = "";
    public string UpdatePath = "";          // folder or STFS file
    public string? DlcPath;                 // folder holding packages (folders and/or STFS files)
    public string InstallDir = "";
    public bool IncludeToypad = true;
    public bool IncludeMods = true;
    public bool IncludeRussian;
    public bool IncludeSaveConverter;
    public bool IncludeUpdater = true;
    public bool DesktopShortcut = true;
}

/// <summary>Facts about the release itself, as opposed to the game it needs.</summary>
public static class Product
{
    /// <summary>
    /// Where the updater looks for new releases. Written into legodimensions.toml
    /// as updates_repo, so an install can be pointed elsewhere without a new
    /// build, and into install.json so the updater still works if the toml is lost.
    /// </summary>
    public const string UpdateRepo = "NeverCookFirst/DimensionsRecomp";
}

public readonly record struct InstallProgress(double Fraction, string Status);

/// <summary>One DLC package waiting to be laid down, whichever form it came in.</summary>
public sealed record DlcSource(string Name, string Path, bool IsPackageFile, string DisplayName, long Bytes);

public static class Validation
{
    public static string? CheckGameDir(string dir, out Xex2Info? info)
    {
        info = null;
        if (string.IsNullOrWhiteSpace(dir) || !Directory.Exists(dir)) return "Select the folder that contains Default.xex.";
        foreach (var f in KnownGame.GameRequiredFiles)
            if (!File.Exists(Path.Combine(dir, f))) return $"{f} not found in this folder. It must be the extracted disc, not an ISO.";
        try { info = Xex2Info.Read(Path.Combine(dir, "Default.xex")); }
        catch (Exception e) { return "Could not read Default.xex: " + e.Message; }
        if (info is null) return "Default.xex is not a valid XEX2 executable.";
        if (info.TitleId != KnownGame.TitleId) return $"Wrong game: title ID {info.TitleId:X8} (expected {KnownGame.TitleId:X8}).";
        if (info.MediaId != KnownGame.MediaId)
            return $"This disc build (media ID {info.MediaId:X8}) is not the one the recomp was made from ({KnownGame.MediaId:X8}). It will not run.";
        return null;
    }

    /// <summary>Folder or STFS file. Returns an error string, or null when valid.</summary>
    public static string? CheckUpdate(string path, out string description)
    {
        description = "";
        if (string.IsNullOrWhiteSpace(path)) return "Select the Title Update 23 folder or package file.";
        if (Directory.Exists(path))
        {
            foreach (var f in KnownGame.UpdateRequiredFiles)
                if (!File.Exists(Path.Combine(path, f))) return $"{f} not found. The folder must contain the extracted Title Update.";
            string xexp = Path.Combine(path, "Default.xexp");
            string sha = Sha256File(xexp);
            if (sha != KnownGame.UpdateXexpSha256) return DescribeWrongUpdate(sha, new FileInfo(xexp).Length);
            description = "Extracted Title Update 23 folder";
            return null;
        }
        if (File.Exists(path))
        {
            if (!StfsPackage.LooksLikePackage(path)) return "This file is not an Xbox 360 package (CON/LIVE/PIRS).";
            try
            {
                using var p = new StfsPackage(path);
                if (p.Execution.TitleId != KnownGame.TitleId) return $"Package belongs to title {p.Execution.TitleId:X8}, not LEGO Dimensions.";
                if (p.ContentType != StfsPackage.ContentTypeInstaller) return $"Package is not a Title Update (content type {p.ContentType:X8}).";
                var xexp = p.Entries.FirstOrDefault(e => !e.IsDirectory && e.Path.Equals("Default.xexp", StringComparison.OrdinalIgnoreCase));
                if (xexp is null) return "Package has no Default.xexp.";
                string sha = Sha256Stfs(p, xexp);
                if (sha != KnownGame.UpdateXexpSha256) return DescribeWrongUpdate(sha, xexp.Length);
                description = $"Title Update package: {p.DisplayName}";
                return null;
            }
            catch (Exception e) { return "Could not read the package: " + e.Message; }
        }
        return "Path does not exist.";
    }

    /// <summary>
    /// Finds DLC packages in a folder: sub-folders that look like extracted
    /// marketplace content (spa.bin / DLCnn.DAT2) and files that are STFS
    /// marketplace packages for this title. Anything else is ignored.
    /// </summary>
    public static List<DlcSource> ScanDlc(string? dir)
    {
        var found = new List<DlcSource>();
        if (string.IsNullOrWhiteSpace(dir) || !Directory.Exists(dir)) return found;

        foreach (var sub in Directory.EnumerateDirectories(dir))
        {
            if (LooksLikeExtractedDlc(sub))
                found.Add(new DlcSource(Path.GetFileName(sub), sub, false, Path.GetFileName(sub), DirSize(sub)));
            else if (Path.GetFileName(sub).Equals("00000002", StringComparison.OrdinalIgnoreCase)
                     || Path.GetFileName(sub).Equals("5752084B", StringComparison.OrdinalIgnoreCase))
                found.AddRange(ScanDlc(sub));   // the user pointed at a content tree; walk down into it
        }
        foreach (var file in Directory.EnumerateFiles(dir))
        {
            if (!StfsPackage.LooksLikePackage(file)) continue;
            try
            {
                using var p = new StfsPackage(file);
                if (p.ContentType != StfsPackage.ContentTypeMarketplace || p.Execution.TitleId != KnownGame.TitleId) continue;
                long bytes = p.Entries.Where(e => !e.IsDirectory).Sum(e => (long)e.Length);
                found.Add(new DlcSource(Path.GetFileName(file), file, true, p.DisplayName, bytes));
            }
            catch { /* not a readable package - skip it */ }
        }
        return found;
    }

    /// <summary>
    /// Says what the user actually selected, instead of only saying "no". The
    /// TU23 and TU24 containers have identical file names, so without this the
    /// rejection is impossible to act on.
    /// </summary>
    static string DescribeWrongUpdate(string sha256, long xexpSize)
    {
        if (sha256 == KnownGame.Update24XexpSha256)
            return "This is Title Update 24, not 23. The recompilation was generated from the TU23 executable "
                 + "and cannot run on any other update.\n\nBoth downloads are named tu00000003_00000000 - tell "
                 + $"them apart by size: TU23 is {KnownGame.Tu23PackageSize:N0} bytes, TU24 is {KnownGame.Tu24PackageSize:N0}.";
        return "This is not Title Update 23, and not a version this installer recognises.\n\n"
             + $"Its Default.xexp is {xexpSize:N0} bytes (TU23's is {KnownGame.UpdateXexpSize:N0}), "
             + $"SHA-256 starts with {sha256[..16]}.\n\nTU23's package is {KnownGame.Tu23PackageSize:N0} bytes.";
    }

    public static bool LooksLikeExtractedDlc(string dir) =>
        File.Exists(Path.Combine(dir, "spa.bin")) || Directory.EnumerateFiles(dir, "DLC*.DAT2").Any();

    public static long DirSize(string dir)
    {
        long n = 0;
        foreach (var f in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories))
            n += new FileInfo(f).Length;
        return n;
    }

    public static string Sha256File(string path)
    {
        using var f = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(f)).ToLowerInvariant();
    }

    static string Sha256Stfs(StfsPackage p, StfsPackage.Entry entry)
    {
        string tmp = Path.GetTempFileName();
        try { p.Extract(entry, tmp); return Sha256File(tmp); }
        finally { File.Delete(tmp); }
    }
}

public sealed class InstallJob
{
    readonly InstallOptions o;
    readonly PayloadSource payload;
    readonly IProgress<InstallProgress> progress;
    readonly CancellationToken ct;
    readonly ReleaseManifest? release;
    // What we lay down, recorded as we go: this becomes install.json, which is
    // how the updater later knows which files are ours.
    readonly List<InstalledFile> installed = new();
    long totalBytes = 1, doneBytes;    // 1, not 0: the first Report() runs before the estimate
    string currentStatus = "";

    public InstallJob(InstallOptions options, PayloadSource payload, IProgress<InstallProgress> progress, CancellationToken ct)
    {
        o = options; this.payload = payload; this.progress = progress; this.ct = ct;
        release = ReleaseManifest.From(payload);
    }

    // Layout under the install folder. The .toml is the only place these
    // names live, so they can change freely.
    string GameDir => Path.Combine(o.InstallDir, "game");
    string UpdateDir => Path.Combine(o.InstallDir, "update");
    string ModdedUpdateDir => Path.Combine(o.InstallDir, "update-mods");
    string ContentDir => Path.Combine(o.InstallDir, "content");
    string DlcDir => Path.Combine(ContentDir, "0000000000000000", "5752084B", "00000002");
    string ModsDir => Path.Combine(o.InstallDir, "mods");
    string ToolsDir => Path.Combine(o.InstallDir, "tools");
    string ModCliExe => Path.Combine(ToolsDir, "modcli", "modcli.exe");
    string ToypadDir => Path.Combine(ToolsDir, "LegoToypad");
    string UpdaterDir => Path.Combine(ToolsDir, "rexupdate");
    public string ReadmePath => Path.Combine(o.InstallDir, "README.txt");

    /// <summary>Where each payload folder lands, so one table drives install and update alike.</summary>
    public static string DestinationFor(string payloadPrefix, string installDir) => payloadPrefix switch
    {
        "game" => installDir,
        "mods" => Path.Combine(installDir, "mods"),
        "modcli" => Path.Combine(installDir, "tools", "modcli"),
        "toypad" => Path.Combine(installDir, "tools", "LegoToypad"),
        "saveconverter" => Path.Combine(installDir, "tools", "SaveConverter"),
        "updater" => Path.Combine(installDir, "tools", "rexupdate"),
        _ => Path.Combine(installDir, payloadPrefix),
    };

    /// <summary>Rough size of what will be written, for the free-space check.</summary>
    public static long EstimateBytes(InstallOptions o, PayloadSource payload, IReadOnlyList<DlcSource> dlc)
    {
        long n = 0;
        if (Directory.Exists(o.GameDir)) n += Validation.DirSize(o.GameDir);
        if (Directory.Exists(o.UpdatePath)) n += Validation.DirSize(o.UpdatePath);
        else if (File.Exists(o.UpdatePath)) n += new FileInfo(o.UpdatePath).Length;
        n += dlc.Sum(d => d.Bytes);
        n += payload.SizeUnder("game");
        if (o.IncludeMods)
        {
            n += payload.SizeUnder("mods") + payload.SizeUnder("modcli");
            // update-mods: PATCH.DAT/HDR are real copies, the rest hard links.
            n += 850L * 1024 * 1024;
        }
        if (o.IncludeToypad) n += payload.SizeUnder("toypad");
        if (o.IncludeSaveConverter) n += payload.SizeUnder("saveconverter");
        if (o.IncludeUpdater) n += payload.SizeUnder("updater");
        return n;
    }

    public Task RunAsync(IReadOnlyList<DlcSource> dlc) => Task.Run(() => Run(dlc), ct);

    void Run(IReadOnlyList<DlcSource> dlc)
    {
        Report("Preparing...");
        totalBytes = Math.Max(1, EstimateBytes(o, payload, dlc));
        Directory.CreateDirectory(o.InstallDir);

        // 1. Game data. The Complete Pack dump carries a DLC inside
        //    5752084B\00000002; the game only reads DLC from the content root, so
        //    that sub-tree is skipped here and picked up by the DLC scan instead.
        string bundledDlcRoot = Path.GetFullPath(Path.Combine(o.GameDir, "5752084B"));
        CopyTree(o.GameDir, GameDir, "Copying game files",
                 skip: p => p.Equals(bundledDlcRoot, StringComparison.OrdinalIgnoreCase)
                            || p.StartsWith(bundledDlcRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase));

        // 2. Title Update.
        if (Directory.Exists(o.UpdatePath)) CopyTree(o.UpdatePath, UpdateDir, "Copying Title Update 23");
        else ExtractPackage(o.UpdatePath, UpdateDir, "Extracting Title Update 23");

        // 2a. The executable patch has to sit NEXT TO Default.xex: the runtime
        //     resolves the sibling path and nothing else, it never looks in
        //     update:. A "Complete Pack" style dump happens to ship a
        //     Default.xexp; a plain disc rip does not, and without it the game
        //     silently runs the UNPATCHED executable while our code was
        //     recompiled from the TU23 image - the first thread the game starts
        //     then lands mid-function and the title exits within a second.
        //     Copied unconditionally, so a dump carrying some other update's
        //     patch is corrected rather than trusted.
        string xexpSource = Path.Combine(UpdateDir, "Default.xexp");
        if (!File.Exists(xexpSource))
            throw new InvalidOperationException("The Title Update has no Default.xexp - it cannot be applied.");
        CopyFile(xexpSource, Path.Combine(GameDir, "Default.xexp"), "Applying Title Update 23");
        if (Validation.Sha256File(Path.Combine(GameDir, "Default.xexp")) != KnownGame.UpdateXexpSha256)
            throw new InvalidOperationException("Default.xexp did not copy into the game folder correctly.");

        // 3. DLC. Each package also needs its .header file: without one the game
        //    lists the DLC but refuses to mount any of it ("content not
        //    installed", characters unavailable). Verified both ways on a real
        //    install - 31 package mounts with the headers, zero without.
        Directory.CreateDirectory(DlcDir);
        foreach (var d in dlc)
        {
            ct.ThrowIfCancellationRequested();
            string dest = Path.Combine(DlcDir, d.Name);
            uint license = 1;
            if (d.IsPackageFile)
            {
                ExtractPackage(d.Path, dest, $"Extracting DLC: {d.DisplayName}");
                license = ReadPackageLicense(d.Path);
            }
            else
            {
                CopyTree(d.Path, dest, $"Copying DLC: {d.DisplayName}");
            }
            WriteContentHeader(d.Name, StfsPackage.ContentTypeMarketplace, license);
        }
        Directory.CreateDirectory(Path.Combine(ContentDir, "achievements"));

        // 4. The recomp itself. Everything under game/ lands next to the exe -
        //    including gamecontrollerdb.txt, which an older payload may lack.
        CopyPayload("game", o.InstallDir, "Installing the recomp");

        // 5. Optional components.
        if (o.IncludeMods)
        {
            CopyPayload("mods", ModsDir, "Installing mods");
            CopyPayload("modcli", Path.Combine(ToolsDir, "modcli"), "Installing mod tool");
            if (o.IncludeRussian)
            {
                CopyPayload("rus", ModsDir, "Installing the Russian translation");
            }
            BuildModdedUpdate();
            if (o.IncludeRussian)
            {
                ApplyMods(PayloadSource.RussianMods);
            }
        }
        if (o.IncludeToypad) CopyPayload("toypad", ToypadDir, "Installing LEGO Toypad app");
        if (o.IncludeSaveConverter) CopyPayload("saveconverter", Path.Combine(ToolsDir, "SaveConverter"), "Installing save converter");
        if (o.IncludeUpdater) InstallUpdater();

        // 6. Config + docs. The plan is also recorded in install.json, so a later
        //    update can tell a value the user changed from one we put there.
        Report("Writing configuration");
        var plan = TomlConfig.Apply(TomlConfig.Plan(BuildPaths(), BuildComponents(), Product.UpdateRepo,
                                                    File.Exists(Path.Combine(o.InstallDir, PayloadSource.GamepadDb))),
                                    release?.Toml);
        File.WriteAllText(Path.Combine(o.InstallDir, TomlConfig.FileName),
                          TomlConfig.Render(plan, WizardForm.AppName), new UTF8Encoding(false));
        File.WriteAllText(ReadmePath, BuildReadme(dlc.Count), new UTF8Encoding(false));
        WriteInstallManifest(plan, dlc);

        if (o.DesktopShortcut)
        {
            Report("Creating shortcut");
            try { CreateShortcut(WizardForm.AppName, Path.Combine(o.InstallDir, "legodimensions.exe")); }
            catch (Exception e) { Debug.WriteLine("shortcut: " + e); /* not worth failing the install over */ }
        }

        doneBytes = totalBytes;
        Report("Done");
    }

    /// <summary>
    /// The modded copy of the update folder that the F8 menu patches. Every file
    /// is a hard link back to the vanilla folder except PATCH.DAT/PATCH.HDR,
    /// which get modified in place and so must be real copies. Falls back to
    /// copying when the volume cannot link (FAT/exFAT).
    /// </summary>
    void BuildModdedUpdate()
    {
        Report("Preparing mod-ready update folder");
        Directory.CreateDirectory(ModdedUpdateDir);
        foreach (var src in Directory.EnumerateFiles(UpdateDir))
        {
            ct.ThrowIfCancellationRequested();
            string name = Path.GetFileName(src);
            string dst = Path.Combine(ModdedUpdateDir, name);
            bool mustCopy = name.Equals("PATCH.DAT", StringComparison.OrdinalIgnoreCase)
                            || name.Equals("PATCH.HDR", StringComparison.OrdinalIgnoreCase);
            if (File.Exists(dst)) File.Delete(dst);
            if (mustCopy || !CreateHardLinkW(dst, src, IntPtr.Zero))
                CopyFile(src, dst, "Preparing mod-ready update folder");
            // A dump taken off a disc often carries the read-only attribute, and
            // File.Copy keeps it. modcli has to write into PATCH.DAT, so a
            // read-only copy fails the whole apply with "access is denied".
            var copied = new FileInfo(dst);
            if (copied.Exists && copied.IsReadOnly) copied.IsReadOnly = false;
        }
    }

    /// <summary>
    /// Injects the chosen mods into the modded update folder, which is what the
    /// F8 menu does at runtime. Without it the config would name mods that were
    /// never applied and the game would still show vanilla text.
    /// </summary>
    void ApplyMods(IReadOnlyList<string> folders)
    {
        Report("Applying the Russian translation");
        if (!File.Exists(ModCliExe))
        {
            throw new InvalidOperationException("modcli.exe is missing, so mods cannot be applied.");
        }
        var psi = new ProcessStartInfo(ModCliExe)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        psi.ArgumentList.Add("apply");
        psi.ArgumentList.Add(ModdedUpdateDir);
        psi.ArgumentList.Add(ModsDir);
        psi.ArgumentList.Add("x360");
        // The same extra archive locations the F8 menu passes: a mod may name a
        // DLC archive (one package folder each, under the content root) or a
        // disc archive (GAME.DAT and friends). Without these modcli cannot find
        // them and fails the whole apply.
        psi.ArgumentList.Add("--search");
        psi.ArgumentList.Add(DlcDir);
        psi.ArgumentList.Add("--search");
        psi.ArgumentList.Add(GameDir);
        foreach (string folder in folders) psi.ArgumentList.Add(folder);
        using var proc = Process.Start(psi)
            ?? throw new InvalidOperationException("Could not start modcli.exe.");
        string output = proc.StandardOutput.ReadToEnd() + proc.StandardError.ReadToEnd();
        proc.WaitForExit();
        if (proc.ExitCode != 0)
        {
            throw new InvalidOperationException(
                "Applying the Russian translation failed: " + output.Trim());
        }
    }

    /// <summary>
    /// The updater is this same executable with the payload stripped off - the
    /// binary as published, before build-installer.ps1 appended anything. Run
    /// with no payload behind it, it starts in updater mode (see Program.Main).
    /// A payload that ships one explicitly wins, which is how an update pack
    /// replaces the updater itself.
    /// </summary>
    void InstallUpdater()
    {
        Report("Installing the updater");
        if (payload.HasUpdater)
        {
            CopyPayload("updater", UpdaterDir, "Installing the updater");
            return;
        }
        string dest = Path.Combine(UpdaterDir, UpdaterExeName);
        if (!PayloadSource.TryWriteHostWithoutPayload(dest))
        {
            // Running from a payload folder during development, or from an exe
            // with no footer. Not worth failing an install over.
            Debug.WriteLine("updater: no appended payload to strip, skipping");
            return;
        }
        installed.Add(new InstalledFile
        {
            Path = Path.GetRelativePath(o.InstallDir, dest).Replace('\\', '/'),
            Payload = "updater/" + UpdaterExeName,
            Sha = InstallManifest.Sha256(dest),
            Len = new FileInfo(dest).Length,
        });
    }

    public const string UpdaterExeName = "rexupdate.exe";

    /// <summary>The layout of this install, as recorded for the updater.</summary>
    InstalledPaths BuildPaths() => new()
    {
        InstallDir = o.InstallDir,
        GameDataRoot = GameDir,
        UpdateDataRoot = UpdateDir,
        ModsUpdateRoot = ModdedUpdateDir,
        ContentRoot = ContentDir,
        ModsRoot = ModsDir,
        ToolsRoot = ToolsDir,
    };

    InstalledComponents BuildComponents() => new()
    {
        Mods = o.IncludeMods,
        Russian = o.IncludeMods && o.IncludeRussian,
        Toypad = o.IncludeToypad,
        SaveConverter = o.IncludeSaveConverter,
        // Checked on disk, not asked of the options: stripping the payload off
        // ourselves can fail (a dev run from a payload folder), and the config
        // must not point at an updater that is not there.
        Updater = o.IncludeUpdater && File.Exists(Path.Combine(UpdaterDir, UpdaterExeName)),
    };

    /// <summary>
    /// install.json: the record of what this install is. Written last, so a
    /// half-finished install has none and is never mistaken for a good one.
    /// </summary>
    void WriteInstallManifest(List<TomlItem> plan, IReadOnlyList<DlcSource> dlc)
    {
        var manifest = new InstallManifest
        {
            Version = release?.Version ?? "0.0.0",
            InstalledUtc = DateTime.UtcNow.ToString("o"),
            UpdatedUtc = DateTime.UtcNow.ToString("o"),
            Components = BuildComponents(),
            Paths = BuildPaths(),
            Files = installed,
            TomlWritten = TomlConfig.Record(plan),
            Dlc = dlc.Select(d => d.Name).ToList(),
            UpdateRepo = Product.UpdateRepo,
        };
        manifest.Save(o.InstallDir);
    }

    string BuildReadme(int dlcCount)
    {
        using var s = typeof(InstallJob).Assembly.GetManifestResourceStream("README.txt")
                      ?? throw new InvalidOperationException("README template missing from the installer.");
        using var r = new StreamReader(s);
        string text = r.ReadToEnd();
        string toypadExe = o.IncludeToypad
            ? Directory.EnumerateFiles(ToypadDir, "*.exe").Select(Path.GetFileName).FirstOrDefault() ?? "LegoToypad.exe"
            : "";
        return text
            .Replace("{INSTALL_DIR}", o.InstallDir)
            .Replace("{DLC_COUNT}", dlcCount.ToString())
            .Replace("{TOYPAD_LINE}", o.IncludeToypad
                ? $@"   It is installed at:  {o.InstallDir}\tools\LegoToypad\{toypadExe}"
                : "   You chose not to install it. Get it from https://github.com/harrysof/LegoToypad/releases")
            .Replace("{MODS_SECTION}", o.IncludeMods ? ReadmeMods : "Mods were not installed. Re-run the installer to add them.")
            .Replace("{SAVECONV_SECTION}", o.IncludeSaveConverter
                ? $@"A save converter is installed at {o.InstallDir}\tools\SaveConverter. Read its ""READ ME FIRST.txt"" before touching any save."
                : "The optional save converter (for bringing a save over from xenia or a console) was not installed.");
    }

    const string ReadmeMods =
@"Press F8 in game to open the mod menu. Tick the mods you want and click Apply,
then restart the game. All mods start OFF. Bundled mods:

   QuickStartup        skips the intro splash screens
   Recomp_TextTest     a harmless text change, useful to confirm mods work

To add a mod, drop its folder (with mod.json) into the ""mods"" folder.

Mods never touch your original files: they are applied to the ""update-mods""
copy, and turning them off restores the vanilla bytes.";

    /// <summary>
    /// The per-package record the runtime's ContentManager expects at
    /// content\&lt;xuid&gt;\&lt;title&gt;\Headers\&lt;type&gt;\&lt;name&gt;.header: an
    /// XCONTENT_AGGREGATE_DATA (0x148 bytes, big-endian scalars) followed by the
    /// package's license mask as a native uint32. Field offsets come from
    /// rexglue-sdk/include/rex/system/xam/content_manager.h; the bytes this
    /// writes are identical to the ones the runtime writes itself when it
    /// installs a package.
    /// </summary>
    void WriteContentHeader(string packageName, uint contentType, uint license)
    {
        byte[] h = new byte[0x14C];
        WriteBE32(h, 0x000, 1);                       // device_id: 1 = HDD
        WriteBE32(h, 0x004, contentType);
        // display_name: 128 UTF-16BE chars. The runtime stores the package name
        // here for hand-installed content, so match that.
        for (int i = 0; i < packageName.Length && i < 128; i++)
        {
            h[0x008 + i * 2] = (byte)(packageName[i] >> 8);
            h[0x008 + i * 2 + 1] = (byte)packageName[i];
        }
        // file_name: 42 ASCII bytes, deliberately not null-terminated when the
        // name fills the field - the two padding bytes after it are the
        // terminator, and they are already zero.
        byte[] ascii = Encoding.ASCII.GetBytes(packageName);
        Array.Copy(ascii, 0, h, 0x108, Math.Min(ascii.Length, 42));
        // 0x138 xuid stays 0: marketplace content is stored per console.
        WriteBE32(h, 0x140, KnownGame.TitleId);
        BitConverter.GetBytes(license).CopyTo(h, 0x148);

        string path = Path.Combine(ContentDir, "0000000000000000", "5752084B",
                                   "Headers", contentType.ToString("X8"), packageName + ".header");
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllBytes(path, h);
    }

    static void WriteBE32(byte[] b, int at, uint v)
    {
        b[at] = (byte)(v >> 24); b[at + 1] = (byte)(v >> 16); b[at + 2] = (byte)(v >> 8); b[at + 3] = (byte)v;
    }

    /// <summary>
    /// The license mask the runtime computes from an STFS package: the OR of the
    /// license bits of every entry whose flags are set (content_manager.cpp).
    /// Falls back to bit 0, which is what every LEGO Dimensions DLC carries.
    /// </summary>
    static uint ReadPackageLicense(string packagePath)
    {
        try
        {
            using var f = File.OpenRead(packagePath);
            f.Position = 0x22C;                       // XContentHeader.licenses
            byte[] buf = new byte[0x10 * 0x10];
            f.ReadExactly(buf);
            uint mask = 0;
            for (int i = 0; i < 0x10; i++)
            {
                uint bits = BinaryPrimitives.ReadUInt32BigEndian(buf.AsSpan(i * 0x10 + 8));
                uint flags = BinaryPrimitives.ReadUInt32BigEndian(buf.AsSpan(i * 0x10 + 12));
                if (flags != 0) mask |= bits;
            }
            return mask != 0 ? mask : 1;
        }
        catch { return 1; }
    }

    // ---- plumbing -------------------------------------------------------

    /// <summary>Writes every payload file under <paramref name="prefix"/> into a folder.</summary>
    void CopyPayload(string prefix, string dstDir, string status)
    {
        foreach (var entry in payload.Under(prefix))
        {
            ct.ThrowIfCancellationRequested();
            string rel = entry.Path[(prefix.Length + 1)..].Replace('/', Path.DirectorySeparatorChar);
            Report($"{status}: {Path.GetFileName(rel)}");
            string dest = Path.Combine(dstDir, rel);
            payload.CopyTo(entry, dest, Advance);
            // The hash comes from the payload manifest when there is one; only a
            // pre-manifest payload pays for hashing 190 MB during an install.
            installed.Add(new InstalledFile
            {
                Path = Path.GetRelativePath(o.InstallDir, dest).Replace('\\', '/'),
                Payload = entry.Path,
                Sha = release?.Find(entry.Path)?.Sha ?? InstallManifest.Sha256(dest),
                Len = entry.Length,
            });
        }
    }

    /// <param name="skip">Full, normalised directory path -> true to leave it and everything below it out.</param>
    void CopyTree(string src, string dst, string status, Func<string, bool>? skip = null)
    {
        if (!Directory.Exists(src)) return;
        // Paths from the command line can mix / and \; compare normalised ones
        // or a skip rule silently misses nested folders.
        src = Path.GetFullPath(src);
        Directory.CreateDirectory(dst);
        foreach (var dir in Directory.EnumerateDirectories(src, "*", SearchOption.AllDirectories))
        {
            string full = Path.GetFullPath(dir);
            if (skip is not null && skip(full)) continue;
            Directory.CreateDirectory(Path.Combine(dst, Path.GetRelativePath(src, full)));
        }
        foreach (var file in Directory.EnumerateFiles(src, "*", SearchOption.AllDirectories))
        {
            string full = Path.GetFullPath(file);
            if (skip is not null && skip(Path.GetDirectoryName(full)!)) continue;
            CopyFile(full, Path.Combine(dst, Path.GetRelativePath(src, full)), status);
        }
    }

    void CopyFile(string src, string dst, string status)
    {
        ct.ThrowIfCancellationRequested();
        Report($"{status}: {Path.GetFileName(src)}");
        Directory.CreateDirectory(Path.GetDirectoryName(dst)!);
        using var input = new FileStream(src, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20, FileOptions.SequentialScan);
        using var output = new FileStream(dst, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20);
        byte[] buf = new byte[1 << 20];
        int n;
        while ((n = input.Read(buf, 0, buf.Length)) > 0)
        {
            ct.ThrowIfCancellationRequested();
            output.Write(buf, 0, n);
            Advance(n);
        }
    }

    void ExtractPackage(string file, string destDir, string status)
    {
        using var p = new StfsPackage(file);
        Directory.CreateDirectory(destDir);
        foreach (var e in p.Entries)
        {
            ct.ThrowIfCancellationRequested();
            string dest = Path.Combine(destDir, e.Path);
            if (e.IsDirectory) { Directory.CreateDirectory(dest); continue; }
            Report($"{status}: {e.Path}");
            p.Extract(e, dest, Advance);
        }
    }

    void Advance(long n)
    {
        doneBytes += n;
        progress.Report(new InstallProgress(Math.Min(1.0, (double)doneBytes / totalBytes), currentStatus));
    }

    void Report(string status)
    {
        currentStatus = status;
        progress.Report(new InstallProgress(Math.Min(1.0, (double)doneBytes / totalBytes), status));
    }

    static void CreateShortcut(string name, string target)
    {
        string desktop = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
        string lnk = Path.Combine(desktop, name + ".lnk");
        Type shellType = Type.GetTypeFromProgID("WScript.Shell") ?? throw new InvalidOperationException("WScript.Shell unavailable");
        dynamic shell = Activator.CreateInstance(shellType)!;
        dynamic sc = shell.CreateShortcut(lnk);
        sc.TargetPath = target;
        sc.WorkingDirectory = Path.GetDirectoryName(target);
        sc.IconLocation = target + ",0";
        sc.Description = WizardForm.AppName;
        sc.Save();
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateHardLinkW(string newFile, string existingFile, IntPtr reserved);
}
