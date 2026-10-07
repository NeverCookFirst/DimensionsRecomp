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
        if (args.Length >= 2 && args[0] == "extract" && args[1] == "--embedded")
        {
            return EmbeddedShaderExtractor.Run(args[2..]);
        }
        if (args.Length >= 2 && args[0] == "extract" && args[1] == "--image")
        {
            return ImageShaderExtractor.Run(args[2..]);
        }
        if (args.Length < 2 || (args[0] != "inventory" && args[0] != "extract"))
        {
            Console.Error.WriteLine(
                "Usage: gpu-shader-extract <inventory|extract> <game-dir> [output-dir]\n" +
                "       gpu-shader-extract extract --image <mapped-image> <output-dir> --image-base <8hex> [--image-provenance <json>]\n" +
                "       gpu-shader-extract extract --embedded <archive-or-directory> <fresh-output> [--max-seconds <1..3600>]");
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

        foreach (string datPath in Directory.EnumerateFiles(gameDir, "*",
                                                             SearchOption.TopDirectoryOnly)
                                                .Where(path =>
                                                    Path.GetExtension(path).Equals(".DAT", StringComparison.OrdinalIgnoreCase) ||
                                                    Path.GetExtension(path).Equals(".DAT2", StringComparison.OrdinalIgnoreCase))
                                                .OrderBy(path => path,
                                                         StringComparer.OrdinalIgnoreCase)
                                                .ThenBy(path => path, StringComparer.Ordinal))
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

            foreach ((string internalPath, int nameTreeIndex) in names)
            {
                string extension = Path.GetExtension(internalPath);
                extension = string.IsNullOrEmpty(extension) ? "<none>" : extension.ToLowerInvariant();
                extensionCounts.TryGetValue(extension, out int count);
                extensionCounts[extension] = count + 1;

                if (args[0] != "extract" || !IsShaderCandidate(internalPath))
                {
                    continue;
                }

                string archiveName = Path.GetFileNameWithoutExtension(datPath);
                string destination;
                try
                {
                    destination = ArchiveOutputPath.Resolve(
                        Path.Combine(outputDir!, archiveName), internalPath);
                }
                catch (InvalidDataException exception)
                {
                    Console.Error.WriteLine(
                        $"skip {Path.GetFileName(datPath)}:{internalPath}: {exception.Message}");
                    skippedEntries++;
                    continue;
                }

                byte[] data;
                try
                {
                    // New-format tree values are sequential name positions,
                    // not necessarily DAT indices. The shared lookup resolves
                    // the archive CRC table before its tree fallback.
                    int entryIndex = archive.FindEntry(internalPath);
                    if (entryIndex < 0 || entryIndex >= archive.FileCount)
                        throw new InvalidDataException($"No file-table entry for name-tree index {nameTreeIndex}");
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
            Console.WriteLine($"skipped candidates: {skippedEntries}");
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
