using System.Diagnostics;
using System.Runtime.InteropServices;

namespace RecompSetup;

/// <summary>
/// The updater: this same executable, installed to tools\rexupdate\rexupdate.exe
/// with the payload stripped off (InstallJob.InstallUpdater). With no payload
/// behind it there is nothing to install, so it goes looking for a new release
/// instead.
///
/// <para>
///   rexupdate.exe                     check, and show the window either way
///   rexupdate.exe --quiet             check, and show nothing unless there is an update
///   rexupdate.exe --apply pack.rxp    apply a pack from disk, no network
///   rexupdate.exe --console           print instead of opening a window
///   rexupdate.exe --dir DIR           the install to work on (default: two levels up)
///   rexupdate.exe --repo owner/name   override the release feed
///   rexupdate.exe --token TOKEN       for a repository that is still private
/// </para>
///
/// <para>
/// --quiet is what the game runs at startup when updates_check is on. It is a
/// separate process on purpose: a failed lookup, a proxy or a dead network can
/// never hold the game up.
/// </para>
/// </summary>
public static class Updater
{
    /// <summary>Exit code for --console when a newer release exists.</summary>
    public const int UpdateAvailable = 10;

    public static int Run(string[] args)
    {
        string? dir = null, apply = null, repo = null, token = null;
        bool quiet = false, console = false;
        for (int i = 0; i < args.Length; i++)
        {
            string? Next() => i + 1 < args.Length ? args[++i] : null;
            switch (args[i])
            {
                case "--dir": dir = Next(); break;
                case "--apply": apply = Next(); break;
                case "--repo": repo = Next(); break;
                case "--token": token = Next(); break;
                case "--quiet": quiet = true; break;
                case "--console": console = true; break;
                case "--check": case "--update": break;    // the default action
                default: return Fail(true, "unknown option: " + args[i]);
            }
        }

        string installDir = Path.GetFullPath(dir ?? DefaultInstallDir());
        var install = InstallManifest.Load(installDir);
        if (install is null)
        {
            if (quiet) return 1;
            return Fail(console,
                $"No {InstallManifest.FileName} in {installDir}.\r\n\r\nThis folder was not set up by the "
                + "installer, or it was installed by a version older than 1.0.");
        }
        // Installs get moved. The recorded paths are only trustworthy when the
        // folder they point at is the one we were run from.
        if (!string.Equals(Path.GetFullPath(install.Paths.InstallDir), installDir, StringComparison.OrdinalIgnoreCase))
            install.Paths = Relocate(installDir);

        token = UpdateFeed.ResolveToken(token);
        if (apply is not null) return ApplyLocal(install, apply, console);

        string feed = repo ?? (install.UpdateRepo.Length > 0 ? install.UpdateRepo : Product.UpdateRepo);
        ReleaseInfo? release;
        try
        {
            release = UpdateFeed.LatestAsync(feed, token, CancellationToken.None).GetAwaiter().GetResult();
        }
        catch (Exception e)
        {
            // Being offline is not worth interrupting anyone over.
            if (quiet) return 0;
            return Fail(console, "Could not reach GitHub: " + e.Message);
        }

        bool newer = release is not null
                     && ReleaseManifest.CompareVersions(release.Version, install.Version) > 0;
        if (!newer || (quiet && release!.Version == install.SkippedVersion))
        {
            if (quiet) return 0;
            string message = release is null ? "No releases found." : $"You are up to date ({install.Version}).";
            if (console) { Print(message); return 0; }
            ApplicationConfiguration.Initialize();
            MessageBox.Show(message, WizardForm.AppName + " - Update", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return 0;
        }

        if (console)
        {
            Print($"{install.Version} -> {release!.Version} ({release.Title})");
            Print(release.PackUrl is null ? "no update pack in this release" : "pack: " + release.PackUrl);
            return UpdateAvailable;
        }

        ApplicationConfiguration.Initialize();
        Application.Run(new UpdaterForm(install, release!, token));
        return 0;
    }

    /// <summary>
    /// tools\rexupdate\rexupdate.exe -> the install folder. Falls back to the
    /// folder we are in, which is all a copy run from anywhere else can expect.
    /// </summary>
    static string DefaultInstallDir()
    {
        string here = AppContext.BaseDirectory;
        var parent = Directory.GetParent(here.TrimEnd(Path.DirectorySeparatorChar))?.Parent;
        if (parent is not null && File.Exists(Path.Combine(parent.FullName, InstallManifest.FileName)))
            return parent.FullName;
        return here;
    }

    /// <summary>Rebuilds the recorded layout around a folder that has moved.</summary>
    static InstalledPaths Relocate(string installDir) => new()
    {
        InstallDir = installDir,
        GameDataRoot = Path.Combine(installDir, "game"),
        UpdateDataRoot = Path.Combine(installDir, "update"),
        ModsUpdateRoot = Path.Combine(installDir, "update-mods"),
        ContentRoot = Path.Combine(installDir, "content"),
        ModsRoot = Path.Combine(installDir, "mods"),
        ToolsRoot = Path.Combine(installDir, "tools"),
    };

    static int ApplyLocal(InstallManifest install, string packPath, bool console)
    {
        if (!File.Exists(packPath)) return Fail(console, "No such pack: " + packPath);
        try
        {
            using var pack = PayloadSource.OpenFile(packPath);
            var manifest = ReleaseManifest.From(pack)
                ?? throw new InvalidOperationException("The pack has no manifest.json.");
            var job = new UpdateJob(install);
            var plan = job.Plan(manifest, pack);
            if (plan.Missing.Count > 0)
                throw new InvalidOperationException("The pack is missing " + string.Join(", ", plan.Missing));
            if (!plan.Usable)
            {
                if (console) Print($"Already at {manifest.Version}, nothing to do.");
                return 0;
            }
            string? locked = job.WhatIsLocked(plan);
            if (locked is not null) throw new InvalidOperationException(locked + " is in use. Close the game first.");

            string last = "";
            var result = job.Apply(plan, pack, (_, text) =>
            {
                if (text == last) return;
                last = text;
                if (console) Print(text);
            }, CancellationToken.None);

            if (result.SelfUpdate is not null)
                UpdateJob.DeferSelfReplace(result.SelfUpdate.Value.Staging, result.SelfUpdate.Value.Dest);

            string summary = $"Updated {result.From} -> {result.To}, {result.Replaced.Count} file(s).";
            if (console) Print(summary);
            else MessageBox.Show(summary, WizardForm.AppName + " - Update", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return 0;
        }
        catch (Exception e)
        {
            return Fail(console, e.Message);
        }
    }

    static int Fail(bool console, string message)
    {
        if (console)
        {
            AttachConsole(-1);
            Console.Error.WriteLine(message);
        }
        else
        {
            ApplicationConfiguration.Initialize();
            MessageBox.Show(message, WizardForm.AppName + " - Update", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
        return 1;
    }

    static void Print(string line)
    {
        AttachConsole(-1);
        Console.WriteLine(line);
        Debug.WriteLine(line);
    }

    [DllImport("kernel32.dll")]
    static extern bool AttachConsole(int pid);
}
