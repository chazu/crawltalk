Object subclass: #FoundationTests
    instanceVariableNames: ''
    classVariableNames: ''
    poolDictionaries: ''
    category: 'Tests-Foundation'!

!FoundationTests class methodsFor: 'testing'!
assert: condition description: text
    condition ifFalse: [self error: text]!
run
    self binaryRoundtrip.
    self cancellation.
    self deadlines.
    self multipleWaiters.
    self gcDuringRequest.
    self failure.
    self snapshotBarrier.
    ^true!
binaryRoundtrip
    | bytes result |
    bytes _ ByteArray new: 3.
    bytes at: 1 put: 65; at: 2 put: 0; at: 3 put: 255.
    result _ (HostServices echo: bytes) bodyBytes.
    self assert: result = bytes description: 'binary roundtrip'!
cancellation
    | request |
    request _ HostServices submit: 'delayEcho' bytes: '10000', (String with: (Character value: 10)), 'cancel' timeout: 11000.
    self assert: request cancel description: 'cancel pending request'.
    request wait.
    self assert: request state = #cancelled description: 'cancelled state'.
    self assert: request cancel not description: 'cancel twice'.
    request close.
    self assert: request state = #closed description: 'closed state'!
deadlines
    | request |
    request _ HostServices submit: 'delayEcho' bytes: '10000', (String with: (Character value: 10)), 'timeout' timeout: 10.
    request wait.
    self assert: request state = #timedOut description: 'deadline state'!
multipleWaiters
    | request done count |
    count _ 0.
    done _ Semaphore new.
    request _ HostServices submit: 'delayEcho' bytes: '50', (String with: (Character value: 10)), 'waiters' timeout: 5000.
    [request wait. count _ count + 1. done signal] fork.
    [request wait. count _ count + 1. done signal] fork.
    done wait; wait.
    self assert: count = 2 description: 'both waiters resumed'!
gcDuringRequest
    | request |
    request _ HostServices submit: 'delayEcho' bytes: '100', (String with: (Character value: 10)), 'survived GC' timeout: 5000.
    5 timesRepeat: [HostServices collectGarbage].
    self assert: request result = 'survived GC' description: 'rooted request across collection'!
failure
    | request |
    request _ HostServices submit: 'missing-service' bytes: '' timeout: 1000.
    request wait.
    self assert: request state = #failed description: 'unknown service failed'.
    self assert: request errorText size > 0 description: 'error detail'!
snapshotBarrier
    | request done |
    done _ Semaphore new.
    request _ HostServices submit: 'delayEcho' bytes: '10000', (String with: (Character value: 10)), 'snapshot' timeout: 11000.
    [request wait. done signal] fork.
    Processor yield.
    HostServices prepareSnapshot.
    done wait.
    self assert: request state = #interrupted description: 'snapshot wakes waiter'.
    self assert: (HostServices echo: 'after snapshot') result = 'after snapshot' description: 'new requests after barrier'! !

FoundationTests run!
