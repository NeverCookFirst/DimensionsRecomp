namespace GpuShaderExtract;

internal static class ArchiveOutputPath
{
    internal static string Resolve(string archiveOutputRoot, string internalPath)
    {
        // Archive paths use a virtual root and either separator spelling.
        // A Windows drive or alternate data stream is never a virtual path,
        // including when the extraction tool itself is running on Unix.
        string relative = internalPath.Replace('\\', '/').TrimStart('/');
        if (relative.Length == 0 || relative.Contains(':'))
        {
            throw new InvalidDataException("unsafe archive path: empty path or drive/stream syntax");
        }

        try
        {
            string root = Path.GetFullPath(archiveOutputRoot);
            string rootPrefix = Path.EndsInDirectorySeparator(root)
                ? root : root + Path.DirectorySeparatorChar;
            string destination = Path.GetFullPath(Path.Combine(
                root, relative.Replace('/', Path.DirectorySeparatorChar)));
            StringComparison comparison = OperatingSystem.IsWindows()
                ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
            if (!destination.StartsWith(rootPrefix, comparison))
            {
                throw new InvalidDataException("unsafe archive path: destination escapes its archive directory");
            }
            return destination;
        }
        catch (Exception exception) when (exception is ArgumentException or
                                          NotSupportedException or PathTooLongException)
        {
            throw new InvalidDataException("unsafe archive path: invalid filesystem path", exception);
        }
    }
}
