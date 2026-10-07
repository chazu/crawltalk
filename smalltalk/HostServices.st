Object subclass: #HostServices
    instanceVariableNames: ''
    classVariableNames: ''
    poolDictionaries: ''
    category: 'System-HostServices'!

!HostServices class methodsFor: 'primitives'!
primitive: operation with: arguments
    <primitive: 200>
    self primitiveFailed! !

!HostServices class methodsFor: 'accessing'!
version
    ^self primitive: 0 with: nil!
epoch
    ^self primitive: 1 with: nil! !

!HostServices class methodsFor: 'requests'!
submit: service bytes: bytes timeout: milliseconds
    | arguments |
    service = 'httpGet' ifTrue: [^self get: bytes maximumBytes: 16777216 timeout: milliseconds].
    arguments _ Array with: service with: bytes with: milliseconds with: Semaphore new.
    ^HostRequest on: (self primitive: 2 with: arguments)!
echo: bytes
    ^self submit: 'echo' bytes: bytes timeout: 5000!
get: url
    ^self get: url maximumBytes: 16777216 timeout: 30000!
get: url maximumBytes: maximum
    ^self get: url maximumBytes: maximum timeout: 30000!
get: url maximumBytes: maximum timeout: milliseconds
    | arguments |
    arguments _ Array new: 5.
    arguments at: 1 put: 'httpGet'; at: 2 put: url;
        at: 3 put: milliseconds; at: 4 put: Semaphore new; at: 5 put: maximum.
    ^HostRequest on: (self primitive: 9 with: arguments)!
download: url toFileNamed: fileName maximumBytes: maximum
    ^(self get: url maximumBytes: maximum) downloadToFileNamed: fileName! !

!HostServices class methodsFor: 'system lifecycle'!
prepareSnapshot
    "Interrupt native jobs and deliver their completion signals before saving."
    ^self primitive: 5 with: nil!
collectGarbage
    ^self primitive: 6 with: nil!
log: text
    ^self primitive: 7 with: text! !

Object subclass: #HostRequest
    instanceVariableNames: 'record '
    classVariableNames: ''
    poolDictionaries: ''
    category: 'System-HostServices'!

!HostRequest class methodsFor: 'instance creation'!
on: anArray
    ^self new record: anArray! !

!HostRequest methodsFor: 'private'!
record: anArray
    record _ anArray.
    ^self! !

!HostRequest methodsFor: 'accessing'!
state
    ^#(pending succeeded failed cancelled timedOut interrupted closed) at: (record at: 3) + 1!
isPending
    ^(record at: 3) = 0!
isSuccess
    ^(record at: 3) = 1!
errorText
    ^record at: 5!
statusCode
    ^record at: 7!
isStreaming
    ^record size > 7 and: [record at: 8]!
result
    self isStreaming ifTrue: [^self bodyString].
    self wait.
    self isSuccess ifFalse: [self error: self errorText].
    ^record at: 4!
bodyStream
    self isStreaming ifFalse: [^ReadStream on: self result].
    (record at: 10) isNil ifTrue: [record at: 10 put: (HostBodyStream on: self)].
    ^record at: 10!
bodyString
    ^self bodyString: 65533!
bodyString: maximum
    | output stream next total cached |
    (maximum isInteger and: [maximum >= 0 and: [maximum <= 65533]])
        ifFalse: [self error: 'bodyString maximum must be 0..65533'].
    self isStreaming ifFalse:
        [cached _ self result.
         cached size > maximum ifTrue: [self error: 'body exceeds materialization limit'].
         ^cached].
    cached _ record at: 4.
    cached isNil ifFalse:
        [cached size > maximum ifTrue: [self error: 'body exceeds materialization limit'].
         ^cached].
    stream _ self bodyStream.
    stream hasRead ifTrue: [self error: 'body stream already consumed; read remaining data from the stream'].
    output _ WriteStream on: (String new: maximum).
    total _ 0.
    [(next _ stream nextChunk) isNil] whileFalse:
        [total _ total + next size.
         total > maximum ifTrue:
            [self close. self error: 'body exceeds materialization limit'].
         output nextPutAll: next].
    cached _ output contents.
    record at: 4 put: cached.
    ^cached!
bodyBytes
    ^self bodyBytes: 65533!
bodyBytes: maximum
    | text bytes |
    text _ self bodyString: maximum.
    bytes _ ByteArray new: text size.
    1 to: text size do: [:i | bytes at: i put: (text at: i) asciiValue].
    ^bytes!
downloadToFileNamed: fileName
    | file stream next total |
    self isStreaming ifFalse: [self error: 'download requires a streaming request'].
    stream _ self bodyStream.
    stream hasRead ifTrue: [self error: 'body stream already consumed'].
    file _ FileStream newHostFileNamed: fileName.
    total _ 0.
    "Close the file before reporting a transport failure. A failed download
     leaves an explicitly partial file; existing files are never replaced."
    [(next _ stream nextChunkOrEnd) isNil] whileFalse:
        [file nextPutAll: next. total _ total + next size].
    file shorten; close.
    self checkSuccess.
    ^total! !

!HostRequest methodsFor: 'stream primitives'!
readChunk
    | next |
    [(next _ HostServices primitive: 8 with: record) isNil]
        whileTrue: [(record at: 6) wait].
    next == false ifTrue: [^nil].
    ^next!
checkSuccess
    self isSuccess ifFalse: [self error: self errorText]!
hasFailed
    ^(record at: 3) > 1! !

!HostRequest methodsFor: 'synchronization'!
wait
    (self isStreaming and: [self isPending]) ifTrue:
        [self error: 'Consume bodyStream or bodyString: before waiting on a streaming request'].
    self isPending ifTrue:
        [(record at: 6) wait.
         "Pass the completion signal to any other waiting processes."
         (record at: 6) signal].
    ^self!
cancel
    ^HostServices primitive: 3 with: record!
close
    HostServices primitive: 4 with: record.
    record at: 3 put: 6.
    record at: 4 put: nil.
    record at: 5 put: 'closed'.
    self isStreaming ifTrue: [(record at: 10) isNil ifFalse: [(record at: 10) discard]].
    ^self! !

!PosixFileDirectory methodsFor: 'host downloads'!
createExclusive: fileName
    | file descriptor |
    file _ self initFileName: fileName.
    descriptor _ self doPrimitive: 4 arg1: fileName arg2: nil.
    descriptor isNil ifTrue: [self error: 'Download destination exists or cannot be created'].
    file fd: descriptor.
    file readWrite: Write.
    ^file! !

!FileStream class methodsFor: 'host downloads'!
newHostFileNamed: fileDesignator
    | directory localName |
    directory _ FileDirectory directoryFromName: fileDesignator setFileName: [:localName].
    ^(directory createExclusive: localName) asFileStream! !

Stream subclass: #HostBodyStream
    instanceVariableNames: 'request chunk position ended owner consumed '
    classVariableNames: ''
    poolDictionaries: ''
    category: 'System-HostServices'!

!HostBodyStream class methodsFor: 'instance creation'!
on: aRequest
    ^self basicNew request: aRequest! !

!HostBodyStream methodsFor: 'private'!
request: aRequest
    request _ aRequest.
    chunk _ nil.
    position _ 0.
    ended _ false.
    consumed _ false.
    owner _ nil.
    ^self!
claim
    owner isNil ifTrue: [owner _ Processor activeProcess].
    owner == Processor activeProcess ifFalse: [self error: 'bodyStream has one consuming process']!
discard
    ended _ true.
    chunk _ nil!
fill
    self claim.
    request hasFailed ifTrue: [self discard. ^false].
    (chunk notNil and: [position < chunk size]) ifTrue: [^true].
    ended ifTrue: [^false].
    chunk _ request readChunk.
    position _ 0.
    chunk isNil ifTrue: [ended _ true. ^false].
    ^true! !

!HostBodyStream methodsFor: 'accessing'!
hasRead
    ^consumed!
nextChunkOrEnd
    | next |
    self fill ifFalse: [^nil].
    next _ position = 0 ifTrue: [chunk] ifFalse: [chunk copyFrom: position + 1 to: chunk size].
    chunk _ nil.
    position _ 0.
    consumed _ true.
    ^next!
nextChunk
    | next |
    next _ self nextChunkOrEnd.
    next isNil ifTrue: [request checkSuccess].
    ^next!
next
    self atEnd ifTrue: [^nil].
    position _ position + 1.
    consumed _ true.
    ^chunk at: position!
next: count
    | output |
    (count isInteger and: [count >= 0 and: [count <= 65533]])
        ifFalse: [self error: 'next: count must be 0..65533'].
    output _ WriteStream on: (String new: count).
    [output position < count and: [self atEnd not]] whileTrue: [output nextPut: self next].
    ^output contents!
atEnd
    self fill ifTrue: [^false].
    request checkSuccess.
    ^true!
close
    request close! !

!SystemDictionary methodsFor: 'snapshotting'!
snapshotPrimitive
    HostServices prepareSnapshot.
    ^self hostSnapshotPrimitive!
hostSnapshotPrimitive
    <primitive: 97>
    self primitiveFailed! !

HostServices version = 2 ifFalse: [HostServices error: 'Unsupported host service ABI']!
