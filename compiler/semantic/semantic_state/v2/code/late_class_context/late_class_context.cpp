#include "compiler/semantic/semantic_state/v2/code/late_class_context/late_class_context.h"

namespace tz {

void LateClassContext::addLateClass(ClassDecl *classDecl) {
  this->lateClasses.push_back(classDecl);
}

bool LateClassContext::hasUncheckedLateClasses() const {
  return this->checkedCount < int(this->lateClasses.size());
}

ClassDecl *LateClassContext::takeUncheckedLateClass() {
  ClassDecl *classDecl = this->lateClasses[this->checkedCount];
  this->checkedCount += 1;

  return classDecl;
}

std::vector<ClassDecl *> LateClassContext::getUncheckedLateClasses() const {
  std::vector<ClassDecl *> unchecked(this->lateClasses.begin() + this->checkedCount,
                                     this->lateClasses.end());

  return unchecked;
}

void LateClassContext::enterDeclaringClass(ClassDecl *classDecl) {
  this->declaringClasses.push_back(classDecl);
}

void LateClassContext::leaveDeclaringClass() {
  this->declaringClasses.pop_back();
}

ClassDecl *LateClassContext::getDeclaringClass() const {
  return this->declaringClasses.empty() ? nullptr : this->declaringClasses.back();
}

void LateClassContext::rememberLateOwner(const ClassDecl *classDecl, InstanceRecord *owner) {
  this->lateOwners[classDecl] = owner;
}

InstanceRecord *LateClassContext::findLateOwner(const ClassDecl *classDecl) const {
  return this->lateOwners.lookup(classDecl);
}

void LateClassContext::addLateErrors(InstanceRecord *owner, const std::vector<Diagnostic> &found) {
  std::vector<Diagnostic> &errors = this->lateErrors[owner];
  for (const Diagnostic &diagnostic : found) {
    if (diagnostic.kind == llvm::SourceMgr::DK_Error) {
      errors.push_back(diagnostic);
    }
  }
}

std::vector<LateError> LateClassContext::getFirstLateErrors() const {
  auto errors = std::vector<LateError>();
  for (const auto &[instance, captured] : this->lateErrors) {
    if (!captured.empty()) {
      errors.push_back({instance, captured.front()});
    }
  }

  return errors;
}

void LateClassContext::addLateMethodName(llvm::StringRef name) {
  this->lateMethodNames.insert(name);
}

bool LateClassContext::hasLateMethodNames() const {
  return !this->lateMethodNames.empty();
}

bool LateClassContext::isLateMethodName(llvm::StringRef name) const {
  return this->lateMethodNames.contains(name);
}

void LateClassContext::rememberClassExpr(const FunctionDecl *functionDecl, bool has) {
  this->classExprHolders[functionDecl] = has;
}

bool LateClassContext::isClassExprKnown(const FunctionDecl *functionDecl) const {
  return this->classExprHolders.count(functionDecl) > 0;
}

bool LateClassContext::hasClassExpr(const FunctionDecl *functionDecl) const {
  return this->classExprHolders.lookup(functionDecl);
}

}
