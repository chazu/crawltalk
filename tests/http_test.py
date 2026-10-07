"""Streaming HTTP acceptance against a local server, including real image restarts."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
BODY = bytes(range(256)) * 8192  # 2 MiB, binary, larger than the image heap.

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == "/slow":
            time.sleep(0.3)
        body = b"A\x00\xff" if self.path in ("/binary", "/slow") else BODY
        self.send_response(201)
        if self.path != "/unknown-size":
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            for offset in range(0, len(body), 4096):
                self.wfile.write(body[offset:offset + 4096])
        except (BrokenPipeError, ConnectionResetError):
            pass

server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
base = f"http://127.0.0.1:{server.server_port}"

def run(directory, source, expect="true", failure=None, image="streaming.im", load=False):
    command = [sys.argv[1], "--directory", str(directory), "--image", image,
               "--cycles", "1000000000", "--wall-ms", "120000", "--gc-at-start"]
    if load:
        command += ["--file-in", str(ROOT / "smalltalk/HostServices.st")]
    command += ["--eval", source.replace("BASE", base), "--expect", expect, "--gc"]
    result = subprocess.run(command, text=True, capture_output=True, timeout=130,
        env={**os.environ, "NO_PROXY": "127.0.0.1,localhost", "no_proxy": "127.0.0.1,localhost"})
    if failure:
        assert result.returncode != 0 and failure in result.stderr, result.stdout + result.stderr
    else:
        assert result.returncode == 0, result.stdout + result.stderr
    return result

try:
    with tempfile.TemporaryDirectory(prefix="crawltalk-http-") as tmp:
        directory = Path(tmp) / "image"
        shutil.copytree(ROOT / "files", directory)
        run(directory, "Smalltalk snapshotAs: 'streaming' thenQuit: false. true", image="snapshot.im", load=True)
        run(directory, """
            | request bytes |
            request _ HostServices get: 'BASE/binary'.
            bytes _ request bodyBytes: 3.
            (request statusCode = 201) and:
                [bytes size = 3 and: [(bytes at: 1) = 65 and: [(bytes at: 2) = 0 and: [(bytes at: 3) = 255]]]]
        """)
        # Mix character, bounded read, and chunk reads without losing bytes.
        run(directory, """
            | request stream total chunk |
            request _ HostServices get: 'BASE/unknown-size' maximumBytes: 2097152.
            stream _ request bodyStream.
            (stream next asciiValue = 0 and: [(stream next: 2) = (String with: (Character value: 1) with: (Character value: 2))])
                ifFalse: [self error: 'mixed stream reads'].
            total _ 3.
            [stream atEnd] whileFalse:
                [chunk _ stream nextChunk.
                 chunk size > 16384 ifTrue: [self error: 'unbounded chunk'].
                 total _ total + chunk size.
                 HostServices collectGarbage].
            (total = 2097152) and: [request isSuccess and: [stream next isNil]]
        """)
        run(directory, "((HostServices get: 'BASE/large' maximumBytes: 2097152) downloadToFileNamed: 'download.bin') = 2097152")
        assert (directory / "download.bin").read_bytes() == BODY
        # File page numbers above SmallInteger range must work for downloads
        # exceeding the former ~8 MiB page-address limit. Use a sparse file.
        run(directory, """
            | file page |
            file _ Disk createExclusive: 'sparse.bin'.
            page _ file initPageNumber: 20000.
            page page at: 1 put: (page page class == String ifTrue: [$A] ifFalse: [65]).
            page size: 1.
            file write: page.
            file endFile: page.
            file close.
            file _ FileStream oldFileNamed: 'sparse.bin'.
            file position: 10239488.
            page _ file next.
            file close.
            page = $A
        """)
        assert (directory / "sparse.bin").stat().st_size == 10239489
        run(directory, "(HostServices get: 'BASE/large') bodyString: 100", failure="materialization limit")
        run(directory, "(HostServices get: 'BASE/unknown-size' maximumBytes: 20000) bodyString: 30000", failure="download exceeds maximumBytes")
        run(directory, """
            | request |
            request _ HostServices get: 'BASE/large'.
            [request statusCode = 0] whileTrue: [Processor yield].
            request cancel.
            (request state = #cancelled) and: [(HostServices echo: 'released') result = 'released']
        """)
        run(directory, """
            | request |
            request _ HostServices get: 'BASE/slow' maximumBytes: 100 timeout: 20.
            [request isPending] whileTrue: [Processor yield].
            request state = #timedOut
        """)
        # Completed-but-unread native bytes are also interrupted before snapshot.
        run(directory, """
            Smalltalk at: #SavedStream put: (HostServices get: 'BASE/binary').
            [SavedStream isPending] whileTrue: [Processor yield].
            SavedStream isSuccess ifFalse: [self error: 'small transfer failed'].
            Smalltalk snapshotAs: 'unread' thenQuit: false.
            true
        """)
        run(directory, "SavedStream state = #interrupted", image="unread.im")
        run(directory, "SavedStream bodyStream nextChunk", image="unread.im", failure="interrupted")
        run(directory, """
            Smalltalk at: #InFlight put: (HostServices get: 'BASE/large').
            [InFlight statusCode = 0] whileTrue: [Processor yield].
            HostServices collectGarbage.
            Smalltalk snapshotAs: 'inflight' thenQuit: false.
            true
        """)
        run(directory, "InFlight state = #interrupted", image="inflight.im")
        # Partial file is closed before transport failure is reported, and may
        # be reopened/deleted by the caller. No successful truncated download.
        run(directory, "(HostServices get: 'BASE/large' maximumBytes: 20000) downloadToFileNamed: 'partial.bin'", failure="download exceeds maximumBytes")
        assert len((directory / "partial.bin").read_bytes()) <= 20000
        # Existing files are never silently replaced.
        run(directory, "(HostServices get: 'BASE/binary') downloadToFileNamed: 'download.bin'", failure="ERROR")
        assert (directory / "download.bin").read_bytes() == BODY
        print("streaming HTTP: 2 MiB binary file, bounded reads, limits, cancellation, deadlines, GC and restart passed")
finally:
    server.shutdown()
    server.server_close()
    thread.join()
