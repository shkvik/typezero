#include "compiler/semantic/semantic_state/late_class_store/late_class_store.h"

namespace tz {

void LateClassStore::addLateClass(ClassDecl *cls) {
  this->lateClasses.push_back(cls);
}

bool LateClassStore::hasUncheckedLateClasses() const {
  return this->checkedCount < int(this->lateClasses.size());
}

ClassDecl *LateClassStore::takeUncheckedLateClass() {
  ClassDecl *cls = this->lateClasses[this->checkedCount];
  this->checkedCount += 1;

  return cls;
}

std::vector<ClassDecl *> LateClassStore::getUncheckedLateClasses() const {
  std::vector<ClassDecl *> unchecked(this->lateClasses.begin() + this->checkedCount,
                                     this->lateClasses.end());

  return unchecked;
}

void LateClassStore::enterDeclaringClass(ClassDecl *cls) {
  this->declaringClasses.push_back(cls);
}

void LateClassStore::leaveDeclaringClass() {
  this->declaringClasses.pop_back();
}

ClassDecl *LateClassStore::getDeclaringClass() const {
  return this->declaringClasses.empty() ? nullptr : this->declaringClasses.back();
}

void LateClassStore::rememberLateOwner(const ClassDecl *cls, InstanceRecord *owner) {
  this->lateOwners[cls] = owner;
}

InstanceRecord *LateClassStore::findLateOwner(const ClassDecl *cls) const {
  return this->lateOwners.lookup(cls);
}

void LateClassStore::captureLateErrorsOf(InstanceRecord *owner, Diagnostics &diagnostics) {
  diagnostics.setCapture(&this->lateErrors[owner]);
}

std::vector<LateError> LateClassStore::getFirstLateErrors() const {
  std::vector<LateError> errors;
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

void LateClassStore::rememberClassExpr(const FunctionDecl *fn, bool has) {
  this->classExprHolders[fn] = has;
}

bool LateClassStore::isClassExprKnown(const FunctionDecl *fn) const {
  return this->classExprHolders.count(fn) > 0;
}

bool LateClassStore::hasClassExpr(const FunctionDecl *fn) const {
  return this->classExprHolders.lookup(fn);
}

}
