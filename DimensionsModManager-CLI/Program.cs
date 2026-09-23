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

        // grep <dat> <needle> [nameFilter]  - entries whose raw bytes contain
        // |needle|. Only useful for entries stored uncompressed (the .txt and
        // .csv config files are), which is exactly what config spelunking needs.
        case "grep":
        {
            var archive = new DatArchive(args[1]);
            byte[] needle = System.Text.Encoding.ASCII.GetBytes(args[2]);
            string nameFilter = args.Length > 3 ? args[3] : "";
            int hits = 0;
            foreach (var (name, _) in archive.EnumerateNames())
            {
                if (nameFilter.Length > 0 &&
                    !name.Contains(nameFilter, StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }
                int index = archive.FindEntry(name);
                if (index < 0)
                {
                    continue;
                }
                var (_, zsize, size) = archive.GetEntry(index);
                if (zsize != size || zsize == 0 || zsize > 8 * 1024 * 1024)
                {
                    continue; // compressed or implausible - a raw search would lie
                }
                byte[] data;
                try
                {
                    data = archive.ReadEntryData(index);
                }
                catch (IOException)
                {
                    continue;
                }
                if (IndexOfBytes(data, needle) >= 0)
                {
                    Console.WriteLine($"{index,7}  {name}");
                    hits++;
                }
            }
            Console.Error.WriteLine($"{hits} hit(s)");
            return hits > 0 ? 0 : 1;
        }

        // extract <dat> <internalPath> <outFile>
        // retag <dat> <oldPrefix> <newPrefix> [old=new ...]
        // Renames entries in place (CRC table only, data untouched). Use on a
        // COPY of an archive placed in a spare INSTALL0_ slot.
        case "retag":
        {
            var archive = new DatArchive(args[1]);
            string oldPrefix = args[2], newPrefix = args[3];
            var exact = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            for (int i = 4; i < args.Length; i++)
            {
                int eq = args[i].IndexOf('=');
                if (eq > 0) exact[args[i][..eq]] = args[i][(eq + 1)..];
            }
            int n = archive.RetagEntries(name =>
            {
                if (exact.TryGetValue(name, out var to)) return to;
                if (name.StartsWith(oldPrefix, StringComparison.OrdinalIgnoreCase))
                    return newPrefix + name[oldPrefix.Length..];
                return null;
            });
            archive.SaveHdr();
            Console.WriteLine($"retagged {n} entries in {archive.HdrPath}");
            return 0;
        }

        // build <folder> <out.DAT> [name] [author] [version]
        // Packs a folder into a brand-new archive pair (out.DAT + out.HDR) in
        // the game's own PATCH/DLC format, using Connor's BrickVault writer.
        // The game probes numbered PATCH archives itself, so a mod that adds
        // files - or files bigger than the ones they replace - goes into a
        // fresh PATCH<n>.DAT next to the update instead of being injected.
        case "build":
        {
            string folder = Path.GetFullPath(args[1]);
            string outDat = Path.GetFullPath(args[2]);
            if (!Directory.Exists(folder))
            {
                Console.Error.WriteLine($"no such folder: {folder}");
                return 1;
            }
            if (!outDat.EndsWith(".DAT", StringComparison.OrdinalIgnoreCase))
            {
                Console.Error.WriteLine("the output must end in .DAT (the .HDR is written beside it)");
                return 1;
            }
            var settings = new BrickVault.DATBuildSettings
            {
                BuilderID = "modcli",
                InputFolderLocation = folder.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar,
                OutputFileLocation = outDat,
                Version = BrickVault.Types.DATFile.DATVersion.V11,
                ShouldCreateHDR = true,
            };
            if (args.Length > 3)
            {
                settings.SetupAsMod(args.Length > 4 ? args[4] : "unknown", args[3],
                                    args.Length > 5 ? args[5] : "1.0");
            }
            var progress = new BrickVault.BuildProgress();
            BrickVault.Types.DATFile.BuildFromFolder(settings, progress);
            var check = new DatArchive(outDat);
            Console.WriteLine($"built {outDat}: {check.FileCount} file(s), readable by our own parser");
            return 0;
        }

        case "bvextract":
        {
            // bvextract <dat> <list> <outDir>: each list line is "name|dest", extracted
            // through BrickVault, which decompresses the disc (v7) archives that
            // our own reader hands back still packed.
            var dat = BrickVault.Types.DATFile.Open(args[1]);
            var byName = new Dictionary<string, BrickVault.ArchiveFile>(StringComparer.OrdinalIgnoreCase);
            foreach (var f in dat.Files)
            {
                string key = f.Path.Replace('/', '\\').TrimStart('\\');
                byName.TryAdd(key, f);
            }
            int ok = 0, miss = 0;
            foreach (string line in File.ReadAllLines(args[2]))
            {
                string[] p = line.Split('|');
                if (p.Length < 2) continue;
                if (!byName.TryGetValue(p[0].Trim(), out var entry)) { Console.Error.WriteLine($"not found: {p[0]}"); miss++; continue; }
                string dest = Path.Combine(args[3], p[1].Trim().Replace('\\', Path.DirectorySeparatorChar));
                Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                using var s = dat.Extract(entry);
                using var o = File.Create(dest);
                s.Position = 0;
                s.CopyTo(o);
                ok++;
            }
            Console.WriteLine($"{ok} extracted, {miss} not found");
            return miss == 0 ? 0 : 1;
        }
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
            // Entries are usually DFLT-compressed; hand back what the game
            // would actually see, not the packed bytes.
            File.WriteAllBytes(args[3], archive.ReadEntryDataDecompressed(index));
            var (off, z, s) = archive.GetEntry(index);
            Console.WriteLine($"entry {index} @0x{off:X} z={z} s={s} -> {args[3]}");
            return 0;
        }

        // clone-tree <dat> <srcPrefix> <dstPrefix> <outDir> [old=new ...]
        // Extracts every entry under |srcPrefix| into <outDir>/<dstPrefix>/<rel>,
        // decompressed, laid out as a mod's datfiles tree. Each old=new pair
        // adds a second copy of any file whose relative path contains |old|,
        // with |old| replaced - that is how a character's cache gets cloned for
        // a character that shipped without one: the engine looks up
        // charcache\<char>\chars\minifig\<char>\<char>.cd.res by name.
        case "clone-tree":
        {
            var archive = new DatArchive(args[1]);
            string srcPrefix = args[2].TrimEnd('\\') + "\\";
            string dstPrefix = args[3].TrimEnd('\\') + "\\";
            string outDir = args[4];
            var renames = args.Skip(5)
                .Select(a => a.Split('=', 2))
                .Where(p => p.Length == 2)
                .ToList();
            int written = 0;
            foreach (var (name, _) in archive.EnumerateNames())
            {
                if (!name.StartsWith(srcPrefix, StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }
                int index = archive.FindEntry(name);
                if (index < 0)
                {
                    continue;
                }
                byte[] data = archive.ReadEntryDataDecompressed(index);
                string rel = name.Substring(srcPrefix.Length);
                var targets = new List<string> { rel };
                foreach (string[] r in renames)
                {
                    if (rel.Contains(r[0], StringComparison.OrdinalIgnoreCase))
                    {
                        targets.Add(rel.Replace(r[0], r[1], StringComparison.OrdinalIgnoreCase));
                    }
                }
                foreach (string t in targets)
                {
                    string dest = Path.Combine(outDir, (dstPrefix + t).Replace('\\', Path.DirectorySeparatorChar));
                    Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                    File.WriteAllBytes(dest, data);
                    written++;
                }
            }
            Console.WriteLine($"{written} file(s) written under {outDir}");
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
    if (Environment.GetEnvironmentVariable("MODCLI_DEBUG") == "1") Console.Error.WriteLine(ex.ToString());
    return 1;
}

// Plain byte-substring search; the payloads here are small config files, so a
// naive scan is fast enough and avoids pulling in a text encoding guess.
static int IndexOfBytes(byte[] haystack, byte[] needle)
{
    for (int i = 0; i + needle.Length <= haystack.Length; i++)
    {
        int j = 0;
        while (j < needle.Length && haystack[i + j] == needle[j])
        {
            j++;
        }
        if (j == needle.Length)
        {
            return i;
        }
    }
    return -1;
}
