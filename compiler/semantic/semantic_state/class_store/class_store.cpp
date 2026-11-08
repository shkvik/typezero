#include "compiler/semantic/semantic_state/class_store/class_store.h"

namespace tz {

unsigned ClassStore::takeClassId() {
  this->classCount += 1;

  return this->classCount;
}

void ClassStore::rememberClassCount(unsigned count) {
  this->classCount = count;
}

void ClassStore::addClass(ClassDecl *cls) {
  this->classes.push_back(cls);
}

std::vector<ClassDecl *> ClassStore::getClasses() const {
  return this->classes;
}

void ClassStore::markClassesLinked() {
  this->classesLinked = true;
}

bool ClassStore::hasLinkedClasses() const {
  return this->classesLinked;
}

void ClassStore::markMethodsDeclared() {
  this->methodsDeclared = true;
}

bool ClassStore::hasDeclaredMethods() const {
  return this->methodsDeclared;
}

void ClassStore::rememberReturnSource(const FunctionDecl *fn, FunctionDecl *source) {
  this->returnSources[fn] = source;
}

bool ClassStore::hasReturnSource(const FunctionDecl *fn) const {
  return this->returnSources.count(fn) > 0;
}

FunctionDecl *ClassStore::findReturnSource(const FunctionDecl *fn) const {
  return this->returnSources.lookup(fn);
}

void ClassStore::rememberInheritedReturn(const FunctionDecl *fn, FunctionDecl *base) {
  this->inheritedReturns[fn] = base;
}

FunctionDecl *ClassStore::findInheritedReturn(const FunctionDecl *fn) const {
  return this->inheritedReturns.lookup(fn);
}

void ClassStore::markRootSuper(const CallExpr *call) {
  this->rootSupers.insert(call);
}

bool ClassStore::isRootSuper(const CallExpr *call) const {
  return this->rootSupers.contains(call);
}

void ClassStore::markBrokenClass(const ClassDecl *cls) {
  this->brokenClasses.insert(cls);
}

bool ClassStore::isBrokenClass(const ClassDecl *cls) const {
  return this->brokenClasses.contains(cls);
}

void ClassStore::enterCompletion(const ClassDecl *cls) {
  this->completing.insert(cls);
}

void ClassStore::leaveCompletion(const ClassDecl *cls) {
  this->completing.erase(cls);
}

bool ClassStore::isCompleting(const ClassDecl *cls) const {
  return this->completing.contains(cls);
}

void ClassStore::rememberSignatureVariants(const Type *iface, llvm::StringRef name,
                                           std::vector<FunctionDecl *> variants) {
  this->signatureVariants[{iface, name.str()}] = std::move(variants);
}

std::vector<FunctionDecl *> ClassStore::getSignatureVariants(const Type *iface,
                                                             llvm::StringRef name) const {
  auto found = this->signatureVariants.find({iface, name.str()});
  if (found == this->signatureVariants.end()) {
    return {};
  }

  return found->second;
}

void ClassStore::markCallReported(const CallExpr *call) {
  this->reportedCalls.insert(call);
}

bool ClassStore::isCallReported(const CallExpr *call) const {
  return this->reportedCalls.contains(call);
}

}
