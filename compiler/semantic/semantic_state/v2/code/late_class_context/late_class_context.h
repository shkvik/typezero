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

class LateClassContext {
public:
  void leaveDeclaringClass();
  void addLateClass(ClassDecl *classDecl);
  void addLateMethodName(llvm::StringRef name);
  void enterDeclaringClass(ClassDecl *classDecl);
  void rememberClassExpr(const FunctionDecl *functionDecl, bool has);
  void rememberLateOwner(const ClassDecl *classDecl, InstanceRecord *owner);
  void addLateErrors(InstanceRecord *owner, const std::vector<Diagnostic> &found);


  bool hasLateMethodNames() const;
  bool hasUncheckedLateClasses() const;
  bool isLateMethodName(llvm::StringRef name) const;
  bool hasClassExpr(const FunctionDecl *functionDecl) const;
  bool isClassExprKnown(const FunctionDecl *functionDecl) const;


  ClassDecl *takeUncheckedLateClass();
  ClassDecl *getDeclaringClass() const;


  std::vector<LateError> getFirstLateErrors() const;
  std::vector<ClassDecl *> getUncheckedLateClasses() const;
  InstanceRecord *findLateOwner(const ClassDecl *classDecl) const;

private:
  int checkedCount = 0;
  llvm::StringSet<> lateMethodNames;
  std::vector<ClassDecl *> lateClasses;
  std::vector<ClassDecl *> declaringClasses;
  llvm::DenseMap<const FunctionDecl *, bool> classExprHolders;
  llvm::DenseMap<const ClassDecl *, InstanceRecord *> lateOwners;
  std::map<InstanceRecord *, std::vector<Diagnostic>> lateErrors;
};

}
