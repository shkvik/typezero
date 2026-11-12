#include "compiler/semantic/semantic_state/late_class_store/late_class_store.h"

namespace tz {

void LateClassStore::addLateClass(ClassDecl *classDecl) {
  this->lateClasses.push_back(classDecl);
}

bool LateClassStore::hasUncheckedLateClasses() const {
  return this->checkedCount < int(this->lateClasses.size());
}

ClassDecl *LateClassStore::takeUncheckedLateClass() {
  ClassDecl *classDecl = this->lateClasses[this->checkedCount];
  this->checkedCount += 1;

  return classDecl;
}

std::vector<ClassDecl *> LateClassStore::getUncheckedLateClasses() const {
  std::vector<ClassDecl *> unchecked(this->lateClasses.begin() + this->checkedCount,
                                     this->lateClasses.end());

  return unchecked;
}

void LateClassStore::enterDeclaringClass(ClassDecl *classDecl) {
  this->declaringClasses.push_back(classDecl);
}

void LateClassStore::leaveDeclaringClass() {
  this->declaringClasses.pop_back();
}

ClassDecl *LateClassStore::getDeclaringClass() const {
  return this->declaringClasses.empty() ? nullptr : this->declaringClasses.back();
}

void LateClassStore::rememberLateOwner(const ClassDecl *classDecl, InstanceRecord *owner) {
  this->lateOwners[classDecl] = owner;
}

InstanceRecord *LateClassStore::findLateOwner(const ClassDecl *classDecl) const {
  return this->lateOwners.lookup(classDecl);
}

void LateClassStore::addLateErrors(InstanceRecord *owner, const std::vector<Diagnostic> &found) {
  std::vector<Diagnostic> &errors = this->lateErrors[owner];
  for (const Diagnostic &diagnostic : found) {
    if (diagnostic.kind == llvm::SourceMgr::DK_Error) {
      errors.push_back(diagnostic);
    }
  }
}

std::vector<LateError> LateClassStore::getFirstLateErrors() const {
  auto errors = std::vector<LateError>();
  for (const auto &[instance, captured] : this->lateErrors) {
    if (!captured.empty()) {
      errors.push_back({instance, captured.front()});
    }
  }

  return errors;
}

void LateClassStore::addLateMethodName(llvm::StringRef name) {
  this->lateMethodNames.insert(name);
}

bool LateClassStore::hasLateMethodNames() const {
  return !this->lateMethodNames.empty();
}

bool LateClassStore::isLateMethodName(llvm::StringRef name) const {
  return this->lateMethodNames.contains(name);
}

void LateClassStore::rememberClassExpr(const FunctionDecl *functionDecl, bool has) {
  this->classExprHolders[functionDecl] = has;
}

bool LateClassStore::isClassExprKnown(const FunctionDecl *functionDecl) const {
  return this->classExprHolders.count(functionDecl) > 0;
}

bool LateClassStore::hasClassExpr(const FunctionDecl *functionDecl) const {
  return this->classExprHolders.lookup(functionDecl);
}

}
