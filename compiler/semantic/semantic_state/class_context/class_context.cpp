#include "compiler/semantic/semantic_state/class_context/class_context.h"

namespace tz {

void ClassContext::enterClass(ClassDecl *classDecl) {
  this->classes.push_back(classDecl);
}

void ClassContext::leaveClass() {
  this->classes.pop_back();
}

ClassDecl *ClassContext::getCurrentClass() const {
  return this->classes.empty() ? nullptr : this->classes.back();
}

void ClassContext::enterInitializer(FunctionDecl *functionDecl, const VarSymbol *self) {
  this->initializers.push_back({functionDecl, self});
  this->closureThisReady.push_back(true);
}

void ClassContext::leaveInitializer() {
  this->initializers.pop_back();
  this->closureThisReady.pop_back();
}

FunctionDecl *ClassContext::getInitializer() const {
  return this->initializers.empty() ? nullptr : this->initializers.back().fn;
}

const VarSymbol *ClassContext::getInitializerThis() const {
  return this->initializers.empty() ? nullptr : this->initializers.back().self;
}

void ClassContext::enterInitializerClosure(bool thisReady) {
  this->closureThisReady.push_back(thisReady);
}

void ClassContext::leaveInitializerClosure() {
  this->closureThisReady.pop_back();
}

bool ClassContext::isClosureThisReady() const {
  return this->closureThisReady.empty() || this->closureThisReady.back();
}

void ClassContext::enterStaticInit() {
  this->staticInitDepth += 1;
}

void ClassContext::leaveStaticInit() {
  this->staticInitDepth -= 1;
}

bool ClassContext::isInsideStaticInit() const {
  return this->staticInitDepth > 0;
}

void ClassContext::forgetSetStatics() {
  this->setStatics.clear();
}

void ClassContext::markStaticSet(const VarSymbol *variable) {
  this->setStatics.insert(variable);
}

bool ClassContext::isStaticSet(const VarSymbol *variable) const {
  return this->setStatics.contains(variable);
}

void ClassContext::rememberThisMember(llvm::StringRef name) {
  this->thisMember = name;
}

void ClassContext::forgetThisMember() {
  this->thisMember = {};
}

llvm::StringRef ClassContext::getThisMember() const {
  return this->thisMember;
}

}
