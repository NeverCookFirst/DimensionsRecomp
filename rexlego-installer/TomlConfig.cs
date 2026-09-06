using System.Text;

namespace RecompSetup;

/// <summary>One line we want in legodimensions.toml.</summary>
/// <param name="Key">cvar name.</param>
/// <param name="Value">The value as TOML text - already quoted if it is a string.</param>
/// <param name="Forced">
/// True for compatibility switches the build cannot run without: an update
/// rewrites them even if the user changed them, because a new build may need
/// different values. False for our recommended settings, which an update leaves
/// alone once the user has touched them.
/// </param>
/// <param name="Section">Comment written above the first key of each group.</param>
public readonly record struct TomlItem(string Key, string Value, bool Forced, string? Section = null);

/// <summary>
/// Everything that knows about legodimensions.toml. The installer renders a
/// fresh file from <see cref="Plan"/>; the updater merges the same plan into
/// whatever is there now.
///
/// <para>
/// The merge has to survive the game: on exit the runtime rewrites this file
/// from its cvar registry (cvar.cpp SerializeToTOML), which drops every comment,
/// reorders the keys, re-quotes the strings and omits anything still at its
/// built-in default. So the merge works line by line on whatever it finds and
/// compares parsed values, never raw text.
/// </para>
/// </summary>
public static class TomlConfig
{
    public const string FileName = "legodimensions.toml";

    /// <summary>TOML literal string: single quotes, no escapes, so Windows paths go in as-is.</summary>
    public static string Literal(string s) => "'" + s.Replace("'", "") + "'";

    /// <summary>
    /// The full set of keys we manage, in the order they are written. Values
    /// that depend on the install (paths, which components exist) come from the
    /// arguments; everything else is the same for everybody.
    /// </summary>
    public static List<TomlItem> Plan(InstalledPaths p, InstalledComponents c, string updateRepo, bool hasGamepadDb)
    {
        var items = new List<TomlItem>();
        void Add(string section, string key, string value, bool forced = false) =>
            items.Add(new TomlItem(key, value, forced, section));

        Add("Paths", "game_data_root", Literal(p.GameDataRoot));
        Add("Paths", "update_data_root", Literal(p.UpdateDataRoot));
        Add("Paths", "user_data_root", Literal(p.ContentRoot));
        Add("Paths", "log_file", Literal(Path.Combine(p.InstallDir, "game.log")));
        if (hasGamepadDb)
        {
            // The default is relative to the working directory, which is not
            // necessarily the install folder.
            Add("Paths", "hid_mappings_file", Literal(Path.Combine(p.InstallDir, PayloadSource.GamepadDb)));
        }

        const string modsSection = "Mods (F8). All off by default.";
        if (c.Mods)
        {
            Add(modsSection, "mods", "''");
            Add(modsSection, "mods_root", Literal(p.ModsRoot));
            Add(modsSection, "mods_update_root", Literal(p.ModsUpdateRoot));
            Add(modsSection, "modcli_path", Literal(Path.Combine(p.ToolsRoot, "modcli", "modcli.exe")));
        }

        const string updatesSection = "Updates. Turn updates_check off (F4 -> Updates) to never look online.";
        if (c.Updater)
        {
            Add(updatesSection, "updates_check", "true");
            // Here so a release can be redirected without shipping a new updater.
            Add(updatesSection, "updates_repo", Literal(updateRepo));
            Add(updatesSection, "updater_path", Literal(Path.Combine(p.ToolsRoot, "rexupdate", "rexupdate.exe")));
        }

        // Not forced: a physical portal is a hardware choice, and anyone who
        // switches to one should keep that across updates.
        const string toypad = "ToyPad. Turn this off to use a real LEGO portal over USB\n"
                            + "# (needs the libusb driver, installed with Zadig).";
        Add(toypad, "toypad_emulation", "true");

        Add("Display", "fullscreen", "true");
        Add("Display", "vsync", "false");
        Add("Display", "framerate_limit", "60");
        Add("Display", "frame_rate", "'60'");
        Add("Display", "present_effect", "'fsr'");
        Add("Display", "present_fsr_max_upsampling_passes", "1");
        Add("Display", "anisotropic_override", "5");

        const string compat = "Compatibility - these keep the game from crashing on things the\n"
                            + "# recompiled code does not cover yet. Do not change them.";
        Add(compat, "invalid_function_nonfatal", "true", forced: true);
        Add(compat, "license_mask", "4294967295", forced: true);
        Add(compat, "gpu_plugin", "'xenos'", forced: true);
        Add(compat, "gpu_backend", "'d3d12'", forced: true);
        Add(compat, "gpu_allow_invalid_fetch_constants", "true", forced: true);
        Add(compat, "clear_memory_page_state", "false", forced: true);
        Add(compat, "readback_resolve", "'fast'", forced: true);
        Add(compat, "readback_resolve_half_pixel_offset", "true", forced: true);
        Add(compat, "readback_memexport_fast", "false", forced: true);
        Add(compat, "readback_resolve_max_kb", "256", forced: true);
        Add(compat, "d3d12_readback_memexport", "true", forced: true);
        Add(compat, "d3d12_readback_resolve", "true", forced: true);
        Add(compat, "execute_unclipped_draw_vs_on_cpu", "true", forced: true);
        Add(compat, "primitive_processor_cache_min_indices", "4096", forced: true);
        Add(compat, "non_seamless_cube_map", "true", forced: true);
        return items;
    }

    /// <summary>
    /// Folds a release's own key changes into a plan. A build that needs a
    /// different compatibility switch ships it in manifest.json rather than
    /// waiting for a new updater.
    /// </summary>
    public static List<TomlItem> Apply(List<TomlItem> plan, TomlChanges? changes)
    {
        if (changes is null) return plan;
        var result = new List<TomlItem>(plan);
        void Upsert(string key, string value, bool forced)
        {
            int at = result.FindIndex(i => i.Key == key);
            if (at >= 0) result[at] = result[at] with { Value = value, Forced = forced };
            else result.Add(new TomlItem(key, value, forced, "Added by this release"));
        }
        foreach (var (k, v) in changes.Forced) Upsert(k, v, true);
        foreach (var (k, v) in changes.Defaults) Upsert(k, v, false);
        result.RemoveAll(i => changes.Removed.Contains(i.Key));
        return result;
    }

    /// <summary>Every key/value a plan writes, for recording in install.json.</summary>
    public static Dictionary<string, string> Record(IEnumerable<TomlItem> plan) =>
        plan.ToDictionary(i => i.Key, i => i.Value, StringComparer.Ordinal);

    /// <summary>A fresh, commented file. Used on install.</summary>
    public static string Render(IEnumerable<TomlItem> plan, string appName)
    {
        var sb = new StringBuilder();
        sb.Append("# ").Append(appName).AppendLine(" - written by the installer.");
        sb.AppendLine("# The game rewrites this file on exit; press F4 in game to change most of it.");
        string? section = null;
        foreach (var item in plan)
        {
            if (item.Section != section)
            {
                section = item.Section;
                sb.AppendLine();
                if (section is not null) sb.Append("# ").AppendLine(section);
            }
            sb.Append(item.Key).Append(" = ").AppendLine(item.Value);
        }
        return sb.ToString();
    }

    /// <summary>
    /// Merges a plan into an existing file. Returns the new text and, through
    /// <paramref name="kept"/>, the keys left as the user set them.
    ///
    /// <para>
    /// A key is rewritten when it is <see cref="TomlItem.Forced"/>, when it is
    /// missing, or when its current value is still the one we wrote last time
    /// (<paramref name="previouslyWritten"/>). Anything else is the user's and
    /// stays. Keys we do not manage are untouched; comments and order survive.
    /// </para>
    /// </summary>
    public static string Merge(string existing, IEnumerable<TomlItem> plan,
                               IReadOnlyDictionary<string, string> previouslyWritten,
                               out List<string> kept)
    {
        var wanted = plan.ToList();
        var byKey = wanted.ToDictionary(i => i.Key, StringComparer.Ordinal);
        var seen = new HashSet<string>(StringComparer.Ordinal);
        kept = new List<string>();

        var lines = existing.Replace("\r\n", "\n").Split('\n').ToList();
        for (int n = 0; n < lines.Count; n++)
        {
            string line = lines[n];
            string trimmed = line.TrimStart();
            if (trimmed.Length == 0 || trimmed[0] == '#' || trimmed[0] == '[') continue;
            int eq = line.IndexOf('=');
            if (eq <= 0) continue;
            string key = line[..eq].Trim();
            if (!byKey.TryGetValue(key, out var item)) continue;
            seen.Add(key);

            string currentRaw = StripComment(line[(eq + 1)..]).Trim();
            bool userChanged = !previouslyWritten.TryGetValue(key, out string? was)
                               || !ValuesEqual(currentRaw, was);
            if (!item.Forced && userChanged)
            {
                kept.Add(key);
                continue;
            }
            if (ValuesEqual(currentRaw, item.Value)) continue;
            lines[n] = key + " = " + item.Value;
        }

        var missing = wanted.Where(i => !seen.Contains(i.Key)).ToList();
        if (missing.Count > 0)
        {
            if (lines.Count > 0 && lines[^1].Trim().Length != 0) lines.Add("");
            lines.Add("# Added by the updater");
            foreach (var item in missing) lines.Add(item.Key + " = " + item.Value);
            lines.Add("");
        }
        return string.Join(Environment.NewLine, lines);
    }

    /// <summary>
    /// Compares two TOML values by what they mean, not how they are written:
    /// the game re-quotes every string when it saves, so 'C:\x' comes back as
    /// "C:\\x" and a textual comparison would call that a user edit.
    /// </summary>
    public static bool ValuesEqual(string a, string b) =>
        string.Equals(ParseValue(a), ParseValue(b), StringComparison.Ordinal);

    /// <summary>Unquotes a TOML scalar. Bare values (numbers, booleans) come back as-is.</summary>
    public static string ParseValue(string raw)
    {
        raw = raw.Trim();
        if (raw.Length >= 2 && raw[0] == '\'' && raw[^1] == '\'') return raw[1..^1];
        if (raw.Length >= 2 && raw[0] == '"' && raw[^1] == '"')
        {
            var sb = new StringBuilder();
            for (int i = 1; i < raw.Length - 1; i++)
            {
                char ch = raw[i];
                if (ch != '\\' || i + 1 >= raw.Length - 1) { sb.Append(ch); continue; }
                char next = raw[++i];
                sb.Append(next switch { 'n' => '\n', 't' => '\t', 'r' => '\r', _ => next });
            }
            return sb.ToString();
        }
        return raw;
    }

    /// <summary>Drops a trailing comment, but not a '#' inside a quoted value.</summary>
    static string StripComment(string valuePart)
    {
        char quote = '\0';
        for (int i = 0; i < valuePart.Length; i++)
        {
            char ch = valuePart[i];
            if (quote != '\0') { if (ch == quote) quote = '\0'; continue; }
            if (ch == '\'' || ch == '"') { quote = ch; continue; }
            if (ch == '#') return valuePart[..i];
        }
        return valuePart;
    }
}
