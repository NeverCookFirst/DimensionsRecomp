"""Apply or verify the native renderer's fixes on the public pinned Plume source."""

import argparse
from pathlib import Path
import re
import subprocess


PIN = "e0c8871b930dc1a6544bf457f2348c456b9d7e13"


def verify_patched_source(checkout, patch):
    """Check every new-side hunk, independently of git apply's exit status.

    Git may skip patches outside its current prefix and still return success.
    Matching the actual hunks also allows unrelated local edits to survive.
    """
    target = None
    hunks = 0
    expected = None
    lines = []

    def finish():
        nonlocal hunks
        if expected is None:
            return
        if len(lines) != expected or not lines or target is None:
            raise ValueError("Invalid native Plume patch hunk")
        path = checkout / target
        if path.resolve() != path or not path.is_file():
            raise ValueError(f"Native Plume source verification failed: invalid source path {target}")
        if "\n".join(lines) + "\n" not in path.read_text():
            raise ValueError(f"Native Plume source verification failed: missing patched hunk in {target}")
        hunks += 1

    for line in patch.read_text().splitlines():
        if line.startswith("diff --git "):
            finish()
            expected = None
            lines = []
            target = None
        elif line.startswith("+++ b/"):
            target = Path(line[6:])
            if target.is_absolute() or ".." in target.parts:
                raise ValueError("Invalid native Plume patch target")
        elif line.startswith("@@ "):
            finish()
            header = re.fullmatch(r"@@ -\d+(?:,\d+)? \+\d+(?:,(\d+))? @@.*", line)
            if header is None:
                raise ValueError("Invalid native Plume patch header")
            expected = int(header.group(1) or "1")
            lines = []
        elif expected is not None:
            if line.startswith((" ", "+")):
                lines.append(line[1:])
            elif not line.startswith(("-", "\\ No newline")):
                raise ValueError("Invalid native Plume patch content")
    finish()
    if hunks == 0:
        raise ValueError("Native Plume patch has no verifiable hunks")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parents[2]
    parser.add_argument("checkout", type=Path, nargs="?", default=root / "thirdparty/plume")
    parser.add_argument("--check", action="store_true", help="Verify only; do not modify the checkout")
    args = parser.parse_args()
    checkout = args.checkout.resolve()
    patches = (
        (root / "tools/gpu-aot/patches/plume-lego.patch", "Native Plume fixes"),
        (root / "tools/gpu-aot/patches/plume-state-cache.patch", "Native Plume state-cache fixes"),
    )

    def git(*arguments, check=False):
        return subprocess.run(["git", "-C", str(checkout), *arguments],
                              cwd=checkout, check=check, capture_output=True, text=True, timeout=30)

    if not checkout.is_dir():
        parser.error(f"Plume checkout does not exist: {checkout}")
    repository = git("rev-parse", "--show-toplevel")
    if repository.returncode or Path(repository.stdout.strip()).resolve() != checkout:
        parser.error("Plume checkout must be the repository root, not a subdirectory")
    revision = git("rev-parse", "HEAD", check=True).stdout.strip()
    if revision != PIN:
        parser.error(f"Expected public Plume {PIN}, got {revision}; select the recorded dependency revision")
    applied = False
    for patch, description in patches:
        if git("apply", "--reverse", "--check", str(patch)).returncode == 0:
            try:
                verify_patched_source(checkout, patch)
            except ValueError as error:
                parser.error(str(error))
            continue
        if args.check:
            parser.error(description + " are missing or incomplete; run prepare_plume.py without --check")
        applicability = git("apply", "--check", str(patch))
        if applicability.returncode:
            parser.error("Cannot apply " + description.lower() + " without conflicting with existing edits:\n" + applicability.stderr.strip())
        git("apply", str(patch), check=True)
        git("apply", "--reverse", "--check", str(patch), check=True)
        try:
            verify_patched_source(checkout, patch)
        except ValueError as error:
            parser.error(str(error))
        applied = True
    if applied:
        print(f"Applied native fence/resize/command-allocation fixes and opt-in state cache to public Plume {PIN}")
    else:
        print(f"PASS: public Plume {PIN} with native fence/resize/command-allocation fixes and opt-in state cache")


if __name__ == "__main__":
    main()
