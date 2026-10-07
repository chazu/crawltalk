from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="crawltalk-snapshot-") as tmp:
    shutil.copy(Path(__file__).resolve().parents[1] / "files/snapshot.im", tmp)
    subprocess.run([sys.argv[1], tmp], check=True)
