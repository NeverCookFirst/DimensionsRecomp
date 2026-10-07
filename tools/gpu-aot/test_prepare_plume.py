"""Prepare real pinned public Plume in disposable, offline Git checkouts.

No compiler, assets, downloads, or edits to the source checkout are required.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


PIN = "e0c8871b930dc1a6544bf457f2348c456b9d7e13"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parents[2]
    parser.add_argument("output", type=Path)
    parser.add_argument("--checkout", type=Path, default=root / "thirdparty/plume")
    parser.add_argument("--preparer", type=Path, default=root / "tools/gpu-aot/prepare_plume.py")
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    source = args.checkout.resolve()
    preparer = args.preparer.resolve()
    patch = root / "tools/gpu-aot/patches/plume-lego.patch"
    cache_patch = root / "tools/gpu-aot/patches/plume-state-cache.patch"
    git_executable = shutil.which("git")
    if git_executable is None:
        parser.error("Git is required for the offline public-source fixture")
    checks = []

    def command(argv, cwd, *, expect=0, oracle=None):
        result = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, timeout=15)
        if result.returncode != expect or (oracle is not None and oracle not in result.stdout + result.stderr):
            raise AssertionError(f"Unexpected preparation result: expected exit {expect}, oracle {oracle!r}; "
                                 f"got {result.returncode}\n{result.stdout}\n{result.stderr}")
        return result

    def git(checkout, *arguments):
        return command([git_executable, "-C", str(checkout), *arguments], args.output)

    # Record Git metadata and the dirty source before any sandbox operations.
    original = {
        "head": git(source, "rev-parse", "HEAD").stdout,
        "status": git(source, "status", "--porcelain=v1", "--untracked-files=all").stdout,
        "source_sha256": digest(source / "plume_d3d12.cpp"),
        "header_sha256": digest(source / "plume_d3d12.h"),
    }
    git(source, "cat-file", "-e", f"{PIN}^{{commit}}")
    public_blob = subprocess.run([git_executable, "-C", str(source), "show", f"{PIN}:plume_d3d12.cpp"],
                                 capture_output=True, check=True, timeout=15).stdout

    with tempfile.TemporaryDirectory(prefix="plume-preparation-", dir=args.output) as temporary:
        sandbox = Path(temporary)
        unrelated = sandbox / "unrelated-working-directory"
        unrelated.mkdir()

        def clone(name):
            checkout = sandbox / name
            # Local object sharing only: no fetch or network access, and no writes
            # to the original checkout's index, HEAD, or worktree registration.
            command([git_executable, "clone", "--shared", "--no-checkout", str(source), str(checkout)], unrelated)
            git(checkout, "checkout", "--detach", PIN)
            if (checkout / "plume_d3d12.cpp").read_bytes() != public_blob:
                raise AssertionError("Isolated checkout does not contain the actual pinned public source")
            return checkout

        def prepare(checkout, *options, expect=0, oracle=None):
            return command([sys.executable, str(preparer), str(checkout), *options], unrelated,
                           expect=expect, oracle=oracle)

        expected = clone("independent-real-git-patch")
        git(expected, "apply", "--check", str(patch))
        git(expected, "apply", str(patch))
        git(expected, "apply", "--check", str(cache_patch))
        git(expected, "apply", str(cache_patch))
        expected_sha = digest(expected / "plume_d3d12.cpp")
        expected_header_sha = digest(expected / "plume_d3d12.h")
        checks.append("actual pinned public source and independently Git-applied patch")

        checkout = clone("preparer-under-test")
        before = digest(checkout / "plume_d3d12.cpp")
        prepare(checkout, "--check", expect=2, oracle="Native Plume fixes are missing or incomplete")
        if digest(checkout / "plume_d3d12.cpp") != before:
            raise AssertionError("Missing-patch --check modified source")
        checks.append("missing patch rejected without writes")

        subdirectory = checkout / "nested"
        subdirectory.mkdir()
        for options in ((), ("--check",)):
            prepare(subdirectory, *options, expect=2, oracle="must be the repository root, not a subdirectory")
        if digest(checkout / "plume_d3d12.cpp") != before:
            raise AssertionError("Subdirectory rejection modified source")
        checks.append("apply/check reject repository subdirectory with exact oracle")

        prepare(checkout, oracle="Applied native fence/resize/command-allocation fixes")
        if digest(checkout / "plume_d3d12.cpp") != expected_sha:
            raise AssertionError("Preparer result differs from independently applied real public patch")
        if digest(checkout / "plume_d3d12.h") != expected_header_sha:
            raise AssertionError("State-cache header differs from independent real patch")
        prepare(checkout, "--check", oracle="PASS: public Plume")
        prepare(checkout, oracle="PASS: public Plume")
        if digest(checkout / "plume_d3d12.cpp") != expected_sha:
            raise AssertionError("Repeated preparation changed the patched source")
        checks.append("apply/check/repeat from unrelated cwd preserve exact patch result")

        old = clone("old-mandatory-prepared-checkout")
        git(old, "apply", str(patch))
        old_cpp, old_header = digest(old / "plume_d3d12.cpp"), digest(old / "plume_d3d12.h")
        prepare(old, "--check", expect=2, oracle="Native Plume state-cache fixes are missing or incomplete")
        if (digest(old / "plume_d3d12.cpp"), digest(old / "plume_d3d12.h")) != (old_cpp, old_header):
            raise AssertionError("Old prepared --check mutated source")
        prepare(old, oracle="Applied native fence/resize/command-allocation fixes")
        prepare(old, "--check", oracle="PASS: public Plume")
        if (digest(old / "plume_d3d12.cpp"), digest(old / "plume_d3d12.h")) != (expected_sha, expected_header_sha):
            raise AssertionError("Old prepared upgrade differs from fresh complete preparation")
        checks.append("old mandatory-prepared checkout upgrades without reverting existing fixes")

        cache_partial = clone("partial-state-cache-header")
        prepare(cache_partial)
        header = (cache_partial / "plume_d3d12.h").read_text()
        cache_partial.joinpath("plume_d3d12.h").write_text(header.replace(
            "bool cachedPipelineValid = false;", "bool cachedPipelineValid = true;"))
        prepare(cache_partial, "--check", expect=2, oracle="Native Plume state-cache fixes are missing or incomplete")
        checks.append("partial state-cache header rejected")

        with (checkout / "plume_d3d12.cpp").open("a") as stream:
            stream.write("\n// Fixture unrelated local edit outside all patched hunks.\n")
        local_sha = digest(checkout / "plume_d3d12.cpp")
        prepare(checkout, "--check", oracle="PASS: public Plume")
        prepare(checkout, oracle="PASS: public Plume")
        if digest(checkout / "plume_d3d12.cpp") != local_sha:
            raise AssertionError("Preparation altered an unrelated local edit")
        checks.append("unrelated local edits survive check and repeat")

        # Change just one mandatory factory guard; --check must reject a partial patch.
        text = (checkout / "plume_d3d12.cpp").read_text()
        old = "if (semaphore->d3d == nullptr) {"
        if text.count(old) != 1:
            raise AssertionError("Actual patched factory guard was not found exactly once")
        (checkout / "plume_d3d12.cpp").write_text(text.replace(old, "if (false) {"))
        prepare(checkout, "--check", expect=2, oracle="Native Plume fixes are missing or incomplete")
        checks.append("partial command-creation patch rejected")

        wrong = clone("wrong-revision")
        tree = git(wrong, "rev-parse", f"{PIN}^{{tree}}").stdout.strip()
        commit = command([git_executable, "-C", str(wrong), "-c", "user.name=Fixture",
                          "-c", "user.email=fixture@example.invalid", "commit-tree", tree,
                          "-p", PIN, "-m", "Synthetic wrong revision; public source is unchanged"], unrelated).stdout.strip()
        git(wrong, "checkout", "--detach", commit)
        prepare(wrong, expect=2, oracle=f"Expected public Plume {PIN}, got {commit}")
        checks.append("wrong revision rejected even with identical public tree")

        noop = clone("successful-no-op-git")
        # Execute the actual preparer while making git apply falsely return zero.
        # All repository/revision commands still run real Git. Test both a false
        # already-applied report and a no-op apply followed by false verification.
        wrapper = '''import runpy, subprocess, sys
real_run = subprocess.run
reverse_calls = 0
mode = sys.argv[1]
preparer, checkout = sys.argv[2:4]
def fake_run(argv, *args, **kwargs):
    global reverse_calls
    if isinstance(argv, list) and argv[0] == 'git' and 'apply' in argv:
        if '--reverse' in argv:
            reverse_calls += 1
            if mode == 'apply' and reverse_calls == 1:
                return subprocess.CompletedProcess(argv, 1, '', 'fixture: patch not applied')
        return subprocess.CompletedProcess(argv, 0, '', '')
    return real_run(argv, *args, **kwargs)
subprocess.run = fake_run
sys.argv = [preparer, checkout]
runpy.run_path(preparer, run_name='__main__')
'''
        for mode in ("already", "apply"):
            command([sys.executable, "-c", wrapper, mode, str(preparer), str(noop)], unrelated,
                    expect=2, oracle="Native Plume source verification failed: missing patched hunk")
            if digest(noop / "plume_d3d12.cpp") != before:
                raise AssertionError("No-op Git fixture unexpectedly modified source")
        checks.append("false Git success rejected for already-applied and no-op apply paths")

        cache_noop = clone("successful-no-op-state-cache-git")
        git(cache_noop, "apply", str(patch))
        cache_before = (digest(cache_noop / "plume_d3d12.cpp"), digest(cache_noop / "plume_d3d12.h"))
        cache_wrapper = wrapper.replace("and 'apply' in argv:",
            "and 'apply' in argv and any('plume-state-cache.patch' in str(v) for v in argv):")
        for mode in ("already", "apply"):
            command([sys.executable, "-c", cache_wrapper, mode, str(preparer), str(cache_noop)], unrelated,
                    expect=2, oracle="Native Plume source verification failed: missing patched hunk")
            if (digest(cache_noop / "plume_d3d12.cpp"), digest(cache_noop / "plume_d3d12.h")) != cache_before:
                raise AssertionError("State-cache no-op fixture modified source")
        checks.append("state-cache false Git success rejected independently of mandatory stage")

    after = {
        "head": git(source, "rev-parse", "HEAD").stdout,
        "status": git(source, "status", "--porcelain=v1", "--untracked-files=all").stdout,
        "source_sha256": digest(source / "plume_d3d12.cpp"),
        "header_sha256": digest(source / "plume_d3d12.h"),
    }
    if original != after:
        raise AssertionError("Original Plume checkout changed during the offline fixture")
    checks.append("original checkout HEAD/status/source preserved")
    report = {"passed": True, "pin": PIN, "actual_public_source": True, "offline": True,
              "preparer_sha256": digest(preparer), "patch_sha256": digest(patch),
              "state_cache_patch_sha256": digest(cache_patch),
              "public_source_sha256": hashlib.sha256(public_blob).hexdigest(),
              "patched_source_sha256": expected_sha,
              "patched_header_sha256": expected_header_sha, "checks": checks}
    (args.output / "verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS: {len(checks)} actual-public-source Plume preparation checks; original checkout unchanged")


if __name__ == "__main__":
    main()
