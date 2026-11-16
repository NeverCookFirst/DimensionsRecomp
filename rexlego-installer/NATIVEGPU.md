# Native GPU installer

Build the standard installer staging first with `build-installer.ps1`. The Native GPU package reuses its optional components and installs the runtime from the hash-checked `Dimensions-NativeGPU-Dev` folder. Run these commands from the repository root:

```powershell
dotnet publish rexlego-installer/Setup.csproj -c Release -r win-x64 --self-contained -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -p:NativeGpuEdition=true -o dist/NativeGPU-OriginalWizard/host
py -3 rexlego-installer/package_native.py --game-exe "<path to rebuilt legodimensions.exe>" --crash-monitor "<path to rebuilt CrashCapture.exe>"
```

The distributable is `dist/NativeGPU-OriginalWizard/DimensionsRecompiled-NativeGPU-Setup.exe`. The executable inside `host` has no installation payload and acts as the updater.

Install into a new folder. The wizard copies user-supplied base game, TU23 and DLC data and offers the same optional components as the standard wizard. The release includes `KNOWN-BUGS.md` in the installed folder. The Native GPU edition uses its own install manifest identity, desktop shortcut, and installer logs. Its online update feed is disabled for local testing.

Start `legodimensions.exe` or use the desktop shortcut. The executable reads the renderer defaults from `native-environment.ini` and records the run under `logs`. The runtime writes its crash dumps beside the executable. Review the run logs and dumps before sharing them.
