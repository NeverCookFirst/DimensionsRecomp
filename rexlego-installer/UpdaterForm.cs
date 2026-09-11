using System.Diagnostics;

namespace RecompSetup;

/// <summary>
/// The updater's window: what the new release is, what it will change, and one
/// button to do it. Deliberately small - the game opens it at startup, so it has
/// to be dismissable in one click.
/// </summary>
public sealed class UpdaterForm : Form
{
    readonly InstallManifest install;
    readonly ReleaseInfo release;
    readonly string? token;
    readonly CancellationTokenSource cts = new();

    readonly Label heading = new();
    readonly Label sub = new();
    readonly TextBox notes = new();
    readonly ProgressBar progress = new();
    readonly Label status = new();
    readonly Button update = new() { Text = "Update now" };
    readonly Button skip = new() { Text = "Skip this version" };
    readonly Button later = new() { Text = "Later" };
    readonly CheckBox launch = new() { Text = "Start the game when finished" };

    bool working;

    public UpdaterForm(InstallManifest install, ReleaseInfo release, string? token)
    {
        this.install = install;
        this.release = release;
        this.token = token;

        Text = WizardForm.AppName + " - Update";
        Font = new Font("Segoe UI", 9.75f);
        ClientSize = new Size(560, 420);
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

        heading.Font = new Font("Segoe UI Semibold", 12f);
        heading.Bounds = new Rectangle(18, 16, 524, 26);
        heading.Text = $"Version {release.Version} is available";

        sub.Bounds = new Rectangle(20, 44, 524, 40);
        sub.Text = $"You have {install.Version}. Only the changed files are downloaded"
                 + (release.PackBytes > 0 ? $" ({release.PackBytes / 1024.0 / 1024.0:0.#} MB)." : ".")
                 + "\r\nYour game files, DLC and saves are not touched.";

        notes.Bounds = new Rectangle(20, 92, 520, 210);
        notes.Multiline = true;
        notes.ReadOnly = true;
        notes.ScrollBars = ScrollBars.Vertical;
        notes.BackColor = Color.White;
        notes.Text = string.IsNullOrWhiteSpace(release.Notes)
            ? "(no release notes)"
            : release.Notes.Replace("\r\n", "\n").Replace("\n", "\r\n");

        launch.Bounds = new Rectangle(20, 308, 300, 22);

        status.Bounds = new Rectangle(20, 92, 520, 20);
        status.Visible = false;
        progress.Bounds = new Rectangle(20, 118, 520, 18);
        progress.Visible = false;

        skip.Bounds = new Rectangle(20, 372, 140, 30);
        update.Bounds = new Rectangle(275, 372, 110, 30);
        later.Bounds = new Rectangle(395, 372, 145, 30);
        update.Click += async (_, _) => await RunUpdateAsync();
        skip.Click += (_, _) =>
        {
            install.SkippedVersion = release.Version;
            install.Save(install.Paths.InstallDir);
            Close();
        };
        later.Click += (_, _) => Close();
        // Half-applied is the one state worth preventing; the window simply
        // refuses to close while files are being swapped.
        FormClosing += (_, e) => { if (working) e.Cancel = true; };

        Controls.AddRange(new Control[] { heading, sub, notes, launch, status, progress, update, skip, later });
        AcceptButton = update;
    }

    async Task RunUpdateAsync()
    {
        if (release.PackUrl is null)
        {
            if (MessageBox.Show(this,
                    "This release has no update pack, so the full installer is needed.\r\n\r\nOpen the release page?",
                    Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes)
                Open(release.PageUrl);
            return;
        }

        working = true;
        update.Enabled = skip.Enabled = later.Enabled = false;
        notes.Visible = launch.Visible = sub.Visible = false;
        progress.Visible = status.Visible = true;

        string pack = Path.Combine(Path.GetTempPath(), $"DimensionsRecompiled-{release.Version}.rxp");
        try
        {
            Set(0, "Downloading...");
            await UpdateFeed.DownloadAsync(release.PackUrl, pack, token,
                (done, total) => BeginInvoke(() => Set(total > 0 ? 0.5 * done / total : 0,
                    $"Downloading... {done / 1024.0 / 1024.0:0.#} MB")), cts.Token);

            var result = await Task.Run(() => ApplyPack(pack), cts.Token);
            Finish(result);
        }
        catch (Exception e)
        {
            working = false;
            update.Enabled = later.Enabled = true;
            status.Text = "Update failed. Nothing was changed.";
            MessageBox.Show(this, e.Message, "Update failed", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        finally
        {
            try { File.Delete(pack); } catch { }
        }
    }

    UpdateResult ApplyPack(string packPath)
    {
        using var pack = PayloadSource.OpenFile(packPath);
        var manifest = ReleaseManifest.From(pack)
            ?? throw new InvalidOperationException("The update pack has no manifest and cannot be trusted.");
        var job = new UpdateJob(install);
        var plan = job.Plan(manifest, pack);
        if (plan.Missing.Count > 0)
        {
            throw new InvalidOperationException(
                "This pack cannot update your install - it is missing "
                + string.Join(", ", plan.Missing.Take(3))
                + (plan.Missing.Count > 3 ? $" and {plan.Missing.Count - 3} more" : "")
                + ".\r\n\r\nDownload the full installer from the release page instead.");
        }
        string? locked = job.WhatIsLocked(plan);
        if (locked is not null)
            throw new InvalidOperationException($"{locked} is in use. Close the game and try again.");

        return job.Apply(plan, pack,
            (fraction, text) => BeginInvoke(() => Set(0.5 + 0.5 * fraction, text)), cts.Token);
    }

    void Finish(UpdateResult result)
    {
        working = false;
        Set(1.0, $"Updated to {result.To}.");

        var lines = new List<string>
        {
            $"Updated from {result.From} to {result.To}.",
            $"{result.Replaced.Count} file(s) replaced. The previous ones are in backup\\{result.From}.",
        };
        if (result.ModsReapplied is not null) lines.Add("Mods: " + result.ModsReapplied);
        if (result.KeptFiles.Count > 0)
        {
            lines.Add("Left as you changed them: " + string.Join(", ", result.KeptFiles.Take(3))
                      + (result.KeptFiles.Count > 3 ? $" and {result.KeptFiles.Count - 3} more" : "")
                      + ".");
        }
        if (result.KeptSettings.Count > 0)
            lines.Add("Left as you set them: " + string.Join(", ", result.KeptSettings));
        if (result.SelfUpdate is not null)
        {
            lines.Add("The updater itself was updated; the new copy takes over when this window closes.");
            UpdateJob.DeferSelfReplace(result.SelfUpdate.Value.Staging, result.SelfUpdate.Value.Dest);
        }

        heading.Text = "Update complete";
        notes.Visible = true;
        notes.Bounds = new Rectangle(20, 148, 520, 154);
        notes.Text = string.Join("\r\n\r\n", lines);
        skip.Visible = update.Visible = false;
        later.Text = "Close";
        later.Enabled = true;

        if (launch.Checked)
        {
            string game = Path.Combine(install.Paths.InstallDir, "legodimensions.exe");
            if (File.Exists(game)) Open(game);
        }
    }

    void Set(double fraction, string text)
    {
        progress.Value = Math.Clamp((int)(fraction * 100), 0, 100);
        status.Text = text;
    }

    static void Open(string target)
    {
        try { Process.Start(new ProcessStartInfo(target) { UseShellExecute = true }); }
        catch (Exception e) { Debug.WriteLine("open " + target + ": " + e); }
    }
}
