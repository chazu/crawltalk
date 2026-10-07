#!/usr/bin/env python3
"""Build an isolated development image from the checked-in seed and file-ins."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ["PosixFilePage.st", "PosixFile.st", "PosixFileDirectory.st", "HostServices.st"]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", type=Path, default=ROOT / "build/crawltalk-headless")
    parser.add_argument("--output", type=Path, default=ROOT / "work/development")
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error(f"destination already exists: {output}; choose a new directory")
    shutil.copytree(ROOT / "files", output)
    command = [str(args.runner.resolve()), "--directory", str(output)]
    for name in SOURCES:
        command += ["--file-in", str(ROOT / "smalltalk" / name)]
    command += ["--eval", "Smalltalk snapshotAs: 'snapshot' thenQuit: false. true", "--expect", "true"]
    subprocess.run(command, check=True)
    subprocess.run([str(args.runner.resolve()), "--directory", str(output), "--gc",
                    "--eval", "(HostServices echo: 'image restored') result", "--expect", "image restored"], check=True)
    paths = [ROOT / "files/snapshot.im", ROOT / "files/Smalltalk-80.sources",
             ROOT / "files/Smalltalk-80.changes"] + [ROOT / "smalltalk" / name for name in SOURCES]
    manifest = {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}
    (output / "build-manifest.json").write_text(json.dumps({"hostAbi": 2, "inputs": manifest}, indent=2) + "\n")
    print(f"Development image ready: {output}")

if __name__ == "__main__":
    main()
