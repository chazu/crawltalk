# Modernization foundation

The original release-2 image remains the bootstrap seed. The interpreter retains
the Smalltalk-80 bytecodes, object identities, process scheduler and 16-bit object
format. Extensions live in reviewable Smalltalk file-ins and a native service
adapter. This is the first implemented foundation, not a parallel Smalltalk VM.

## Build and run

The supported full build uses CMake 3.16+, a C++14 compiler, Python 3, SDL2 and
libcurl 7.32+. On macOS, the system libcurl works; SDL2 can come from Homebrew.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
python3 scripts/build-image.py
./build/Smalltalk -directory work/development -delay 8
```

`build-image.py` refuses an existing destination. Use `--output work/another-image`
to keep multiple development images. `--runner` selects another build's headless
executable. The script copies the seed's image, sources and changes together,
loads the POSIX and host-service file-ins, saves, then verifies a fresh restart
with an asynchronous request. `build-manifest.json` records the SHA-256 of every
input. Reproducibility here means the same source-defined behavior from the same
seed, not byte-identical images or deterministic native thread scheduling.

For a build without the desktop, pass `-DCRAWLTALK_GUI=OFF`. For a build without
libcurl, pass `-DCRAWLTALK_HTTP=OFF`; HTTP then returns an explicit unknown-service
failure. Echo and the rest of the runtime remain available. The older Makefiles,
Xcode project and Visual Studio project include the new foundation sources but
do not enable libcurl. Use CMake for the full HTTP-enabled build.

## Smalltalk source workflow

`smalltalk/PosixFilePage.st`, `PosixFile.st` and `PosixFileDirectory.st` are complete
exports from `files/snapshot.im`, including class definitions and class-side
methods. Methods missing source in the original files are recovered by the
image's own decompiler. The export timestamp is removed, CR line endings are normalized to LF,
and trailing whitespace is stripped. Regenerate them without changing the seed:

```sh
python3 scripts/export-posix.py --output work/exported-posix
diff -u smalltalk/PosixFile.st work/exported-posix/PosixFile.st
```

The build loads Page, File, Directory, then HostServices. The seed is still
required; this is not a source-only bootstrap of the entire historical image.
The checked-in seed files are never the destination of the build/test scripts.

Edit a `.st` file and load it with the headless runner, or copy it into the
development image's directory and use `(FileStream oldFileNamed: 'Name.st') fileIn`
from a workspace. The runner stages file-ins in the image directory, normalizes
LF/CRLF to the image's CR convention, and lets FileStream read them a chunk at a
time. It removes the staging file after evaluation, so file-ins can exceed one
image String's size limit. The
class browser remains usable; export image edits back into the source files for
review. Keep image, sources and changes together, because compiled methods retain
source offsets. A changes log alone is not the canonical extension source.

## Headless execution

```sh
./build/crawltalk-headless --directory work/development \
  --eval "(HostServices echo: 'hello') result" --expect hello --gc
./build/crawltalk-headless --directory work/development \
  --file-in tests/FoundationTests.st --expect true --display work/desktop.pbm
```

Evaluation uses the existing Compiler and a real Smalltalk Process. The process's
continuation is ordinary Smalltalk bytecode, so an image saved during evaluation
can restart in the GUI and terminate that evaluation without native runner state.
Syntax errors and attempts to open the standard image notifier become CLI errors;
overridden application error handlers still run normally.
An individual `--eval` expression must be text of at most 60000 bytes; use a
file-in for larger source collections.

Options include `--image NAME`, repeatable `--eval SOURCE` and `--file-in FILE`,
`--cycles N` (total budget, default 50 million), `--boot-cycles N` (default 2
million), and `--wall-ms N` (default 30000). `--expect` compares the final result;
integers, booleans, nil, strings and symbols print directly, other objects print
`<object>`. File-ins answer true after completing. `--gc` collects after each
evaluation; `--gc-at-start` collects before the first bytecode after loading.

The Smalltalk clock starts at a fixed epoch with zero elapsed milliseconds.
`--cycles-per-tick N` defaults to 1800 and `--tick-step MS` to 1. `--events FILE`
injects sorted lines containing `millisecond unsigned-input-word`; `#` starts a
comment line. These are raw Smalltalk-80 input words, including any required
timestamp words, rather than Unicode keystrokes. The runner reports how many
were injected. Native service deadlines use a separate monotonic real clock;
native tests inject their own clock through `HostServices::Options`.

## Async services and image lifecycle

Smalltalk-facing examples:

```smalltalk
(HostServices echo: 'hello') result

[Transcript show:
    (HostServices get: 'https://example.com/') bodyString] fork.
```

`submit:bytes:timeout:` returns a `HostRequest`. Buffered services such as echo
support `wait`, `result`, cancellation and multiple waiting processes. HTTP uses
a single-consumer `HostBodyStream`, obtained with `bodyStream`. It supports
`next`, `next:`, `nextChunk`, `atEnd` and `close`; `next` answers a Character,
`next:` and `nextChunk` answer byte Strings, and `nextChunk` answers nil at EOF.
EOF is reported only after successful completion; cancellation, timeout and
transport failures raise the image's normal error. Calls wait on a Semaphore
when no data is ready. A stream's first read/peek claims its consuming process.

```smalltalk
| request stream chunk |
request _ HostServices get: 'https://example.com/data'
    maximumBytes: 50000000 timeout: 120000.
stream _ request bodyStream.
[stream atEnd] whileFalse:
    [chunk _ stream nextChunk.
     "Process this chunk before reading the next one."]
```

`get:` defaults to a 16 MiB download budget and 30-second deadline; callers can
select `maximumBytes:` (1 through 4,294,967,295) and `timeout:` (1–120000 ms).
There is no 16 KiB total HTTP response cap. Bodies can exceed the entire image
heap because only consumed chunks enter it. Native buffering is at most 16 KiB
of payload per request, plus libcurl's buffers and a chunk being copied to the
image. A full buffer blocks the producer and naturally applies network
backpressure. The VM and other Smalltalk processes keep running. Two unconsumed
responses can occupy both workers, so consume, cancel or close each stream.

Use `(request bodyString: 60000)` to explicitly materialize a bounded body.
`bodyString` defaults to the image's 65,533-byte String allocation ceiling;
`bodyBytes:` provides the same bounded convenience as a ByteArray. They preserve
NUL/high bytes, with no implicit UTF-8 decoding. A materialization overflow closes
the request and reports an error. Reading some of a stream and then requesting a
whole body String is rejected, preventing a suffix from being mistaken for the
complete body. Cached complete Strings remain readable. Calling `wait` on a
pending HTTP request is rejected: waiting for completion without consuming a
large response would deadlock against backpressure.

```smalltalk
HostServices download: 'https://example.com/archive'
    toFileNamed: 'archive.bin' maximumBytes: 50000000
```

Downloads copy chunks directly into FileStream without assembling a body String.
They answer the byte count, exclusively create the destination, and never replace
an existing file. A transport failure closes the output before raising an error;
the partial file remains for the caller to inspect or remove. Existing FileStream
write errors use the image's normal notifier. File page addressing now handles
page numbers beyond SmallInteger range; the native filesystem interface still
limits file sizes to 2,147,483,647 bytes. Streaming to other consumers is governed
by the caller's download limit instead. `request downloadToFileNamed:` allows a
custom timeout to be selected when the request is created.

The native runtime owns two worker threads and at most 64 accepted requests with
undelivered results or unread streams. General buffered services retain a 16 KiB
input/output limit. Workers receive only copied native data. The owner thread
polls coalesced readiness notifications and signals image semaphores; body data
is copied only by read primitives. The SDL desktop and interpreter stay on the
main thread. Cancellation chooses a terminal outcome and late worker results
cannot replace it. Unread native buffers expire at the request deadline even
when the HTTP transfer has already completed, so abandoned streams release slots.

Built-in services are `echo`, `delayEcho` (milliseconds, LF, payload), and optional
`httpGet`. HTTP supports HTTP/HTTPS GET, response status and binary bodies. It
keeps TLS verification enabled and returns redirects without following them.
Headers, POST, connection pooling and text decoding remain future work. Libcurl
must support asynchronous DNS. Cancellation wakes backpressured writers
immediately; libcurl's idle progress callback can otherwise take about a second.
Shutdown requests cancellation and joins workers. Custom services must cooperate
with cancellation and bound their external operations.

Before primitive 97, the installed `SystemDictionary>>snapshotPrimitive` wrapper
interrupts pending requests and delivers their semaphore notifications. Normal
bytecode scheduling then places awakened processes in image-owned scheduler
state before the save. Direct primitive-97 calls fail while native bindings
remain, rather than saving a process whose producer would be lost. Completed buffered results remain ordinary image data. Streaming requests with
unread native bytes are interrupted too, even if their HTTP transfer succeeded. No native job is replayed after restart.
If another process submits new native work between the barrier and the snapshot
primitive, the save fails explicitly and can be retried after that work is stopped.

Startup also detects pending or unexhausted streaming `HostRequest` records in externally supplied images,
marks them interrupted and queues their semaphore signals. Those queued signals
participate in both GC schemes, including a collection before the first resumed
bytecode. Request IDs are never reused within a runtime; epochs distinguish VM
sessions and cancellation additionally checks record identity.

Snapshot writes use an exclusively created staging file, flush and close it, then
atomically replace the destination. Write/flush/close/rename failures preserve
the previous image and make the primitive fail. This does not claim full
power-loss durability of the containing directory or roll back changes-log writes.

## Validation and remaining boundaries

CTest covers native concurrency and cancellation races; actual image compilation,
file-ins, binary data, multiple waiters and GC; pending-request snapshot/restart
and orphan recovery; failed snapshot writes; and HTTP against a local server.

```sh
cmake -S . -B build-asan -DCRAWLTALK_GUI=OFF \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCRAWLTALK_SANITIZE=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DCRAWLTALK_GUI=OFF \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCRAWLTALK_THREAD_SANITIZE=ON
cmake --build build-tsan --target hostservices-test
ctest --test-dir build-tsan -R host_services --output-on-failure
```

Native parallelism does not execute Smalltalk bytecodes on multiple cores. The
roughly 2 MiB memory model, object-table limits, legacy string encoding, and
priority scheduler remain. A larger object format and isolated parallel VMs
require separate designs. No general FFI or dynamic plugin loader is introduced.
Desktop keyboard/mouse acceptance and Windows/BSD native execution are separate
from the headless and macOS build checks.

The streaming interface is ABI v2. Rebuild a fresh image with
`python3 scripts/build-image.py --output work/streaming`, or file in the updated
`smalltalk/HostServices.st` on the new VM and save a new image. ABI v1's seven-slot
buffered request records retain their results; pending records are interrupted.
New requests use the documented ten-slot layout. Stream/file tests cover binary
responses larger than the image heap, size limits without Content-Length,
backpressure, cancellation/deadlines, snapshots with unread data, exclusive file
creation, and sparse file addressing beyond 8 MiB.
