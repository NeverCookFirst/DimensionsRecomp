// Headless mod application for the ReXGlue build.
//
//   modcli list    <modsRoot> [platform]
//   modcli apply   <targetDir> <modsRoot> [platform] [mod names...]
//   modcli restore <targetDir>
//
// targetDir is the folder that directly contains the .DAT/.HDR pair to patch.
// For the ReXGlue recomp that is the update folder (PATCH.DAT lives there),
// not the disc folder, because the update's copy of a file overrides the disc.

using DimensionsModManager;

if (args.Length == 0)
{
    Console.Error.WriteLine("usage: modcli list|apply|restore ...");
    return 2;
}

try
{
    switch (args[0].ToLowerInvariant())
    {
        case "list":
        {
            string modsRoot = args[1];
            string platform = args.Length > 2 ? args[2] : "ps3";
            foreach (ModInfo m in ModEngine.DiscoverMods(modsRoot, platform))
            {
                Console.WriteLine($"{m.Name}  [{Path.GetFileName(m.FolderPath)}]");
            }
            return 0;
        }

        case "apply":
        {
            string targetDir = args[1];
            string modsRoot = args[2];
            string platform = args.Length > 3 ? args[3] : "ps3";
            var wanted = args.Skip(4).ToHashSet(StringComparer.OrdinalIgnoreCase);

            List<ModInfo> all = ModEngine.DiscoverMods(modsRoot, platform);
            List<ModInfo> selected = wanted.Count == 0
                ? all
                : all.Where(m => wanted.Contains(m.Name) || wanted.Contains(Path.GetFileName(m.FolderPath))).ToList();

            if (selected.Count == 0)
            {
                Console.Error.WriteLine("no matching mods found");
                return 1;
            }

            foreach (ModInfo m in selected)
            {
                Console.WriteLine($"applying: {m.Name}");
            }
            AppliedState state = ModEngine.ApplyMods(targetDir, selected);
            Console.WriteLine(
                $"done: {state.Entries.Count} loose file(s), {state.DatPatches.Count} DAT injection(s)");
            return 0;
        }

        case "restore":
        {
            int n = ModEngine.RestoreVanilla(args[1]);
            Console.WriteLine($"restored {n} item(s)");
            return 0;
        }

        default:
            Console.Error.WriteLine($"unknown command: {args[0]}");
            return 2;
    }
}
catch (Exception ex)
{
    Console.Error.WriteLine($"ERROR: {ex.Message}");
    return 1;
}
