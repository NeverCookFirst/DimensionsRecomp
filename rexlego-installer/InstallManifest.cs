using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace RecompSetup;

/// <summary>
/// install.json, written into the install folder at the end of an install and
/// rewritten by the updater. It is the only record of what we put there, so it
/// answers the three questions nothing else can:
///
///   - is this folder one of our installs, and at what version;
///   - which files are ours (safe to replace, safe to delete on uninstall) -
///     everything else in the folder is the user's game dump, saves or mods;
///   - what values we last wrote into legodimensions.toml, so an update can
///     tell "still our default" from "the user changed it".
/// </summary>
public sealed class InstallManifest
{
    public const string FileName = "install.json";

    public string Product { get; set; } = "DimensionsRecompiled";
    public string Version { get; set; } = "0.0.0";
    public string InstalledUtc { get; set; } = "";
    public string UpdatedUtc { get; set; } = "";
    public InstalledComponents Components { get; set; } = new();
    public InstalledPaths Paths { get; set; } = new();
    /// <summary>Every file we laid down, keyed by its payload path so an update can match it up.</summary>
    public List<InstalledFile> Files { get; set; } = new();
    /// <summary>Key -> value, exactly as we last wrote it into legodimensions.toml.</summary>
    public Dictionary<string, string> TomlWritten { get; set; } = new();
    /// <summary>DLC package folder names, so nothing has to rescan tens of GB to know.</summary>
    public List<string> Dlc { get; set; } = new();
    /// <summary>owner/repo the updater asks for releases. Empty means "do not check".</summary>
    public string UpdateRepo { get; set; } = "";
    /// <summary>Version the user told the updater to stop offering.</summary>
    public string SkippedVersion { get; set; } = "";

    static readonly JsonSerializerOptions Json = new()
    {
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        // build-installer.ps1 writes manifest.json from PowerShell, whose
        // ConvertTo-Json keeps whatever casing it is handed.
        PropertyNameCaseInsensitive = true,
    };

    public static string PathIn(string installDir) => Path.Combine(installDir, FileName);

    public static InstallManifest? Load(string installDir)
    {
        try
        {
            string p = PathIn(installDir);
            if (!File.Exists(p)) return null;
            return JsonSerializer.Deserialize<InstallManifest>(File.ReadAllText(p), Json);
        }
        catch { return null; }
    }

    public void Save(string installDir)
    {
        File.WriteAllText(PathIn(installDir), JsonSerializer.Serialize(this, Json), new UTF8Encoding(false));
    }

    public InstalledFile? Find(string payloadPath) =>
        Files.FirstOrDefault(f => f.Payload.Equals(payloadPath, StringComparison.OrdinalIgnoreCase));

    /// <summary>True when the component this payload path belongs to was installed.</summary>
    public bool WantsPayload(string payloadPath)
    {
        string prefix = payloadPath.Split('/')[0];
        return prefix switch
        {
            "mods" or "modcli" => Components.Mods,
            // The Russian translation is an optional component. Without this case
            // it fell through to the catch-all below and every install that did
            // not pick it was told the update pack was broken.
            "rus" => Components.Russian,
            "toypad" => Components.Toypad,
            "saveconverter" => Components.SaveConverter,
            _ => true,               // game/, updater/ and the manifest always apply
        };
    }

    public static string Sha256(string path)
    {
        using var f = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(f)).ToLowerInvariant();
    }
}

public sealed class InstalledComponents
{
    public bool Mods { get; set; }
    /// <summary>The community Russian translation, installed and switched on.</summary>
    public bool Russian { get; set; }
    public bool Toypad { get; set; }
    public bool SaveConverter { get; set; }
    public bool Updater { get; set; }
}

/// <summary>
/// Absolute paths, so the updater does not have to re-derive the layout and a
/// future release can move things without stranding older installs.
/// </summary>
public sealed class InstalledPaths
{
    public string InstallDir { get; set; } = "";
    public string GameDataRoot { get; set; } = "";
    public string UpdateDataRoot { get; set; } = "";
    public string ModsUpdateRoot { get; set; } = "";
    public string ContentRoot { get; set; } = "";
    public string ModsRoot { get; set; } = "";
    public string ToolsRoot { get; set; } = "";
}

/// <summary>
/// One file we own. <see cref="Path"/> is relative to the install folder,
/// <see cref="Payload"/> is where it came from in the payload.
/// </summary>
public sealed class InstalledFile
{
    public string Path { get; set; } = "";
    public string Payload { get; set; } = "";
    public string Sha { get; set; } = "";
    public long Len { get; set; }
}
