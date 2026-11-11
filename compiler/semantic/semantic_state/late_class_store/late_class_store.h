#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/diagnostic/diagnostic.h"
#include "compiler/semantic/semantic_state/generic_store/generic_store_types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringSet.h"
#include <map>
#include <vector>

namespace tz {

struct LateError {
  InstanceRecord *instance = nullptr;
  Diagnostic first;
};

class LateClassStore {
public:
  void addLateClass(ClassDecl *cls);
  bool hasUncheckedLateClasses() const;
  ClassDecl *takeUncheckedLateClass();
  std::vector<ClassDecl *> getUncheckedLateClasses() const;
  void enterDeclaringClass(ClassDecl *cls);
  void leaveDeclaringClass();
  ClassDecl *getDeclaringClass() const;
  void rememberLateOwner(const ClassDecl *cls, InstanceRecord *owner);
  InstanceRecord *findLateOwner(const ClassDecl *cls) const;
  void addLateErrors(InstanceRecord *owner, const std::vector<Diagnostic> &found);
  std::vector<LateError> getFirstLateErrors() const;
  void addLateMethodName(llvm::StringRef name);
  bool hasLateMethodNames() const;
  bool isLateMethodName(llvm::StringRef name) const;
  void rememberClassExpr(const FunctionDecl *fn, bool has);
  bool isClassExprKnown(const FunctionDecl *fn) const;
  bool hasClassExpr(const FunctionDecl *fn) const;

private:
  std::vector<ClassDecl *> lateClasses;
  int checkedCount = 0;
  std::vector<ClassDecl *> declaringClasses;
  llvm::DenseMap<const ClassDecl *, InstanceRecord *> lateOwners;
  std::map<InstanceRecord *, std::vector<Diagnostic>> lateErrors;
  llvm::StringSet<> lateMethodNames;
  llvm::DenseMap<const FunctionDecl *, bool> classExprHolders;
};

}
