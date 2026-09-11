using System.Diagnostics;
using System.Text;

namespace RecompSetup;

/// <summary>What an update would do, worked out before anything is touched.</summary>
public sealed class UpdatePlan
{
    public required InstallManifest Install { get; init; }
    public required ReleaseManifest Release { get; init; }
    /// <summary>Files whose content differs from the release. Payload paths.</summary>
    public List<ReleaseFile> Changed { get; } = new();
    /// <summary>Changed files the pack does not carry - it cannot finish the job.</summary>
    public List<string> Missing { get; } = new();
    /// <summary>Payload paths this release retires.</summary>
    public List<string> Remove { get; } = new();
    public long Bytes => Changed.Sum(f => f.Len);
    public bool AnyMods => Changed.Any(f => f.P.StartsWith("mods/", StringComparison.OrdinalIgnoreCase));
    public bool Usable => Missing.Count == 0 && (Changed.Count > 0 || Remove.Count > 0);
}

/// <summary>
/// Applies an update pack to an existing install.
///
/// <para>
/// The rules it works by, in order of how much trouble breaking them causes:
/// nothing outside the release's file list is ever written (so the game dump,
/// the DLC, saves and the user's own mods cannot be harmed); every file is
/// staged next to its target and swapped in only after its hash checks out;
/// the previous copy goes to backup\&lt;version&gt;\ first; and legodimensions.toml
/// is merged, never overwritten, because the game rewrites it on every exit.
/// </para>
/// </summary>
public sealed class UpdateJob
{
    public const string StagingSuffix = ".rexnew";

    readonly InstallManifest install;
    readonly string installDir;

    public UpdateJob(InstallManifest install)
    {
        this.install = install;
        installDir = install.Paths.InstallDir;
    }

    /// <summary>Where a payload path lands in this install.</summary>
    public string Destination(string payloadPath)
    {
        int slash = payloadPath.IndexOf('/');
        string prefix = slash < 0 ? "" : payloadPath[..slash];
        string rel = slash < 0 ? payloadPath : payloadPath[(slash + 1)..];
        return Path.Combine(InstallJob.DestinationFor(prefix, installDir),
                            rel.Replace('/', Path.DirectorySeparatorChar));
    }

    /// <summary>
    /// Works out what has to change. Files are compared against what is actually
    /// on disk, not against install.json's record of them, so a file the user
    /// deleted or replaced is put back.
    /// </summary>
    public UpdatePlan Plan(ReleaseManifest release, PayloadSource? pack)
    {
        var plan = new UpdatePlan { Install = install, Release = release };
        foreach (var file in release.Files)
        {
            if (!install.WantsPayload(file.P)) continue;      // component not installed
            string dest = Destination(file.P);
            if (File.Exists(dest) && InstallManifest.Sha256(dest) == file.Sha) continue;

            plan.Changed.Add(file);
            if (pack is not null && pack.Find(file.P) is null) plan.Missing.Add(file.P);
        }
        foreach (string gone in release.Removed)
        {
            if (File.Exists(Destination(gone))) plan.Remove.Add(gone);
        }
        return plan;
    }

    /// <summary>
    /// The name of a file that is locked, or null when everything can be
    /// replaced. The caller has to ask the user to close the game: replacing a
    /// running executable is the one thing Windows will not do.
    /// </summary>
    public string? WhatIsLocked(UpdatePlan plan)
    {
        foreach (var file in plan.Changed)
        {
            string dest = Destination(file.P);
            if (!File.Exists(dest) || IsSelf(dest)) continue;
            string ext = Path.GetExtension(dest);
            if (!ext.Equals(".exe", StringComparison.OrdinalIgnoreCase)
                && !ext.Equals(".dll", StringComparison.OrdinalIgnoreCase)) continue;
            try
            {
                using var probe = new FileStream(dest, FileMode.Open, FileAccess.ReadWrite, FileShare.None);
            }
            catch (IOException) { return Path.GetFileName(dest); }
            catch (UnauthorizedAccessException) { return Path.GetFileName(dest); }
        }
        return null;
    }

    /// <summary>The updater cannot overwrite itself while it runs; that swap is deferred.</summary>
    static bool IsSelf(string path)
    {
        string self = Environment.ProcessPath ?? "";
        return self.Length > 0 && string.Equals(Path.GetFullPath(path), Path.GetFullPath(self),
                                                StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// Does the work. <paramref name="report"/> gets a 0..1 fraction and a status
    /// line, on the calling thread.
    /// </summary>
    public UpdateResult Apply(UpdatePlan plan, PayloadSource pack, Action<double, string> report,
                              CancellationToken ct)
    {
        if (!plan.Usable)
        {
            throw new InvalidOperationException(plan.Missing.Count > 0
                ? "This update pack does not contain every changed file. Run the full installer instead."
                : "Nothing to update.");
        }

        var result = new UpdateResult { From = install.Version, To = plan.Release.Version };
        string backupDir = Path.Combine(installDir, "backup", install.Version);
        long total = Math.Max(1, plan.Bytes);
        long done = 0;
        var staged = new List<(string Staging, string Dest, ReleaseFile File)>();

        try
        {
            // 1. Stage every file first. Nothing in the install is touched until
            //    all of them are written and verified, so a failure halfway
            //    through - a full disk, a damaged download - changes nothing.
            foreach (var file in plan.Changed)
            {
                ct.ThrowIfCancellationRequested();
                string dest = Destination(file.P);
                string staging = dest + StagingSuffix;
                string what = "Unpacking " + Path.GetFileName(dest);
                report(done / (double)total, what);
                var entry = pack.Find(file.P)!;
                pack.CopyTo(entry, staging, n => { done += n; report(done / (double)total, what); });
                if (InstallManifest.Sha256(staging) != file.Sha)
                {
                    throw new InvalidOperationException(
                        $"{file.P} did not unpack correctly (hash mismatch). The download may be damaged.");
                }
                staged.Add((staging, dest, file));
            }

            // 2. Back up what is about to be replaced, then swap it in.
            foreach (var (staging, dest, file) in staged)
            {
                ct.ThrowIfCancellationRequested();
                report(0.9, "Replacing " + Path.GetFileName(dest));
                if (File.Exists(dest))
                {
                    string backup = Path.Combine(backupDir, file.P.Replace('/', Path.DirectorySeparatorChar));
                    Directory.CreateDirectory(Path.GetDirectoryName(backup)!);
                    File.Copy(dest, backup, overwrite: true);
                    result.BackedUp++;
                }
                if (IsSelf(dest))
                {
                    // Left staged on purpose; DeferSelfReplace does it after exit.
                    result.SelfUpdate = (staging, dest);
                    continue;
                }
                if (File.Exists(dest)) File.Delete(dest);
                File.Move(staging, dest);
                result.Replaced.Add(file.P);
            }

            // 3. Files this release retired.
            foreach (string gone in plan.Remove)
            {
                string dest = Destination(gone);
                string backup = Path.Combine(backupDir, gone.Replace('/', Path.DirectorySeparatorChar));
                try
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(backup)!);
                    File.Copy(dest, backup, overwrite: true);
                    File.Delete(dest);
                    result.Removed.Add(gone);
                }
                catch (Exception e) { Debug.WriteLine("remove " + gone + ": " + e.Message); }
            }
        }
        catch
        {
            foreach (var (staging, _, _) in staged)
            {
                try { if (File.Exists(staging)) File.Delete(staging); } catch { }
            }
            throw;
        }

        // 4. Config. Merged, so anything the user changed in F4 survives.
        report(0.95, "Updating configuration");
        var newPlan = TomlConfig.Apply(
            TomlConfig.Plan(install.Paths, install.Components, install.UpdateRepo,
                            File.Exists(Path.Combine(installDir, PayloadSource.GamepadDb))),
            plan.Release.Toml);
        string tomlPath = Path.Combine(installDir, TomlConfig.FileName);
        if (File.Exists(tomlPath))
        {
            string merged = TomlConfig.Merge(File.ReadAllText(tomlPath), newPlan, install.TomlWritten, out var kept);
            File.WriteAllText(tomlPath, merged, new UTF8Encoding(false));
            result.KeptSettings = kept;
        }
        else
        {
            File.WriteAllText(tomlPath, TomlConfig.Render(newPlan, WizardForm.AppName), new UTF8Encoding(false));
        }

        // 5. Mods. The mod tool patches update-mods\PATCH.DAT in place, so a
        //    changed bundled mod has to be re-applied over restored bytes -
        //    otherwise the new mod lands on top of the old one's edits.
        if (install.Components.Mods && plan.AnyMods)
        {
            report(0.97, "Re-applying mods");
            result.ModsReapplied = ReapplyMods(tomlPath);
        }

        // 6. Record the new state.
        report(0.99, "Finishing");
        install.Version = plan.Release.Version;
        install.UpdatedUtc = DateTime.UtcNow.ToString("o");
        install.TomlWritten = TomlConfig.Record(newPlan);
        foreach (var file in plan.Changed)
        {
            var record = install.Find(file.P);
            if (record is null)
            {
                install.Files.Add(new InstalledFile
                {
                    Path = Path.GetRelativePath(installDir, Destination(file.P)).Replace('\\', '/'),
                    Payload = file.P,
                    Sha = file.Sha,
                    Len = file.Len,
                });
            }
            else { record.Sha = file.Sha; record.Len = file.Len; }
        }
        install.Files.RemoveAll(f => result.Removed.Contains(f.Payload));
        install.Save(installDir);
        report(1.0, "Done");
        return result;
    }

    /// <summary>
    /// Restores the modded update folder and re-applies whatever was enabled, by
    /// driving the same modcli the F8 menu uses. Returns what it did, or null
    /// when there is no tool to do it with.
    /// </summary>
    string? ReapplyMods(string tomlPath)
    {
        string modcli = Path.Combine(install.Paths.ToolsRoot, "modcli", "modcli.exe");
        if (!File.Exists(modcli)) return null;

        string selection = ReadTomlValue(tomlPath, "mods") ?? "";
        var enabled = selection.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);

        string target = install.Paths.ModsUpdateRoot;
        Run(modcli, $"restore \"{target}\"");
        if (enabled.Length == 0) return "restored (no mods enabled)";

        // The same extra archive locations the F8 menu passes: a mod may name a
        // DLC archive (one package folder each, under the content root) or a
        // disc archive (GAME.DAT and friends). Without these modcli cannot find
        // them and fails the whole re-apply.
        string dlcDir = Path.Combine(install.Paths.ContentRoot,
                                     "0000000000000000", "5752084B", "00000002");
        string args = $"apply \"{target}\" \"{install.Paths.ModsRoot}\" x360 "
                    + $"--search \"{dlcDir}\" --search \"{install.Paths.GameDataRoot}\" "
                    + string.Join(' ', enabled.Select(m => "\"" + m + "\""));
        int rc = Run(modcli, args);
        return rc == 0 ? "re-applied: " + string.Join(", ", enabled)
                       : "modcli failed (" + rc + ") - open the F8 menu and press Apply";
    }

    static int Run(string exe, string args)
    {
        var psi = new ProcessStartInfo(exe, args) { UseShellExecute = false, CreateNoWindow = true };
        using var p = Process.Start(psi);
        if (p is null) return -1;
        p.WaitForExit();
        return p.ExitCode;
    }

    /// <summary>Reads one flat key out of the config. Enough for "mods"; not a TOML parser.</summary>
    public static string? ReadTomlValue(string tomlPath, string key)
    {
        if (!File.Exists(tomlPath)) return null;
        foreach (string line in File.ReadLines(tomlPath))
        {
            string t = line.TrimStart();
            if (t.Length == 0 || t[0] == '#') continue;
            int eq = t.IndexOf('=');
            if (eq <= 0 || t[..eq].Trim() != key) continue;
            return TomlConfig.ParseValue(t[(eq + 1)..].Trim());
        }
        return null;
    }

    /// <summary>
    /// Hands the last swap to a detached cmd: it waits for us to exit, then moves
    /// the staged updater over the running one. Windows will not let a process
    /// replace its own executable.
    /// </summary>
    public static void DeferSelfReplace(string staging, string dest)
    {
        string command = $"ping -n 3 127.0.0.1 >nul & move /y \"{staging}\" \"{dest}\" >nul";
        var psi = new ProcessStartInfo("cmd.exe", "/c " + command)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            WindowStyle = ProcessWindowStyle.Hidden,
        };
        try { Process.Start(psi); } catch (Exception e) { Debug.WriteLine("self-replace: " + e); }
    }
}

public sealed class UpdateResult
{
    public string From { get; init; } = "";
    public string To { get; init; } = "";
    public List<string> Replaced { get; } = new();
    public List<string> Removed { get; } = new();
    public List<string> KeptSettings { get; set; } = new();
    public int BackedUp { get; set; }
    public string? ModsReapplied { get; set; }
    /// <summary>Set when the updater updated itself; the swap happens after it exits.</summary>
    public (string Staging, string Dest)? SelfUpdate { get; set; }
}
