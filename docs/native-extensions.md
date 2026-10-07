# Native extension contract, version 2

`HostServices class>>primitive:with:` uses private primitive 200. The interpreter
dispatches to `hostbridge.cpp`; native libraries belong in `hostservices.cpp` or
another service implementation, never in the bytecode interpreter.

## Ownership and lifetime

The thread constructing an Interpreter owns its heap, execution contexts and
scheduler. Public execution, evaluation, GC and completion-delivery entry points
enforce that ownership. Workers receive native byte strings, a deadline and a
cancellation token. A worker must not keep an OOP, read image storage, invoke a
Smalltalk block, or call `asynchronousSignal`.

The adapter keeps pending records in `hostRequests` and registers native roots.
`retainNative`/`releaseNative` update reference counts; `prepareForCollection`
marks the roots and `collectionCompleted` restores their external counts after
tracing. Temporary allocations use `Interpreter::Root`. Queued semaphore signals
are also counted, marked and released on delivery. Allocating a second image
object requires the first one to already have a root or image reference.

No native address is encoded as a SmallInteger. A request ID is a monotonically
allocated unsigned 32-bit integer (represented as LargePositiveInteger when
needed). It is paired with a random session epoch. Retired IDs do not re-enter
the same runtime's namespace; exhaustion is an explicit failure.

## Primitive operations

The primitive has two arguments, an operation SmallInteger and its payload.
Image indexes below are one-based.

| Operation | Payload | Answer |
|---|---|---|
| 0 | nil | ABI version, currently 2 |
| 1 | nil | Current session epoch String |
| 2 | Array: service String, input String/ByteArray, timeout milliseconds, Semaphore | Request record |
| 3 | Request record | Whether pending cancellation won |
| 4 | Request record | Whether closing a pending native request won |
| 5 | nil | Interrupt all pending requests for snapshot; deliver completions; true |
| 6 | nil | Force image garbage collection; true |
| 7 | String | Write to host stdout and answer the String |
| 8 | Streaming request record | Next byte String, nil when not ready, false at EOF |
| 9 | Array: service, input, timeout, Semaphore, maximum download bytes | Streaming request record |

An ABI v2 request record is an Array of ten elements (the first seven retain v1 meaning):

1. Native request ID (0 for a submission rejected by the runtime).
2. Session epoch String.
3. State: 0 pending, 1 succeeded, 2 failed, 3 cancelled, 4 timed out,
   5 interrupted by snapshot/restart, 6 closed.
4. Result byte String, nil before delivery. Embedded NUL and bytes 128–255 survive.
5. Error String, nil before delivery or rejection.
6. Completion Semaphore.
7. Service status SmallInteger, 0 when absent; HTTP uses the response code.
8. Streaming Boolean.
9. Native stream exhausted/retired Boolean (true for buffered requests).
10. Cached HostBodyStream or nil, owned by Smalltalk.

Streaming records leave slot 4 nil unless `bodyString:` explicitly materializes
and caches the complete body. A successful transfer can still have unread native
bytes. Its binding remains rooted until drained or closed. Native polling emits
coalesced readiness and state notifications, followed by exactly one retirement;
it does not copy body bytes into the image. Primitive 8 copies only the bytes
actually requested by the consumer, at most one buffer per call. It acknowledges
readiness by resetting excess semaphore signals, preventing unbounded semaphore
counts during fast reads. The stream has exactly one consuming Smalltalk process;
multiple concurrent readers are rejected by HostBodyStream.

For a streaming native service, call `Task::write` with received bytes and return
an empty `Result::bytes`. `Task::write` splits callbacks of any size, blocks the
producer on a full buffer, and wakes on reads, cancellation, snapshot interruption,
deadline or shutdown. No VM lock is held by a waiting writer. The fixed worker
pool still bounds simultaneous transfers; two unread streams can occupy both
workers. `Task::status` publishes response status once known. The total download
limit is separate from buffer capacity and is checked on actual body bytes,
including responses with no Content-Length. Error paths discard unread native
bytes and deliver failure rather than a successful truncated EOF.

Buffered operation 2 retains its 16 KiB input/output contract. HTTP uses streaming
operation 9; invoking httpGet as a buffered native service fails explicitly.
Timeouts cover queueing, transport and unread native buffers. Even a completed
transfer's unread native bytes expire at its deadline, freeing abandoned slots.
Cancellation or snapshot interruption also retires completed-but-unread streams.
Bytes already read by a consumer cannot be rolled back.

Record contents are private to `HostRequest` and the adapter. Cancellation also
requires the exact registered record object, so a fabricated record cannot
cancel a different request. Smalltalk image code is trusted; arbitrary mutation
of live request records or `become:` on them is outside this interface contract.

Malformed primitive arguments fail without popping the receiver or arguments,
allowing the normal Smalltalk fallback. Runtime rejection (unknown service or
capacity exhaustion) produces a failed request with error detail. Transport
failure is a request outcome, not a primitive failure. HTTP error status codes
are successful transports and remain inspectable via `statusCode`.

## Adding a service

Register a named `HostServices::Service` with `addService` before submitting work.
The callback receives `(const std::string&, const Task&)` and returns `Result`.
The input is an owned copy whose lifetime covers the callback. Return success
or a terminal failure, byte data, an optional error and a status in 0–999.
Exceptions are translated to failed completions. Invalid states, status values,
and oversized output are rejected. Error text is capped at 1024 bytes.

Use `Task::stopped()`, `remainingMilliseconds()` and `waitFor()` to cooperate with
cancellation, timeouts, snapshots and shutdown. `waitFor()` is an interruptible
native wait; it does not block the interpreter. A service must bound allocations
before producing a result, and must not start detached tasks. The callback runs
on one of the fixed worker threads and may overlap other calls to that service.
Shared native library state therefore needs its own documented ownership.

Submission and result delivery are bounded by the runtime's capacity; completed
but undelivered buffered jobs and unread streams still occupy a slot. Buffered
requests deliver one completion. Streaming requests deliver readiness/state
notifications and one retirement. Cancellation/timeout can deliver before a running
callback exits; that callback's late result is discarded. At most the fixed
worker count of cancelled callbacks can still retain their native input copies.
Results from different requests have no promised delivery order. The VM calls
`pollHostServices` at batch boundaries; each poll drains at most the bounded
request population. Shutdown is performed by the runtime owner and joins all
workers. An injected native clock must be monotonic and safe to read on workers.

Smalltalk remains responsible for the public API, domain objects and policy.
For example, the HTTP primitive supplies bytes and a status; header models,
authentication helpers, retries and decoding belong in Smalltalk classes unless
they require another narrowly defined native operation.

The image format is unchanged. Incompatible record or primitive payload changes
must increment the ABI version and provide an explicit image migration. To migrate
an ABI v1 development image, file in the new `smalltalk/HostServices.st` using the
v2 VM and save a new snapshot, or rebuild from the seed. HostRequest's instance
layout remains one record field. Seven-slot v1 buffered records remain readable;
pending v1 records are interrupted at startup, and new requests use ten slots.
Completed v1 buffered results survive the migration. Reserve
new private primitive numbers in this document rather than reusing historical
slots; 128 and 130–133 retain the filesystem bindings. Directory primitive 131
adds operation 4 for exclusive file creation. Downloads use it through
`FileStream class>>newHostFileNamed:` to avoid the legacy directory's replacing
create path. POSIX page addressing accepts LargePositiveInteger page numbers,
with checked signed-32-bit file offsets (file size at most 2,147,483,647 bytes).

The HTTP implementation follows libcurl's [thread ownership rules](https://curl.se/libcurl/c/threadsafe.html),
[progress callback cancellation](https://curl.se/libcurl/c/CURLOPT_XFERINFOFUNCTION.html),
and [signal-free timeout requirements](https://curl.se/libcurl/c/CURLOPT_NOSIGNAL.html).
