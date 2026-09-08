// Headless mod application for the ReXGlue build.
//
//   modcli list    <modsRoot> [platform]
//   modcli apply   <targetDir> <modsRoot> [platform] [--search <dir>]... [mod names...]
//   modcli restore <targetDir>
//   modcli names   <dat> [substring]
//   modcli extract <dat> <internalPath> <outFile>
//
// --search adds folders (and their immediate subfolders) where archives a mod
// names may live besides targetDir: the recomp's DLC packages sit under the
// content root as <package>/DLCnn.DAT2. Those are patched in place, with the
// same byte backups and restore as the update archives.
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
            // --search <dir> may appear any number of times after the platform:
            // extra places to look for archives a mod names (the DLC packages
            // of the Xbox 360 build live under the content root, not next to
            // PATCH.DAT). Everything else is a mod name.
            var searchDirs = new List<string>();
            var wanted = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            for (int i = 4; i < args.Length; i++)
            {
                if (args[i] == "--search" && i + 1 < args.Length)
                {
                    searchDirs.Add(args[++i]);
                }
                else
                {
                    wanted.Add(args[i]);
                }
            }

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
            AppliedState state = ModEngine.ApplyMods(targetDir, selected, searchDirs);
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

        // names <dat> [substring]  - internal paths from the name tree
        case "names":
        {
            var archive = new DatArchive(args[1]);
            string filter = args.Length > 2 ? args[2] : "";
            foreach (var (name, index) in archive.EnumerateNames())
            {
                if (filter.Length > 0 && !name.Contains(filter, StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }
                int real = archive.FindEntry(name);
                var (offset, zsize, size) = real >= 0 ? archive.GetEntry(real) : (-1L, 0u, 0u);
                Console.WriteLine($"{real,7} @0x{offset:X} z={zsize} s={size}  {name}");
            }
            return 0;
        }

        // extract <dat> <internalPath> <outFile>
        case "extract":
        {
            var archive = new DatArchive(args[1]);
            int index = archive.FindEntry(args[2]);
            if (index < 0)
            {
                Console.Error.WriteLine("not found");
                return 1;
            }
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(args[3]))!);
            File.WriteAllBytes(args[3], archive.ReadEntryData(index));
            var (off, z, s) = archive.GetEntry(index);
            Console.WriteLine($"entry {index} @0x{off:X} z={z} s={s} -> {args[3]}");
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
