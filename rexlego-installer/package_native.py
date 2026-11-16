"""Append a NativeGPU payload to the original setup wizard's published host."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import zipfile


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--game-exe", type=Path, required=True,
                        help="Rebuilt legodimensions.exe with native direct startup")
    parser.add_argument("--crash-monitor", type=Path, required=True,
                        help="CrashCapture.exe built with observe mode")
    args = parser.parse_args()
    if not args.game_exe.is_file() or "native-environment.ini".encode("utf-16le") not in args.game_exe.read_bytes():
        parser.error("--game-exe must be a Direct Startup build")
    installer = Path(__file__).resolve().parent
    workspace = installer.parent
    output = workspace / "dist/NativeGPU-OriginalWizard"
    executable = output / "DimensionsRecompiled-NativeGPU-Setup.exe"
    host = output / "host" / executable.name
    staging = installer / "staging"
    dev = workspace / "Dimensions-NativeGPU-Dev"
    files = {}
    for component in ("mods", "rus", "modcli", "moddingtools", "saveconverter",
                      "toypad", "update", "update-mods"):
        paths = [p for p in (staging / component).rglob("*") if p.is_file()]
        if not paths:
            raise ValueError(f"Missing original setup component: {component}")
        files.update({p.relative_to(staging).as_posix(): p for p in paths})
    files["rus/Lang_Russian_By_koctr113/native-menu-ru.tsv"] = (
        installer / "Resources/native-menu-ru.tsv"
    )
    installed = json.loads((dev / "install.json").read_text(encoding="utf-8-sig"))
    for name, expected in installed["payload_hashes"].items():
        if name.startswith("runtime/"):
            source = dev / name
            if sha(source) != expected:
                raise ValueError(f"NativeGPU runtime hash mismatch: {name}")
            files["game/" + source.name] = source
    handoff = workspace / "native-gpu-installer/KNOWN-BUGS.md"
    files["game/KNOWN-BUGS.md"] = handoff
    files["game/legodimensions.exe"] = args.game_exe
    build_marker = args.game_exe.with_name("native-gpu-build.txt")
    if not build_marker.is_file():
        parser.error("--game-exe must be beside its generated native-gpu-build.txt")
    files["game/native-gpu-build.txt"] = build_marker
    files["game/tools/CrashCapture.exe"] = args.crash_monitor
    environment = json.loads((dev / "native-environment.json").read_text(encoding="utf-8-sig"))
    renderer_settings = "[renderer]\n" + "".join(f"{name}={value}\n" for name, value in environment.items())
    manifest = json.loads((staging / "manifest.json").read_text(encoding="utf-8-sig"))
    manifest.update(product="DimensionsRecompiledNativeGPU", version="0.2.0",
                    notes="Native GPU 0.2.0")
    manifest["toml"]["forced"].update(gpu_backend="'d3d12'", updates_check="false")
    manifest["toml"].setdefault("defaults", {}).update(
        native_settings_frontend="true", native_fsr3="false"
    )
    manifest["files"] = [
        {"p": name, "sha": sha(path), "len": path.stat().st_size}
        for name, path in sorted(files.items()) if not name.startswith("moddingtools/")
    ]
    manifest["files"].append({"p": "updater/rexupdate.exe", "sha": sha(host),
                              "len": host.stat().st_size})
    settings_bytes = renderer_settings.encode("ascii")
    manifest["files"].append({"p": "game/native-environment.ini",
                              "sha": hashlib.sha256(settings_bytes).hexdigest(),
                              "len": len(settings_bytes)})
    payload = output / "payload.zip"
    with zipfile.ZipFile(payload, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, path in sorted(files.items()):
            archive.write(path, name)
        archive.writestr("game/native-environment.ini", settings_bytes)
        archive.writestr("manifest.json", json.dumps(manifest, indent=2))
    temporary = executable.with_suffix(".stage")
    with temporary.open("wb") as stream, host.open("rb") as app, payload.open("rb") as data:
        shutil.copyfileobj(app, stream)
        shutil.copyfileobj(data, stream)
        stream.write(b"RXPAYLD1" + struct.pack("<q", payload.stat().st_size))
    temporary.replace(executable)
    shutil.copy2(handoff, output / handoff.name)
    print(f"{executable}\nSHA256 {sha(executable)}")


if __name__ == "__main__":
    main()
