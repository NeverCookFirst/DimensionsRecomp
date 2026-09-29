using System.Globalization;
using DimensionsModManager;

namespace GpuShaderExtract;

internal static class Program
{
    private static readonly string[] ShaderExtensions =
    {
        ".vso", ".pso", ".vsh", ".psh", ".fxo", ".shader", ".bin"
    };

    private static int Main(string[] args)
    {
        if (args.Length < 2 || (args[0] != "inventory" && args[0] != "extract"))
        {
            Console.Error.WriteLine(
                "Usage: gpu-shader-extract <inventory|extract> <game-dir> [output-dir]");
            return 2;
        }

        string gameDir = Path.GetFullPath(args[1]);
        if (!Directory.Exists(gameDir))
        {
            Console.Error.WriteLine($"Game directory not found: {gameDir}");
            return 2;
        }

        string? outputDir = args.Length >= 3 ? Path.GetFullPath(args[2]) : null;
        if (args[0] == "extract" && outputDir is null)
        {
            Console.Error.WriteLine("extract requires an output directory");
            return 2;
        }

        var extensionCounts = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        int namedEntries = 0;
        int extractedEntries = 0;
        int skippedEntries = 0;

        foreach (string datPath in Directory.EnumerateFiles(gameDir, "*.DAT",
                                                             SearchOption.TopDirectoryOnly)
                                                .OrderBy(path => path,
                                                         StringComparer.OrdinalIgnoreCase))
        {
            string hdrPath = DatArchive.HdrPathFor(datPath);
            if (!File.Exists(hdrPath))
            {
                Console.WriteLine($"skip {Path.GetFileName(datPath)}: no matching header");
                continue;
            }

            var archive = new DatArchive(datPath);
            var names = archive.EnumerateNames().OrderBy(pair => pair.Key,
                                                          StringComparer.OrdinalIgnoreCase)
                               .ToArray();
            namedEntries += names.Length;
            Console.WriteLine($"{Path.GetFileName(datPath)}: {archive.FileCount} entries, " +
                              $"{names.Length} named");

            foreach ((string internalPath, int entryIndex) in names)
            {
                string extension = Path.GetExtension(internalPath);
                extension = string.IsNullOrEmpty(extension) ? "<none>" : extension.ToLowerInvariant();
                extensionCounts.TryGetValue(extension, out int count);
                extensionCounts[extension] = count + 1;

                if (args[0] != "extract" || !IsShaderCandidate(internalPath))
                {
                    continue;
                }

                byte[] data;
                try
                {
                    data = archive.ReadEntryDataDecompressed(entryIndex);
                }
                catch (Exception exception) when (exception is IOException or
                                                  InvalidDataException or
                                                  ArgumentOutOfRangeException)
                {
                    // Some optional install archives are intentionally sparse:
                    // their headers describe files supplied by another disc or
                    // install phase, while the local DAT ends before the slot.
                    // A missing optional entry must not discard thousands of
                    // valid shaders extracted from the other archives.
                    Console.Error.WriteLine(
                        $"skip {Path.GetFileName(datPath)}:{internalPath}: {exception.Message}");
                    skippedEntries++;
                    continue;
                }
                string archiveName = Path.GetFileNameWithoutExtension(datPath);
                string safeRelative = internalPath.Replace('\\', Path.DirectorySeparatorChar)
                                                  .Replace('/', Path.DirectorySeparatorChar)
                                                  .TrimStart(Path.DirectorySeparatorChar);
                string destination = Path.Combine(outputDir!, archiveName, safeRelative);
                Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
                File.WriteAllBytes(destination, data);
                extractedEntries++;
            }
        }

        Console.WriteLine($"named entries: {namedEntries.ToString(CultureInfo.InvariantCulture)}");
        foreach ((string extension, int count) in extensionCounts
                     .OrderByDescending(pair => pair.Value)
                     .ThenBy(pair => pair.Key, StringComparer.OrdinalIgnoreCase))
        {
            Console.WriteLine($"{count,7} {extension}");
        }

        if (args[0] == "extract")
        {
            Console.WriteLine($"extracted shader candidates: {extractedEntries}");
            Console.WriteLine($"skipped unreadable candidates: {skippedEntries}");
            return extractedEntries == 0 ? 1 : 0;
        }
        return 0;
    }

    private static bool IsShaderCandidate(string path)
    {
        string extension = Path.GetExtension(path);
        return ShaderExtensions.Contains(extension, StringComparer.OrdinalIgnoreCase) ||
               path.Contains("shader", StringComparison.OrdinalIgnoreCase);
    }
}
