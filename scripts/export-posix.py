#!/usr/bin/env python3
"""Export every POSIX class method from a disposable copy of the supplied seed."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--runner", type=Path, default=ROOT / "build/crawltalk-headless")
parser.add_argument("--output", type=Path, default=ROOT / "work/exported-posix")
args = parser.parse_args()
if args.output.exists():
    parser.error("output already exists; choose a new directory")
names = ["PosixFilePage", "PosixFile", "PosixFileDirectory"]
with tempfile.TemporaryDirectory(prefix="crawltalk-export-") as tmp:
    image = Path(tmp) / "image"
    shutil.copytree(ROOT / "files", image)
    source = ". ".join(name + " fileOut" for name in names) + ". true"
    subprocess.run([str(args.runner.resolve()), "--directory", str(image),
                    "--eval", source, "--expect", "true"], check=True)
    args.output.mkdir(parents=True)
    for name in names:
        text = (image / (name + ".st")).read_text()
        # Omit only the generated timestamp chunk; preserve exported definitions.
        text = text[text.index("!") + 1:].lstrip()
        text = "\n".join(line.rstrip() for line in text.splitlines()) + "\n"
        (args.output / (name + ".st")).write_text(text)
print(args.output.resolve())
