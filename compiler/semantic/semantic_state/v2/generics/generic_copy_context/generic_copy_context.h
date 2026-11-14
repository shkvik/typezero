#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/generic_store/generic_store_types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/SMLoc.h"
#include <deque>
#include <map>
#include <utility>
#include <vector>

namespace tz {

class GenericCopyContext {
public:
  void leaveFilling();
  void leaveInstanceLoc();
  void forgetCheckingInstance();
  void forgetPendingInstances();
  void enterInstanceLoc(llvm::SMLoc loc);
  void addUnfilledClass(ClassDecl *instance);
  void enterFilling(const ClassDecl *instance);
  void markBrokenClass(const ClassDecl *classDecl);
  void addPendingInstance(InstanceRecord *instance);
  void markBrokenGeneric(const FuncSymbol *generic);
  void markEndlessReported(const ClassDecl *generic);
  void rememberCheckingInstance(InstanceRecord *instance);
  void markOwnClassReturnReported(const FuncSymbol *generic);
  void rememberForcedType(const Expr *expr, const Type *type);
  void rememberFailedCall(llvm::SMLoc loc, const InstanceRecord *instance);
  void rememberClassCheck(const ClassDecl *instance, InstanceRecord *check);
  void rememberClassDefaults(const ClassDecl *classDecl, std::vector<const Type *> defaults);
  void rememberConstructorFunction(const ClassDecl *generic, const FunctionDecl *constructor,
                                   FuncSymbol *function);
  void rememberFunctionInstance(const FuncSymbol *generic, std::vector<const Type *> key,
                                InstanceRecord *instance);


  bool hasForcedTypes() const;
  bool isCheckingInstance() const;
  bool isBrokenClass(const ClassDecl *classDecl) const;
  bool isBrokenGeneric(const FuncSymbol *generic) const;
  bool isEndlessReported(const ClassDecl *generic) const;
  bool isOwnClassReturnReported(const FuncSymbol *generic) const;


  int countPendingInstances() const;
  int countFillingOf(const ClassDecl *generic) const;


  InstanceRecord *addInstanceRecord();
  InstanceRecord *getCheckingInstance() const;
  InstanceRecord *getPendingInstance(int index) const;
  InstanceRecord *findClassCheck(const ClassDecl *instance) const;
  InstanceRecord *findFunctionInstance(const FuncSymbol *generic,
                                       const std::vector<const Type *> &key) const;


  llvm::SMLoc getInstanceLoc() const;
  std::vector<ClassDecl *> takeUnfilledClasses();
  const Type *findForcedType(const Expr *expr) const;
  const InstanceRecord *findFailedCall(llvm::SMLoc loc) const;
  std::vector<const Type *> getClassDefaults(const ClassDecl *classDecl) const;
  FuncSymbol *findConstructorFunction(const ClassDecl *generic,
                                      const FunctionDecl *constructor) const;

private:
  std::vector<llvm::SMLoc> instanceLocs;
  std::vector<const ClassDecl *> filling;
  std::vector<ClassDecl *> unfilledClasses;
  InstanceRecord *checkingInstance = nullptr;
  std::deque<InstanceRecord> instanceRecords;
  std::vector<InstanceRecord *> pendingInstances;
  llvm::SmallPtrSet<const ClassDecl *, 2> brokenClasses;
  llvm::SmallPtrSet<const ClassDecl *, 2> endlessReported;
  llvm::SmallPtrSet<const FuncSymbol *, 4> brokenGenerics;
  llvm::DenseMap<const ClassDecl *, InstanceRecord *> classChecks;
  llvm::DenseMap<const char *, const InstanceRecord *> failedCalls;
  llvm::SmallPtrSet<const FuncSymbol *, 2> ownClassReturnsReported;
  llvm::DenseMap<const ClassDecl *, std::vector<const Type *>> classDefaults;
  llvm::DenseMap<std::pair<const char *, unsigned>, const Type *> forcedTypes;
  std::map<std::pair<const ClassDecl *, const FunctionDecl *>, FuncSymbol *> constructorFunctions;
  std::map<std::pair<const FuncSymbol *, std::vector<const Type *>>, InstanceRecord *>
      functionInstances;
};

}
