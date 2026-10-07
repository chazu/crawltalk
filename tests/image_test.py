"""Acceptance checks against the real seed, compiler, scheduler, GC and snapshots."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUNNER = Path(sys.argv[1]).resolve()

def run(directory, *arguments, expect="true", failure=None):
    command = [str(RUNNER), "--directory", str(directory), *arguments]
    if expect is not None:
        command += ["--expect", expect]
    result = subprocess.run(command, text=True, capture_output=True, timeout=45)
    if failure:
        assert result.returncode != 0 and failure in result.stderr, result.stdout + result.stderr
    else:
        assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout

with tempfile.TemporaryDirectory(prefix="crawltalk-test-") as tmp:
    directory = Path(tmp) / "image"
    shutil.copytree(ROOT / "files", directory)
    display = Path(tmp) / "desktop.pbm"
    run(directory, "--eval", "3 + 4", "--gc", "--display", str(display), expect="7")
    assert display.read_text().startswith("P1\n1024 808\n")
    run(directory, "--eval", "-2 bitShift: 1", expect="-4")
    run(directory, "--eval", "(1 bitShift: 32) printString", expect="4294967296")
    run(directory, "--eval", "((-3 bitShift: -1) = -2) and: [(-1 bitShift: -16384) = -1 and: [(0 bitShift: 16383) = 0]]")
    run(directory, "--eval", "[", failure="Smalltalk syntax error")
    run(directory, "--eval", "nil error: 'expected test failure'", failure="expected test failure")
    run(directory, "--cycles", "10", failure="execution limit exceeded")
    large_source = Path(tmp) / "large.st"
    large_source.write_text(('"' + 'x' * 200 + '"!\r\n') * 400 + 'Smalltalk at: #LargeFileInPassed put: true!\r\n')
    run(directory, "--file-in", str(large_source), "--eval", "LargeFileInPassed")
    assert not list(directory.glob("crawltalk-filein-*.st"))
    source_args = []
    for name in ["PosixFilePage.st", "PosixFile.st", "PosixFileDirectory.st", "HostServices.st"]:
        source_args += ["--file-in", str(ROOT / "smalltalk" / name)]
    run(directory, *source_args,
        "--file-in", str(ROOT / "tests/FoundationTests.st"), "--gc",
        "--eval", "Smalltalk snapshotAs: 'foundation' thenQuit: false. true")
    run(directory, "--image", "foundation.im", "--gc-at-start",
        "--eval", "(HostServices echo: 'restored') result", expect="restored")
    run(directory, "--image", "foundation.im", "--eval", "HostServices primitive: 2 with: nil",
        failure="a primitive has failed")
    run(directory, "--image", "foundation.im", "--eval", """
        Smalltalk at: #SavedEpoch put: HostServices epoch.
        Smalltalk at: #SavedResult put: (HostServices echo: 'persisted result') wait.
        Smalltalk at: #SavedRequest put:
            (HostServices submit: 'delayEcho'
             bytes: '100000', (String with: (Character value: 10)), 'pending' timeout: 110000).
        Smalltalk at: #WaiterDone put: false.
        [SavedRequest wait. Smalltalk at: #WaiterDone put: true] fork.
        Processor yield.
        Smalltalk snapshotAs: 'pending' thenQuit: false.
        true
    """)
    run(directory, "--image", "pending.im", "--gc-at-start", "--eval", """
        (SavedRequest state = #interrupted) and:
            [WaiterDone and: [SavedEpoch ~= HostServices epoch and:
                [SavedResult result = 'persisted result' and: [SavedRequest cancel not]]]]
    """)
    # A pending record from an external/older image has no native producer.
    run(directory, "--image", "foundation.im", "--eval", """
        Smalltalk at: #Orphan put: (HostServices echo: 'orphan') wait.
        (Orphan instVarAt: 1) at: 3 put: 0.
        Smalltalk at: #OrphanWoke put: false.
        [Orphan wait. Smalltalk at: #OrphanWoke put: true] fork.
        Processor yield.
        Smalltalk snapshotAs: 'orphan' thenQuit: false.
        true
    """)
    run(directory, "--image", "orphan.im", "--gc-at-start", "--eval",
        "(Orphan state = #interrupted) and: [OrphanWoke]")
    run(directory, "--image", "pending.im", "--eval",
        "(HostServices echo: 'second boot') result", expect="second boot")
    # ABI v1's seven-slot buffered records remain readable after filing in v2.
    run(directory, "--image", "foundation.im", "--eval", """
        | record |
        record _ Array new: 7.
        record at: 1 put: 0; at: 2 put: 'v1'; at: 3 put: 1;
            at: 4 put: 'legacy result'; at: 5 put: ''; at: 6 put: Semaphore new; at: 7 put: 0.
        Smalltalk at: #LegacyRequest put: (HostRequest on: record).
        Smalltalk snapshotAs: 'legacy' thenQuit: false.
        true
    """)
    run(directory, "--image", "legacy.im", "--eval", "LegacyRequest result", expect="legacy result")
    events = Path(tmp) / "events.txt"
    events.write_text("0 0\n")
    output = run(directory, "--events", str(events), "--eval", "true")
    assert "events=1" in output, output
    print("image compiler, services, GC, input and snapshot tests passed")
