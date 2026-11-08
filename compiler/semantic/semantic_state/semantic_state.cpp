#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

SemanticState::SemanticState(AstContext &ctx, Diagnostics &diags, SourceFile &file,
                             llvm::ArrayRef<const Feature *> registered, EventInference &inference)
    : ast(ctx),
      diagnostics(diags),
      features(registered),
      inventoryStore(file),
      libraryDeclarations(file.builtins),
      eventStore(inference) {}

AstContext &SemanticState::getAst() {
  return this->ast;
}

Diagnostics &SemanticState::getDiagnostics() {
  return this->diagnostics;
}

llvm::ArrayRef<const Feature *> SemanticState::getFeatures() const {
  return this->features;
}

Completer *SemanticState::getCompleter() const {
  return this->completer;
}

void SemanticState::rememberCompleter(Completer *engine) {
  this->completer = engine;
}

ModuleStates &SemanticState::getModuleStates() {
  return this->moduleStates;
}

ScopeStack &SemanticState::getScopeStack() {
  return this->scopeStack;
}

FunctionStack &SemanticState::getFunctionStack() {
  return this->functionStack;
}

BlockPosition &SemanticState::getBlockPosition() {
  return this->blockPosition;
}

FlowState &SemanticState::getFlowState() {
  return this->flowState;
}

PredicateStore &SemanticState::getPredicateStore() {
  return this->predicateStore;
}

ReturnInference &SemanticState::getReturnInference() {
  return this->returnInference;
}

GenericStore &SemanticState::getGenericStore() {
  return this->genericStore;
}

AliasStore &SemanticState::getAliasStore() {
  return this->aliasStore;
}

TypeContext &SemanticState::getTypeContext() {
  return this->typeContext;
}

ClassStore &SemanticState::getClassStore() {
  return this->classStore;
}

ClassContext &SemanticState::getClassContext() {
  return this->classContext;
}

LateClassStore &SemanticState::getLateClassStore() {
  return this->lateClassStore;
}

ExpressionMarks &SemanticState::getExpressionMarks() {
  return this->expressionMarks;
}

ConstantStore &SemanticState::getConstantStore() {
  return this->constantStore;
}

CallbackFacts &SemanticState::getCallbackFacts() {
  return this->callbackFacts;
}

InventoryStore &SemanticState::getInventoryStore() {
  return this->inventoryStore;
}

LibraryDeclarations &SemanticState::getLibraryDeclarations() {
  return this->libraryDeclarations;
}

EventStore &SemanticState::getEventStore() {
  return this->eventStore;
}

ReflectStore &SemanticState::getReflectStore() {
  return this->reflectStore;
}

StreamStore &SemanticState::getStreamStore() {
  return this->streamStore;
}

FileSystemStore &SemanticState::getFileSystemStore() {
  return this->fileSystemStore;
}

BytesStore &SemanticState::getBytesStore() {
  return this->bytesStore;
}

WideningStore &SemanticState::getWideningStore() {
  return this->wideningStore;
}

}
