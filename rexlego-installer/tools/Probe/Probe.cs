using RecompSetup;

if (args.Length < 2) { Console.Error.WriteLine("probe xex <file> | stfs <file> [extract-dir]"); return 2; }
switch (args[0])
{
    case "xex":
    {
        var i = Xex2Info.Read(args[1]);
        if (i is null) { Console.WriteLine("not a XEX2"); return 1; }
        Console.WriteLine($"title={i.TitleId:X8} media={i.MediaId:X8} version={i.VersionString} ({i.Version:X8}) base={i.BaseVersionString} ({i.BaseVersion:X8})");
        return 0;
    }
    case "stfs":
    {
        using var p = new StfsPackage(args[1]);
        Console.WriteLine($"magic={p.Magic} type={p.ContentType:X8} title={p.Execution.TitleId:X8} media={p.Execution.MediaId:X8} version={p.Execution.VersionString} name='{p.DisplayName}' game='{p.TitleName}'");
        foreach (var e in p.Entries) Console.WriteLine($"  {(e.IsDirectory ? "<dir> " : "")}{e.Path}  {e.Length}");
        if (args.Length > 2)
        {
            long total = 0;
            foreach (var e in p.Entries.Where(e => !e.IsDirectory))
                p.Extract(e, Path.Combine(args[2], e.Path), n => total += n);
            Console.WriteLine($"extracted {total} bytes to {args[2]}");
        }
        return 0;
    }
}
return 2;
