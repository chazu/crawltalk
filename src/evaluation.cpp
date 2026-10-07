#include "interpreter.h"
#include "oops.h"
#include <algorithm>
#include <stdexcept>

Interpreter::Root::Root(Interpreter& owner, int value) : oop(value), vm(owner) {
    vm.retainNative(oop);
}
Interpreter::Root::~Root() { vm.releaseNative(oop); }
void Interpreter::retainNative(int oop) {
    nativeRoots.push_back(oop);
    memory.increaseReferencesTo(oop);
}
void Interpreter::releaseNative(int oop) {
    auto at = std::find(nativeRoots.begin(), nativeRoots.end(), oop);
    if (at == nativeRoots.end()) throw std::logic_error("unbalanced native root");
    nativeRoots.erase(at);
    memory.decreaseReferencesTo(oop);
}

int Interpreter::globalNamed(const std::string& name) {
    // SystemDictionary is a variable-sized Dictionary: tally, associations.
    for (int i = 1; i < memory.fetchWordLengthOf(SmalltalkPointer); ++i) {
        int association = memory.fetchPointer_ofObject(i, SmalltalkPointer);
        if (association == NilPointer) continue;
        if (stringFromObject(memory.fetchPointer_ofObject(0, association)) == name)
            return memory.fetchPointer_ofObject(1, association);
    }
    throw std::runtime_error("missing image global: " + name);
}
int Interpreter::symbolNamed(const std::string& name) {
    for (int oop = memory.initialInstanceOf(ClassSymbolPointer); oop != NilPointer;
         oop = memory.instanceAfter(oop)) {
        if (stringFromObject(oop) == name) return oop;
    }
    throw std::runtime_error("missing image selector: " + name);
}

void Interpreter::beginEvaluation(const std::string& source) {
    requireOwner();
    if (evaluationContext) throw std::logic_error("evaluation already active");
    if (source.size() > 60000 || source.find('\0') != std::string::npos)
        throw std::runtime_error("evaluation source must be text under 60001 bytes");
    // A real Smalltalk Process keeps snapshots independent of native runner
    // state. On restart its continuation discards the result and terminates
    // the evaluation process, leaving the desktop's stack untouched.
    Root compiler(*this, globalNamed("Compiler"));
    Root selector(*this, symbolNamed("evaluate:"));
    Root text(*this, stringObjectFor(source.c_str()));
    Root terminate(*this, symbolNamed("terminateActive"));
    Root trampoline(*this, memory.instantiateClass_withBytes(ClassCompiledMethod, 9));
    // Initialize literal words before exposing them to the collector.
    memory.storeWord_ofObject_withValue(0, trampoline.oop, 1);
    memory.storeWord_ofObject_withValue(1, trampoline.oop, NilPointer);
    memory.storeWord_ofObject_withValue(2, trampoline.oop, NilPointer);
    memory.storeWord_ofObject_withValue(0, trampoline.oop, 5); // two literals
    memory.storePointer_ofObject_withValue(1, trampoline.oop, schedulerPointer());
    memory.storePointer_ofObject_withValue(2, trampoline.oop, terminate.oop);
    memory.storeByte_ofObject_withValue(6, trampoline.oop, 135); // pop result
    memory.storeByte_ofObject_withValue(7, trampoline.oop, 32);  // Processor
    memory.storeByte_ofObject_withValue(8, trampoline.oop, 209); // terminateActive
    Root context(*this, memory.instantiateClass_withPointers(ClassMethodContextPointer, 38));
    memory.storePointer_ofObject_withValue(MethodIndex, context.oop, trampoline.oop);
    storeInstructionPointerValue_inContext(7, context.oop);
    storeStackPointerValue_inContext(0, context.oop);
    Root process(*this, memory.instantiateClass_withPointers(globalNamed("Process"), 4));
    memory.storePointer_ofObject_withValue(SuspendedContextIndex, process.oop, context.oop);
    storeInteger_ofObject_withValue(PriorityIndex, process.oop,
        fetchInteger_ofObject(PriorityIndex, activeProcess()));
    evaluationContext = context.oop;
    retainNative(evaluationContext);
    sleep(activeProcess());
    transferTo(process.oop);
    checkProcessSwitch();
    push(compiler.oop);
    push(text.oop);
    sendSelector_argumentCount(selector.oop, 1);
}
bool Interpreter::evaluationFinished() const {
    requireOwner();
    return evaluationContext && activeContext == evaluationContext && !newProcessWaiting;
}
std::string Interpreter::finishEvaluation() {
    if (!evaluationFinished()) throw std::logic_error("evaluation has not returned");
    int result = stackTop();
    std::string text;
    if (memory.isIntegerObject(result)) text = std::to_string(memory.integerValueOf(result));
    else if (result == NilPointer) text = "nil";
    else if (result == TruePointer) text = "true";
    else if (result == FalsePointer) text = "false";
    else if (memory.fetchClassOf(result) == ClassStringPointer ||
             memory.fetchClassOf(result) == ClassSymbolPointer) text = stringFromObject(result);
    else text = "<object>";
    suspendActive();
    checkProcessSwitch();
    releaseNative(evaluationContext);
    evaluationContext = 0;
    return text;
}
