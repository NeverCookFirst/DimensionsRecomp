using System.Diagnostics;

namespace RecompSetup;

/// <summary>
/// The wizard. Plain WinForms built in code, no designer: a title strip, a
/// page panel, Back / Next / Cancel. Pages validate on Next and after Browse,
/// so a tester cannot install with a wrong disc or the wrong Title Update.
/// </summary>
public sealed class WizardForm : Form
{
    public const string AppName = "Dimensions Recompiled";

    enum Page { Welcome, Game, Update, Dlc, Components, Location, Installing, Done }

    readonly InstallOptions opts = new();
    readonly PayloadSource payload;
    List<DlcSource> dlc = new();
    Page page = Page.Welcome;

    readonly Label title = new();
    readonly Panel content = new();
    readonly Button back = new() { Text = "< Back" };
    readonly Button next = new() { Text = "Next >" };
    readonly Button cancel = new() { Text = "Cancel" };

    // page widgets that need reading later
    CheckBox? welcomeAck;
    TextBox? gameBox, updateBox, dlcBox, installBox;
    Label? gameStatus, updateStatus, dlcStatus, installStatus;
    ListBox? dlcList;
    CheckBox? cbToypad, cbMods, cbSaveConv, cbUpdater, cbShortcut;
    ProgressBar? progress;
    Label? progressLabel;
    CancellationTokenSource? installCts;
    string? readmePath;
    long requiredBytes = -1;

    static readonly Font BodyFont = new("Segoe UI", 9.75f);
    static readonly Font TitleFont = new("Segoe UI Semibold", 13f);

    public WizardForm(PayloadSource payload)
    {
        this.payload = payload;
        Text = AppName + " - Setup";
        Font = BodyFont;
        ClientSize = new Size(640, 470);
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

        title.Font = TitleFont;
        title.Bounds = new Rectangle(0, 0, 640, 48);
        title.Padding = new Padding(20, 0, 0, 0);
        title.TextAlign = ContentAlignment.MiddleLeft;
        title.BackColor = Color.White;

        content.Bounds = new Rectangle(0, 48, 640, 362);

        var sep = new Label { Bounds = new Rectangle(0, 410, 640, 2), BorderStyle = BorderStyle.Fixed3D };
        back.Bounds = new Rectangle(360, 425, 85, 30);
        next.Bounds = new Rectangle(450, 425, 85, 30);
        cancel.Bounds = new Rectangle(545, 425, 85, 30);
        back.Click += (_, _) => Go(-1);
        next.Click += (_, _) => Go(+1);
        cancel.Click += (_, _) => OnCancel();
        FormClosing += (_, e) => { if (page == Page.Installing) { e.Cancel = true; OnCancel(); } };

        Controls.AddRange(new Control[] { title, content, sep, back, next, cancel });
        AcceptButton = next;
        Show(Page.Welcome);
    }

    // ---- navigation -------------------------------------------------------

    void Go(int dir)
    {
        if (dir > 0)
        {
            string? err = ValidateCurrent();
            if (err is not null)
            {
                MessageBox.Show(this, err, "Cannot continue", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (page == Page.Done) { Finish(); return; }
        }
        var target = (Page)((int)page + dir);
        Show(target);
        if (target == Page.Installing) _ = RunInstallAsync();
    }

    void Show(Page p)
    {
        page = p;
        content.Controls.Clear();
        back.Enabled = p is not (Page.Welcome or Page.Installing or Page.Done);
        next.Enabled = p != Page.Installing;
        cancel.Enabled = p != Page.Done;
        next.Text = p switch { Page.Location => "Install", Page.Done => "Finish", _ => "Next >" };
        switch (p)
        {
            case Page.Welcome: BuildWelcome(); break;
            case Page.Game: BuildGame(); break;
            case Page.Update: BuildUpdate(); break;
            case Page.Dlc: BuildDlc(); break;
            case Page.Components: BuildComponents(); break;
            case Page.Location: BuildLocation(); break;
            case Page.Installing: BuildInstalling(); break;
            case Page.Done: BuildDone(); break;
        }
    }

    string? ValidateCurrent() => page switch
    {
        Page.Welcome => welcomeAck!.Checked ? null : "Please confirm that you understand this is an unstable test build.",
        Page.Game => ValidateGame(),
        Page.Update => ValidateUpdate(),
        Page.Dlc => ValidateDlc(),
        Page.Components => ReadComponents(),
        Page.Location => ValidateLocation(),
        _ => null,
    };

    void OnCancel()
    {
        if (page == Page.Installing)
        {
            if (MessageBox.Show(this, "Stop the installation? Files copied so far will be left in the install folder.",
                    "Cancel", MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes)
                installCts?.Cancel();
            return;
        }
        Close();
    }

    // ---- helpers ------------------------------------------------------------

    Label Text_(string text, int y, int h = 60) => new()
    {
        Text = text, Bounds = new Rectangle(20, y, 600, h), AutoSize = false,
    };

    (TextBox box, Label status) PathRow(int y, string browseText, Action<TextBox> browse, string? browse2Text = null, Action<TextBox>? browse2 = null)
    {
        var box = new TextBox { Bounds = new Rectangle(20, y, browse2 is null ? 495 : 380, 26) };
        var b1 = new Button { Text = browseText, Bounds = new Rectangle(browse2 is null ? 520 : 405, y - 1, browse2 is null ? 100 : 105, 28) };
        b1.Click += (_, _) => browse(box);
        content.Controls.AddRange(new Control[] { box, b1 });
        if (browse2 is not null)
        {
            var b2 = new Button { Text = browse2Text, Bounds = new Rectangle(515, y - 1, 105, 28) };
            b2.Click += (_, _) => browse2(box);
            content.Controls.Add(b2);
        }
        var status = new Label { Bounds = new Rectangle(20, y + 34, 600, 70), ForeColor = Color.DimGray };
        content.Controls.Add(status);
        return (box, status);
    }

    static void PickFolder(TextBox box, string description)
    {
        using var d = new FolderBrowserDialog { Description = description, UseDescriptionForTitle = true, ShowNewFolderButton = false };
        if (Directory.Exists(box.Text)) d.InitialDirectory = box.Text;
        if (d.ShowDialog() == DialogResult.OK) box.Text = d.SelectedPath;
    }

    static void PickFile(TextBox box, string titleText)
    {
        using var d = new OpenFileDialog { Title = titleText, Filter = "All files|*.*", CheckFileExists = true };
        if (d.ShowDialog() == DialogResult.OK) box.Text = d.FileName;
    }

    static void SetStatus(Label l, string? error, string ok)
    {
        l.ForeColor = error is null ? Color.ForestGreen : Color.Firebrick;
        l.Text = error ?? ok;
    }

    static string Gb(long bytes) => $"{bytes / 1024.0 / 1024 / 1024:0.0} GB";

    // ---- pages --------------------------------------------------------------

    void BuildWelcome()
    {
        title.Text = "Welcome";
        content.Controls.Add(Text_(
            $"This will set up {AppName} - the LEGO Dimensions recompilation - on your PC.\n\n" +
            "You will need:\n" +
            "   -  your own dump of the Xbox 360 disc, extracted to a folder (Default.xex, GAME.DAT ...)\n" +
            "   -  Title Update 23, as a folder or as the original package file\n" +
            "   -  optionally, your DLC packages\n\n" +
            "This installer contains no game data. It copies your files into a folder you choose and adds the " +
            "recompiled executable, its settings, and optional tools. About 10 GB of free space is needed, " +
            "more with DLC.", 20, 190));

        var warn = Text_(
            "THIS IS AN EARLY TEST BUILD. It is not stable: expect crashes, graphical glitches and bugs. " +
            "Nothing here is affiliated with LEGO, TT Games or Warner Bros.", 215, 60);
        warn.ForeColor = Color.Firebrick;
        content.Controls.Add(warn);

        welcomeAck = new CheckBox { Text = "I understand this is an unstable test build and I expect bugs.", Bounds = new Rectangle(20, 285, 600, 26) };
        content.Controls.Add(welcomeAck);
    }

    void BuildGame()
    {
        title.Text = "Step 1 of 5 - Game files";
        content.Controls.Add(Text_(
            "Select the folder with your extracted LEGO Dimensions disc. It must contain Default.xex and the GAME*.DAT / .HDR files. " +
            "An ISO will not work - extract it first (for example with xenia's dump tool or extract-xiso).\n\n" +
            "Only the exact disc build the recomp was made from is accepted; the installer checks Default.xex.", 20, 110));
        (gameBox, gameStatus) = PathRow(140, "Browse...", b => { PickFolder(b, "Select the extracted game folder"); ValidateGame(); });
        gameBox.Text = opts.GameDir;
        gameBox.Leave += (_, _) => ValidateGame();
        if (opts.GameDir != "") ValidateGame();
    }

    string? ValidateGame()
    {
        opts.GameDir = gameBox!.Text.Trim().Trim('"');
        string? err = Validation.CheckGameDir(opts.GameDir, out var info);
        SetStatus(gameStatus!, err, info is null ? "" :
            $"OK - LEGO Dimensions, title {info.TitleId:X8}, media {info.MediaId:X8}, version {info.VersionString}");
        return err;
    }

    void BuildUpdate()
    {
        title.Text = "Step 2 of 5 - Title Update 23";
        content.Controls.Add(Text_(
            "Title Update 23 is required - the recomp was built from the patched executable and will not run without it.\n\n" +
            "Point to either:\n" +
            "   -  a folder with the extracted update (Default.xexp, PATCH.DAT, PATCH.HDR ...), or\n" +
            "   -  the original package file downloaded from the console (usually named tu00000003_00000000 " +
            "or similar, no extension). The installer will extract it.", 20, 130));
        (updateBox, updateStatus) = PathRow(160, "Folder...", b => { PickFolder(b, "Select the extracted Title Update folder"); ValidateUpdate(); },
                                            "Package file...", b => { PickFile(b, "Select the Title Update package"); ValidateUpdate(); });
        updateBox.Text = opts.UpdatePath;
        updateBox.Leave += (_, _) => ValidateUpdate();
        if (opts.UpdatePath != "") ValidateUpdate();
    }

    string? ValidateUpdate()
    {
        opts.UpdatePath = updateBox!.Text.Trim().Trim('"');
        updateStatus!.ForeColor = Color.DimGray; updateStatus.Text = "Checking..."; updateStatus.Refresh();
        string? err = Validation.CheckUpdate(opts.UpdatePath, out var desc);
        SetStatus(updateStatus, err, "OK - " + desc);
        return err;
    }

    void BuildDlc()
    {
        title.Text = "Step 3 of 5 - DLC (optional)";
        content.Controls.Add(Text_(
            "If you have DLC, select the folder that holds the packages. Both forms are accepted and can be mixed: " +
            "extracted folders (each with spa.bin and DLCnn.DAT2) and original package files. " +
            "Anything that is not LEGO Dimensions DLC is ignored.\n\n" +
            "Leave this empty if you have none - the game runs without DLC.", 20, 90));
        (dlcBox, dlcStatus) = PathRow(120, "Browse...", b => { PickFolder(b, "Select the folder containing your DLC packages"); ValidateDlc(); });
        dlcStatus.Bounds = new Rectangle(20, 154, 600, 24);
        dlcBox.Text = opts.DlcPath ?? "";
        dlcBox.Leave += (_, _) => ValidateDlc();
        dlcList = new ListBox { Bounds = new Rectangle(20, 182, 600, 150), IntegralHeight = false };
        content.Controls.Add(dlcList);
        ValidateDlc();
    }

    string? ValidateDlc()
    {
        opts.DlcPath = dlcBox!.Text.Trim().Trim('"');
        if (opts.DlcPath == "") opts.DlcPath = null;
        if (opts.DlcPath is not null && !Directory.Exists(opts.DlcPath))
        {
            SetStatus(dlcStatus!, "That folder does not exist.", "");
            return "The DLC folder does not exist. Clear the field to skip DLC.";
        }

        var found = Validation.ScanDlc(opts.DlcPath);
        // The Complete Pack disc dump ships one DLC inside the game folder itself.
        var bundled = Validation.ScanDlc(Path.Combine(opts.GameDir, "5752084B", "00000002"));
        foreach (var b in bundled)
            if (!found.Any(f => f.Name.Equals(b.Name, StringComparison.OrdinalIgnoreCase))) found.Add(b);
        dlc = found.OrderBy(d => d.DisplayName).ToList();

        dlcList!.Items.Clear();
        foreach (var d in dlc) dlcList.Items.Add($"{d.DisplayName}   ({(d.IsPackageFile ? "package" : "folder")}, {d.Bytes / 1024 / 1024} MB)");
        dlcStatus!.ForeColor = dlc.Count == 0 ? Color.DimGray : Color.ForestGreen;
        dlcStatus.Text = dlc.Count == 0
            ? "No DLC selected."
            : $"{dlc.Count} DLC package(s) found, {Gb(dlc.Sum(d => d.Bytes))} total" + (bundled.Count > 0 ? $" ({bundled.Count} of them inside the game folder)." : ".");
        return null;
    }

    void BuildComponents()
    {
        title.Text = "Step 4 of 5 - Components";
        content.Controls.Add(Text_("Choose what to install alongside the game.", 20, 30));
        int y = 60;
        CheckBox Row(string text, string desc, bool checked_, bool available)
        {
            var cb = new CheckBox { Text = text, Bounds = new Rectangle(20, y, 600, 26), Checked = checked_ && available, Enabled = available };
            var d = new Label { Text = available ? desc : desc + "  (not included in this installer build)", Bounds = new Rectangle(40, y + 26, 580, 40), ForeColor = Color.DimGray };
            content.Controls.AddRange(new Control[] { cb, d });
            y += 72;
            return cb;
        }
        cbToypad = Row("LEGO Toypad app  (recommended)",
            "Emulates the USB Toy Pad so you can place figures. Without it the game cannot be played. By harrysof, MIT licence.",
            opts.IncludeToypad, payload.HasToypad);
        cbMods = Row("Mods and the in-game mod menu (F8)  (recommended)",
            "Two small mods, both switched off by default, and the tool that applies them. Also prepares a mod-ready copy of the update (about 850 MB extra).",
            opts.IncludeMods, payload.HasMods);
        cbSaveConv = Row("Save converter",
            "Converts saves from xenia or a real console for use here. Only for people who already have a save to bring over.",
            opts.IncludeSaveConverter, payload.HasSaveConverter);
        cbUpdater = Row("Automatic update checks  (recommended)",
            "Looks for a new release when the game starts and offers to install just the files that changed. "
            + "Can be switched off any time in the F4 menu, under Updates.",
            opts.IncludeUpdater, true);
        cbShortcut = new CheckBox { Text = "Create a desktop shortcut", Bounds = new Rectangle(20, y, 600, 26), Checked = opts.DesktopShortcut };
        content.Controls.Add(cbShortcut);
    }

    string? ReadComponents()
    {
        opts.IncludeToypad = cbToypad!.Checked;
        opts.IncludeMods = cbMods!.Checked;
        opts.IncludeSaveConverter = cbSaveConv!.Checked;
        opts.IncludeUpdater = cbUpdater!.Checked;
        opts.DesktopShortcut = cbShortcut!.Checked;
        return null;
    }

    void BuildLocation()
    {
        title.Text = "Step 5 of 5 - Install location";
        content.Controls.Add(Text_(
            "Choose where to install. Everything - game data, update, DLC, saves and settings - goes into this one folder, " +
            "so it can be moved or deleted as a unit. Avoid Program Files: the game writes its settings and saves next to itself.", 20, 70));
        if (opts.InstallDir == "")
            opts.InstallDir = Path.Combine(Path.GetPathRoot(Environment.SystemDirectory) ?? @"C:\", "Games", AppName);
        (installBox, installStatus) = PathRow(100, "Browse...", b =>
        {
            using var d = new FolderBrowserDialog { Description = "Select the install folder", UseDescriptionForTitle = true, ShowNewFolderButton = true };
            if (d.ShowDialog() == DialogResult.OK) b.Text = d.SelectedPath;
            UpdateSpace();
        });
        installBox.Text = opts.InstallDir;
        installBox.Leave += (_, _) => UpdateSpace();
        installStatus.Bounds = new Rectangle(20, 134, 600, 50);

        var summary = new Label { Bounds = new Rectangle(20, 195, 600, 140), ForeColor = Color.DimGray };
        summary.Text =
            $"Game:            {opts.GameDir}\n" +
            $"Title Update:    {opts.UpdatePath}\n" +
            $"DLC:             {(dlc.Count == 0 ? "none" : dlc.Count + " package(s)")}\n" +
            $"Components:      {string.Join(", ", new[] {
                opts.IncludeToypad ? "Toypad app" : null, opts.IncludeMods ? "mods" : null,
                opts.IncludeSaveConverter ? "save converter" : null, opts.DesktopShortcut ? "shortcut" : null }.Where(s => s is not null))}\n\n" +
            "Click Install to begin. Copying takes a few minutes.";
        content.Controls.Add(summary);
        UpdateSpace();
    }

    void UpdateSpace()
    {
        opts.InstallDir = installBox!.Text.Trim().Trim('"');
        installStatus!.ForeColor = Color.DimGray;
        installStatus.Text = "Calculating space...";
        var o = opts; var d = dlc;
        Task.Run(() => InstallJob.EstimateBytes(o, payload, d)).ContinueWith(t =>
        {
            if (page != Page.Location || t.IsFaulted) return;
            requiredBytes = t.Result;
            string root = "";
            long free = -1;
            try { root = Path.GetPathRoot(Path.GetFullPath(opts.InstallDir)) ?? ""; free = new DriveInfo(root).AvailableFreeSpace; } catch { }
            bool ok = free < 0 || free > requiredBytes * 1.05;
            installStatus.ForeColor = ok ? Color.ForestGreen : Color.Firebrick;
            installStatus.Text = $"Required: about {Gb(requiredBytes)}.   Free on {root}: {(free < 0 ? "unknown" : Gb(free))}."
                + (ok ? "" : "  Not enough space.");
        }, TaskScheduler.FromCurrentSynchronizationContext());
    }

    string? ValidateLocation()
    {
        opts.InstallDir = installBox!.Text.Trim().Trim('"');
        if (opts.InstallDir == "") return "Choose an install folder.";
        try { opts.InstallDir = Path.GetFullPath(opts.InstallDir).TrimEnd('\\'); }
        catch { return "That is not a valid folder path."; }
        string pf = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
        string pf86 = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);
        if (opts.InstallDir.StartsWith(pf, StringComparison.OrdinalIgnoreCase) || opts.InstallDir.StartsWith(pf86, StringComparison.OrdinalIgnoreCase))
            return "Please do not install into Program Files - the game needs to write its settings and saves next to the executable.";
        string game = Path.GetFullPath(opts.GameDir).TrimEnd('\\');
        if (game.StartsWith(opts.InstallDir + "\\", StringComparison.OrdinalIgnoreCase) || game.Equals(opts.InstallDir, StringComparison.OrdinalIgnoreCase)
            || opts.InstallDir.StartsWith(game + "\\", StringComparison.OrdinalIgnoreCase))
            return "The install folder must not be inside the game folder, or the game folder inside it.";
        if (Directory.Exists(opts.InstallDir) && Directory.EnumerateFileSystemEntries(opts.InstallDir).Any()
            && MessageBox.Show(this, "The folder is not empty. Files with the same names will be overwritten. Continue?",
                "Folder not empty", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
            return "Choose an empty folder.";
        try
        {
            long free = new DriveInfo(Path.GetPathRoot(opts.InstallDir)!).AvailableFreeSpace;
            if (requiredBytes > 0 && free < requiredBytes * 1.05)
                return $"Not enough free space: about {Gb(requiredBytes)} needed, {Gb(free)} available.";
        }
        catch { /* unknown drive type - let the copy itself complain */ }
        return null;
    }

    void BuildInstalling()
    {
        title.Text = "Installing";
        progressLabel = new Label { Bounds = new Rectangle(20, 120, 600, 48), Text = "Preparing..." };
        progress = new ProgressBar { Bounds = new Rectangle(20, 175, 600, 24), Maximum = 1000 };
        content.Controls.AddRange(new Control[] { progressLabel, progress });
    }

    async Task RunInstallAsync()
    {
        installCts = new CancellationTokenSource();
        var reporter = new Progress<InstallProgress>(p =>
        {
            if (progress is null || page != Page.Installing) return;
            progress.Value = Math.Clamp((int)(p.Fraction * 1000), 0, 1000);
            progressLabel!.Text = p.Status;
        });
        var job = new InstallJob(opts, payload, reporter, installCts.Token);
        try
        {
            await job.RunAsync(dlc);
            readmePath = job.ReadmePath;
            Show(Page.Done);
        }
        catch (OperationCanceledException)
        {
            Show(Page.Location);
        }
        catch (Exception e)
        {
            MessageBox.Show(this, "Installation failed:\n\n" + e.Message + "\n\nNothing has been removed; you can fix the problem and click Install again.",
                "Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
            Show(Page.Location);
        }
    }

    void BuildDone()
    {
        title.Text = "Finished";
        content.Controls.Add(Text_(
            $"{AppName} is installed.\n\n" +
            $"Folder:   {opts.InstallDir}\n\n" +
            "Click Finish to open the README. Please read it - it explains the hotkeys, the settings, how the Toypad app " +
            "works, and the known problems with saves.\n\n" +
            "Remember: this is a test build. When it breaks, grab game.log from the install folder and report what you were doing.", 20, 220));
    }

    void Finish()
    {
        if (readmePath is not null && File.Exists(readmePath))
        {
            try { Process.Start(new ProcessStartInfo(readmePath) { UseShellExecute = true }); }
            catch { /* no text viewer associated - the file is still there */ }
        }
        Close();
    }
}
