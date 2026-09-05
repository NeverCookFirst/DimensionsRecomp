using System.Net.Http.Headers;
using System.Text.Json;

namespace RecompSetup;

/// <summary>One release as GitHub describes it, reduced to what we act on.</summary>
public sealed record ReleaseInfo(
    string Version,
    string Tag,
    string Title,
    string Notes,
    string PageUrl,
    string? PackUrl,
    long PackBytes,
    string? SetupUrl);

/// <summary>
/// The update feed: GitHub Releases, read through the public API.
///
/// <para>
/// A release is expected to carry two assets - "DimensionsRecompiled-Setup.exe"
/// for people installing from scratch, and "update-&lt;version&gt;.rxp" holding only
/// the files that changed since the previous release. The updater downloads the
/// second one; the first is what it points at when a pack cannot do the job.
/// </para>
///
/// <para>
/// While the repository is private every request needs a token, so one is read
/// from --token, the GITHUB_TOKEN environment variable, or a token.txt sitting
/// next to the updater. Once the repository is public none of that is needed.
/// </para>
/// </summary>
public static class UpdateFeed
{
    public const string PackExtension = ".rxp";
    public const string SetupAssetName = "DimensionsRecompiled-Setup.exe";

    static HttpClient NewClient(string? token)
    {
        var http = new HttpClient { Timeout = TimeSpan.FromMinutes(30) };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("DimensionsRecompiled-Updater/1.0");
        http.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
        http.DefaultRequestHeaders.Add("X-GitHub-Api-Version", "2022-11-28");
        if (!string.IsNullOrWhiteSpace(token))
            http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", token);
        return http;
    }

    /// <summary>Token from the argument, the environment, or token.txt next to the updater.</summary>
    public static string? ResolveToken(string? fromArgs)
    {
        if (!string.IsNullOrWhiteSpace(fromArgs)) return fromArgs.Trim();
        string? env = Environment.GetEnvironmentVariable("GITHUB_TOKEN");
        if (!string.IsNullOrWhiteSpace(env)) return env.Trim();
        try
        {
            string side = Path.Combine(AppContext.BaseDirectory, "token.txt");
            if (File.Exists(side)) return File.ReadAllText(side).Trim();
        }
        catch { }
        return null;
    }

    /// <summary>
    /// The newest release of <paramref name="repo"/> ("owner/name"), or null when
    /// there is none. Pre-releases count: every tester build is one.
    /// </summary>
    public static async Task<ReleaseInfo?> LatestAsync(string repo, string? token, CancellationToken ct)
    {
        using var http = NewClient(token);
        // /releases rather than /releases/latest, which hides pre-releases.
        string url = $"https://api.github.com/repos/{repo}/releases?per_page=10";
        using var response = await http.GetAsync(url, ct);
        response.EnsureSuccessStatusCode();
        using var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync(ct));

        foreach (var release in doc.RootElement.EnumerateArray())
        {
            if (release.TryGetProperty("draft", out var draft) && draft.GetBoolean()) continue;
            string tag = release.GetProperty("tag_name").GetString() ?? "";
            string? pack = null, setup = null;
            long packBytes = 0;
            if (release.TryGetProperty("assets", out var assets))
            {
                foreach (var asset in assets.EnumerateArray())
                {
                    string name = asset.GetProperty("name").GetString() ?? "";
                    // A private repository serves assets only through the API url
                    // with an octet-stream Accept header; browser_download_url
                    // needs no token but only works once the repo is public.
                    string href = token is null
                        ? asset.GetProperty("browser_download_url").GetString() ?? ""
                        : asset.GetProperty("url").GetString() ?? "";
                    if (name.EndsWith(PackExtension, StringComparison.OrdinalIgnoreCase))
                    {
                        pack = href;
                        packBytes = asset.TryGetProperty("size", out var size) ? size.GetInt64() : 0;
                    }
                    else if (name.Equals(SetupAssetName, StringComparison.OrdinalIgnoreCase))
                    {
                        setup = href;
                    }
                }
            }
            return new ReleaseInfo(
                tag.TrimStart('v', 'V'),
                tag,
                release.GetProperty("name").GetString() ?? tag,
                release.TryGetProperty("body", out var body) ? body.GetString() ?? "" : "",
                release.GetProperty("html_url").GetString() ?? "",
                pack, packBytes, setup);
        }
        return null;
    }

    /// <summary>Downloads to <paramref name="destPath"/>, reporting bytes done and total.</summary>
    public static async Task DownloadAsync(string url, string destPath, string? token,
                                           Action<long, long> progress, CancellationToken ct)
    {
        using var http = NewClient(token);
        using var request = new HttpRequestMessage(HttpMethod.Get, url);
        // Asked for explicitly: without it the API returns the asset's JSON.
        request.Headers.Accept.Clear();
        request.Headers.Accept.ParseAdd("application/octet-stream");

        using var response = await http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, ct);
        response.EnsureSuccessStatusCode();
        long total = response.Content.Headers.ContentLength ?? 0;

        Directory.CreateDirectory(Path.GetDirectoryName(destPath)!);
        await using var input = await response.Content.ReadAsStreamAsync(ct);
        await using var output = new FileStream(destPath, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20);
        byte[] buffer = new byte[1 << 20];
        long done = 0;
        int n;
        while ((n = await input.ReadAsync(buffer, ct)) > 0)
        {
            await output.WriteAsync(buffer.AsMemory(0, n), ct);
            done += n;
            progress(done, total);
        }
    }
}
