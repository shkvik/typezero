#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

CheckState::CheckState(AstContext &ctx, Diagnostics &diags, SourceFile &file,
                       FeatureRegistry &registry, EventInference &inference)
    : ast(ctx),
      diagnostics(diags),
      features(registry),
      programUse(file),
      eventStore(inference) {}

AstContext &CheckState::getAst() {
  return this->ast;
}

Diagnostics &CheckState::getDiagnostics() {
  return this->diagnostics;
}

FeatureRegistry &CheckState::getFeatures() {
  return this->features;
}

ModuleStates &CheckState::getModuleStates() {
  return this->moduleStates;
}

ScopeStack &CheckState::getScopeStack() {
  return this->scopeStack;
}

FunctionStack &CheckState::getFunctionStack() {
  return this->functionStack;
}

BlockPosition &CheckState::getBlockPosition() {
  return this->blockPosition;
}

FlowState &CheckState::getFlowState() {
  return this->flowState;
}

PredicateStore &CheckState::getPredicateStore() {
  return this->predicateStore;
}

ReturnInference &CheckState::getReturnInference() {
  return this->returnInference;
}

GenericStore &CheckState::getGenericStore() {
  return this->genericStore;
}

AliasStore &CheckState::getAliasStore() {
  return this->aliasStore;
}

TypeContext &CheckState::getTypeContext() {
  return this->typeContext;
}

ClassStore &CheckState::getClassStore() {
  return this->classStore;
}

ClassContext &CheckState::getClassContext() {
  return this->classContext;
}

LateClassStore &CheckState::getLateClassStore() {
  return this->lateClassStore;
}

ExpressionMarks &CheckState::getExpressionMarks() {
  return this->expressionMarks;
}

ConstantStore &CheckState::getConstantStore() {
  return this->constantStore;
}

CallbackFacts &CheckState::getCallbackFacts() {
  return this->callbackFacts;
}

ProgramUse &CheckState::getProgramUse() {
  return this->programUse;
}

LibraryDeclarations &CheckState::getLibraryDeclarations() {
  return this->libraryDeclarations;
}

EventStore &CheckState::getEventStore() {
  return this->eventStore;
}

ReflectStore &CheckState::getReflectStore() {
  return this->reflectStore;
}

StreamStore &CheckState::getStreamStore() {
  return this->streamStore;
}

FileSystemStore &CheckState::getFileSystemStore() {
  return this->fileSystemStore;
}

BytesStore &CheckState::getBytesStore() {
  return this->bytesStore;
}

WideningStore &CheckState::getWideningStore() {
  return this->wideningStore;
}

}
