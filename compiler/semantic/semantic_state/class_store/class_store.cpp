#include "compiler/semantic/semantic_state/class_store/class_store.h"

namespace tz {

unsigned ClassStore::takeClassId() {
  this->classCount += 1;

  return this->classCount;
}

void ClassStore::rememberClassCount(unsigned count) {
  this->classCount = count;
}

void ClassStore::addClass(ClassDecl *classDecl) {
  this->classes.push_back(classDecl);
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

void ClassStore::rememberReturnSource(const FunctionDecl *functionDecl, FunctionDecl *source) {
  this->returnSources[functionDecl] = source;
}

bool ClassStore::hasReturnSource(const FunctionDecl *functionDecl) const {
  return this->returnSources.count(functionDecl) > 0;
}

FunctionDecl *ClassStore::findReturnSource(const FunctionDecl *functionDecl) const {
  return this->returnSources.lookup(functionDecl);
}

void ClassStore::rememberInheritedReturn(const FunctionDecl *functionDecl, FunctionDecl *base) {
  this->inheritedReturns[functionDecl] = base;
}

FunctionDecl *ClassStore::findInheritedReturn(const FunctionDecl *functionDecl) const {
  return this->inheritedReturns.lookup(functionDecl);
}

void ClassStore::markRootSuper(const CallExpr *call) {
  this->rootSupers.insert(call);
}

bool ClassStore::isRootSuper(const CallExpr *call) const {
  return this->rootSupers.contains(call);
}

void ClassStore::markBrokenClass(const ClassDecl *classDecl) {
  this->brokenClasses.insert(classDecl);
}

bool ClassStore::isBrokenClass(const ClassDecl *classDecl) const {
  return this->brokenClasses.contains(classDecl);
}

void ClassStore::enterCompletion(const ClassDecl *classDecl) {
  this->completing.insert(classDecl);
}

void ClassStore::leaveCompletion(const ClassDecl *classDecl) {
  this->completing.erase(classDecl);
}

bool ClassStore::isCompleting(const ClassDecl *classDecl) const {
  return this->completing.contains(classDecl);
}

void ClassStore::rememberSignatureVariants(const Type *interfaceType, llvm::StringRef name,
                                           std::vector<FunctionDecl *> variants) {
  this->signatureVariants[{interfaceType, name.str()}] = std::move(variants);
}

std::vector<FunctionDecl *> ClassStore::getSignatureVariants(const Type *interfaceType,
                                                             llvm::StringRef name) const {
  auto found = this->signatureVariants.find({interfaceType, name.str()});
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
