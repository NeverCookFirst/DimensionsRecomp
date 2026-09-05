using System.Text.Json;
using System.Text.Json.Serialization;

namespace RecompSetup;

/// <summary>
/// manifest.json, the first entry of every payload we ship - both the one
/// appended to Setup.exe and the small "update-x.y.z.rxp" packs attached to a
/// release. It is written by build-installer.ps1, read by the installer (to
/// stamp the install) and by the updater (to decide what to replace).
///
/// <para>
/// <see cref="Files"/> always lists the COMPLETE file set of the release, even
/// in an update pack that carries only a few of them. That is what lets the
/// updater notice "this pack cannot bring you up to date, run the full
/// installer" instead of leaving a half-updated folder behind.
/// </para>
/// </summary>
public sealed class ReleaseManifest
{
    public string Product { get; set; } = "DimensionsRecompiled";
    public string Version { get; set; } = "0.0.0";
    public string ReleasedUtc { get; set; } = "";
    /// <summary>Human-readable summary shown in the updater. The release body on GitHub wins over this.</summary>
    public string Notes { get; set; } = "";
    /// <summary>
    /// Oldest install this pack can update. An install older than this is missing
    /// files the pack does not carry, so the updater sends the user to Setup.exe.
    /// Empty in a full payload, which can update anything.
    /// </summary>
    public string MinVersion { get; set; } = "";
    /// <summary>True for the payload inside Setup.exe, false for an update pack.</summary>
    public bool Full { get; set; }
    public List<ReleaseFile> Files { get; set; } = new();
    /// <summary>Payload-relative paths retired in this release; the updater deletes them.</summary>
    public List<string> Removed { get; set; } = new();
    /// <summary>Config keys this release needs. See <see cref="TomlConfig"/> for how they are applied.</summary>
    public TomlChanges Toml { get; set; } = new();

    public ReleaseFile? Find(string payloadPath) =>
        Files.FirstOrDefault(f => f.P.Equals(payloadPath, StringComparison.OrdinalIgnoreCase));

    public const string EntryName = "manifest.json";

    static readonly JsonSerializerOptions Json = new()
    {
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        // build-installer.ps1 writes manifest.json from PowerShell, whose
        // ConvertTo-Json keeps whatever casing it is handed.
        PropertyNameCaseInsensitive = true,
    };

    public string ToJson() => JsonSerializer.Serialize(this, Json);

    public static ReleaseManifest? Parse(string json)
    {
        try { return JsonSerializer.Deserialize<ReleaseManifest>(json, Json); }
        catch { return null; }
    }

    /// <summary>
    /// The manifest of a payload, or null for one built before manifests existed.
    /// Callers must cope: the 0.1.x testers have no manifest anywhere.
    /// </summary>
    public static ReleaseManifest? From(PayloadSource payload)
    {
        var entry = payload.Find(EntryName);
        if (entry is null) return null;
        string tmp = Path.Combine(Path.GetTempPath(), "rex-manifest-" + Guid.NewGuid().ToString("N") + ".json");
        try
        {
            payload.CopyTo(entry, tmp, null);
            return Parse(File.ReadAllText(tmp));
        }
        catch { return null; }
        finally { try { File.Delete(tmp); } catch { } }
    }

    /// <summary>
    /// Compares dotted numeric versions ("1.2.3"). Anything unparseable sorts
    /// lowest, so a mangled version never blocks an update.
    /// </summary>
    public static int CompareVersions(string a, string b)
    {
        static int[] Parts(string v) =>
            v.Split('.', StringSplitOptions.RemoveEmptyEntries)
             .Select(p => int.TryParse(new string(p.TakeWhile(char.IsDigit).ToArray()), out int n) ? n : 0)
             .ToArray();
        int[] x = Parts(a), y = Parts(b);
        for (int i = 0; i < Math.Max(x.Length, y.Length); i++)
        {
            int xi = i < x.Length ? x[i] : 0, yi = i < y.Length ? y[i] : 0;
            if (xi != yi) return xi.CompareTo(yi);
        }
        return 0;
    }
}

/// <summary>One file in a release. <see cref="P"/> is payload-relative, '/'-separated.</summary>
public sealed class ReleaseFile
{
    public string P { get; set; } = "";
    public string Sha { get; set; } = "";
    public long Len { get; set; }
}

/// <summary>
/// What a release wants in legodimensions.toml.
///
///   Forced   - compatibility switches the build cannot run without. Always
///              rewritten, because a new build may need different values.
///   Defaults - our recommended settings. Written on a fresh install, and on an
///              update only when the user has not changed them since (compared
///              against what we recorded in install.json).
///   Removed  - keys that no longer exist; dropped from the file.
/// </summary>
public sealed class TomlChanges
{
    public Dictionary<string, string> Forced { get; set; } = new();
    public Dictionary<string, string> Defaults { get; set; } = new();
    public List<string> Removed { get; set; } = new();
}
