#include "interpreter.h"
#include "hostservices.h"
#include "oops.h"
#include <iostream>
#include <stdexcept>

namespace {
// ABI v2 keeps the first seven slots of v1. Indexes here are C indexes.
enum { Id, Epoch, State, Bytes, Error, Semaphore, Status, Streaming, Exhausted, Stream, RecordSize };
}
Interpreter::~Interpreter() {
    if (hostServices) hostServices->shutdown();
}
void Interpreter::requireOwner() const {
    if (std::this_thread::get_id() != ownerThread)
        throw std::logic_error("Smalltalk heap accessed outside its owner thread");
}
int Interpreter::bytesObjectFor(const std::string& bytes) {
    if (bytes.size() > 16384) throw std::runtime_error("host result exceeds byte limit");
    int oop = memory.instantiateClass_withBytes(ClassStringPointer, static_cast<int>(bytes.size()));
    for (std::size_t i = 0; i < bytes.size(); ++i)
        memory.storeByte_ofObject_withValue(static_cast<int>(i), oop, static_cast<unsigned char>(bytes[i]));
    return oop;
}
void Interpreter::pollHostServices() {
    requireOwner();
    if (!hostServices) return;
    for (auto& completion : hostServices->poll()) {
        auto at = hostRequests.find(completion.id);
        if (at == hostRequests.end()) continue; // a retired request never aliases a new one
        int record = at->second;
        if (memory.fetchPointer_ofObject(Streaming, record) != TruePointer)
            memory.storePointer_ofObject_withValue(Bytes, record, bytesObjectFor(completion.result.bytes));
        memory.storePointer_ofObject_withValue(Error, record, bytesObjectFor(completion.result.error));
        storeInteger_ofObject_withValue(Status, record, completion.result.status);
        storeInteger_ofObject_withValue(State, record, static_cast<int>(completion.result.state));
        asynchronousSignal(memory.fetchPointer_ofObject(Semaphore, record));
        if (completion.retired) {
            memory.storePointer_ofObject_withValue(Exhausted, record, TruePointer);
            hostRequests.erase(at);
            releaseNative(record);
        }
    }
}
void Interpreter::recoverHostRequests() {
    // Old pending records cannot have a native producer in this VM session.
    // The normal snapshot path interrupts them before saving. This also repairs
    // pending records from an externally supplied image at startup.
    int cls;
    try { cls = globalNamed("HostRequest"); } catch (const std::runtime_error&) { return; }
    for (int oop = memory.initialInstanceOf(cls); oop != NilPointer; oop = memory.instanceAfter(oop)) {
        Root request(*this, oop);
        if (memory.fetchWordLengthOf(oop) != 1) continue;
        int record = memory.fetchPointer_ofObject(0, oop);
        if (memory.isIntegerObject(record) || record == NilPointer || memory.fetchClassOf(record) != ClassArrayPointer) continue;
        int size = memory.fetchWordLengthOf(record);
        if (size != RecordSize && size != 7) continue;
        bool unread = size == RecordSize && memory.fetchPointer_ofObject(Streaming, record) == TruePointer &&
            memory.fetchPointer_ofObject(Exhausted, record) != TruePointer;
        if (memory.fetchPointer_ofObject(State, record) != ZeroPointer && !unread) continue;
        storeInteger_ofObject_withValue(State, record, static_cast<int>(HostServices::State::Interrupted));
        if (size == RecordSize) memory.storePointer_ofObject_withValue(Exhausted, record, TruePointer);
        memory.storePointer_ofObject_withValue(Error, record, bytesObjectFor("interrupted by image restart"));
        int semaphore = memory.fetchPointer_ofObject(Semaphore, record);
        if (!memory.isIntegerObject(semaphore) && memory.fetchClassOf(semaphore) == ClassSemaphorePointer)
            asynchronousSignal(semaphore);
    }
}
void Interpreter::primitiveHostService() {
    requireOwner();
    // Validate without popping, so every primitive failure preserves the stack.
    if (argumentCount != 2 || !memory.isIntegerObject(stackValue(1))) { primitiveFail(); return; }
    int operation = memory.integerValueOf(stackValue(1));
    int args = stackTop();
    if ((operation == 0 || operation == 1 || operation == 5 || operation == 6) && args != NilPointer) {
        primitiveFail(); return;
    }
    auto isClass = [&](int oop, int cls) {
        return !memory.isIntegerObject(oop) && memory.fetchClassOf(oop) == cls;
    };
    auto array = [&](int oop, int size) { return isClass(oop, ClassArrayPointer) && memory.fetchWordLengthOf(oop) == size; };
    auto integer32 = [&](int oop) {
        return memory.isIntegerObject(oop) ? memory.integerValueOf(oop) >= 0 :
            isClass(oop, ClassLargePositiveIntegerPointer) && memory.fetchByteLengthOf(oop) >= 1 && memory.fetchByteLengthOf(oop) <= 4;
    };
    auto answer = [&](int oop) { pop(3); push(oop); };
    if (operation == 0) { answer(memory.integerObjectOf(2)); return; }
    if (operation < 1 || operation > 9) { primitiveFail(); return; }
    if (!hostServices) hostServices.reset(new HostServices);
    if (operation == 1) { answer(bytesObjectFor(hostServices->epoch())); return; }
    if (operation == 2 || operation == 9) {
        bool streaming = operation == 9;
        if (!array(args, streaming ? 5 : 4)) { primitiveFail(); return; }
        int service = memory.fetchPointer_ofObject(0, args);
        int input = memory.fetchPointer_ofObject(1, args);
        int timeout = memory.fetchPointer_ofObject(2, args);
        int semaphore = memory.fetchPointer_ofObject(3, args);
        if (!isClass(service, ClassStringPointer) ||
            (!isClass(input, ClassStringPointer) && !isClass(input, globalNamed("ByteArray"))) ||
            !integer32(timeout) || !isClass(semaphore, ClassSemaphorePointer)) { primitiveFail(); return; }
        auto timeoutMs = positive32BitValueOf(timeout);
        std::uint32_t maximumDownload = 0;
        if (streaming) {
            int limit = memory.fetchPointer_ofObject(4, args);
            if (!integer32(limit)) { primitiveFail(); return; }
            maximumDownload = positive32BitValueOf(limit);
            if (!maximumDownload) { primitiveFail(); return; }
        }
        if (!timeoutMs || timeoutMs > 120000 || memory.fetchByteLengthOf(service) > 64 || memory.fetchByteLengthOf(input) > 16384) {
            primitiveFail(); return;
        }
        Root record(*this, memory.instantiateClass_withPointers(ClassArrayPointer, RecordSize));
        storeInteger_ofObject_withValue(Id, record.oop, 0);
        memory.storePointer_ofObject_withValue(Epoch, record.oop, bytesObjectFor(hostServices->epoch()));
        storeInteger_ofObject_withValue(State, record.oop, 0);
        memory.storePointer_ofObject_withValue(Semaphore, record.oop, semaphore);
        storeInteger_ofObject_withValue(Status, record.oop, 0);
        memory.storePointer_ofObject_withValue(Streaming, record.oop, streaming ? TruePointer : FalsePointer);
        memory.storePointer_ofObject_withValue(Exhausted, record.oop, streaming ? FalsePointer : TruePointer);
        try {
            auto id = hostServices->submit(stringFromObject(service), stringFromObject(input), timeoutMs, maximumDownload);
            memory.storePointer_ofObject_withValue(Id, record.oop, positive32BitIntegerFor(id));
            hostRequests.emplace(id, record.oop);
            retainNative(record.oop);
        } catch (const std::runtime_error& e) {
            memory.storePointer_ofObject_withValue(Exhausted, record.oop, TruePointer);
            storeInteger_ofObject_withValue(State, record.oop, static_cast<int>(HostServices::State::Failed));
            memory.storePointer_ofObject_withValue(Error, record.oop, bytesObjectFor(e.what()));
            asynchronousSignal(semaphore);
        }
        answer(record.oop);
        return;
    }
    if (operation == 3 || operation == 4 || operation == 8) {
        if ((!array(args, RecordSize) && !array(args, 7)) || !integer32(memory.fetchPointer_ofObject(Id, args)) ||
            !isClass(memory.fetchPointer_ofObject(Epoch, args), ClassStringPointer)) { primitiveFail(); return; }
        auto id = positive32BitValueOf(memory.fetchPointer_ofObject(Id, args));
        auto at = hostRequests.find(id);
        bool owned = at != hostRequests.end() && at->second == args &&
            stringFromObject(memory.fetchPointer_ofObject(Epoch, args)) == hostServices->epoch();
        if (operation == 8) {
            if (!array(args, RecordSize) || memory.fetchPointer_ofObject(Streaming, args) != TruePointer) { primitiveFail(); return; }
            if (!owned) {
                if (memory.fetchPointer_ofObject(Exhausted, args) != TruePointer) { primitiveFail(); return; }
                answer(FalsePointer); return;
            }
            // A single consumer acknowledges readiness. This prevents an excess
            // signal per chunk from accumulating during fast, nonblocking reads.
            int semaphore = memory.fetchPointer_ofObject(Semaphore, args);
            storeInteger_ofObject_withValue(ExcessSignalsIndex, semaphore, 0);
            auto chunk = hostServices->read(id);
            Root result(*this, chunk.bytes.empty() ? (chunk.end ? FalsePointer : NilPointer) : bytesObjectFor(chunk.bytes));
            pollHostServices();
            answer(result.oop);
            return;
        }
        bool changed = owned && hostServices->cancel(id, operation == 3 ? HostServices::State::Cancelled : HostServices::State::Closed);
        pollHostServices();
        answer(changed ? TruePointer : FalsePointer);
        return;
    }
    if (operation == 5) {
        hostServices->interruptAll();
        pollHostServices();
        answer(TruePointer);
    } else if (operation == 6) {
        memory.garbageCollect();
        answer(TruePointer);
    } else if (operation == 7) {
        if (!isClass(args, ClassStringPointer)) { primitiveFail(); return; }
        std::cout << stringFromObject(args) << std::endl;
        answer(args);
    }
}
