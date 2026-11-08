#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/generic_store/generic_store_types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/SMLoc.h"
#include <deque>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace tz {

class GenericStore {
public:
  void rememberCheckingInstance(InstanceRecord *instance);
  void forgetCheckingInstance();
  InstanceRecord *getCheckingInstance() const;
  bool isCheckingInstance() const;
  InstanceRecord *addInstanceRecord();
  void rememberFunctionInstance(const FuncSymbol *generic, std::vector<const Type *> key,
                                InstanceRecord *instance);
  InstanceRecord *findFunctionInstance(const FuncSymbol *generic,
                                       const std::vector<const Type *> &key) const;
  void addPendingInstance(InstanceRecord *instance);
  int countPendingInstances() const;
  InstanceRecord *getPendingInstance(int index) const;
  void forgetPendingInstances();
  void markBrokenGeneric(const FuncSymbol *generic);
  bool isBrokenGeneric(const FuncSymbol *generic) const;
  void markOwnClassReturnReported(const FuncSymbol *generic);
  bool isOwnClassReturnReported(const FuncSymbol *generic) const;
  void rememberFailedCall(llvm::SMLoc loc, const InstanceRecord *instance);
  const InstanceRecord *findFailedCall(llvm::SMLoc loc) const;
  void rememberForcedType(const Expr *expr, const Type *type);
  bool hasForcedTypes() const;
  const Type *findForcedType(const Expr *expr) const;
  void rememberClassInstance(const ClassDecl *generic, std::vector<const Type *> args,
                             ClassDecl *instance);
  ClassDecl *findClassInstance(const ClassDecl *generic,
                               const std::vector<const Type *> &args) const;
  void rememberClassDefaults(const ClassDecl *cls, std::vector<const Type *> defaults);
  std::vector<const Type *> getClassDefaults(const ClassDecl *cls) const;
  void rememberClassCheck(const ClassDecl *instance, InstanceRecord *check);
  InstanceRecord *findClassCheck(const ClassDecl *instance) const;
  void addUnfilledClass(ClassDecl *instance);
  std::vector<ClassDecl *> takeUnfilledClasses();
  void enterFilling(const ClassDecl *instance);
  void leaveFilling();
  int countFillingOf(const ClassDecl *generic) const;
  void markEndlessReported(const ClassDecl *generic);
  bool isEndlessReported(const ClassDecl *generic) const;
  void rememberConstructorFunction(const ClassDecl *generic, const FunctionDecl *ctor,
                                   FuncSymbol *fn);
  FuncSymbol *findConstructorFunction(const ClassDecl *generic, const FunctionDecl *ctor) const;
  void enterInstanceLoc(llvm::SMLoc loc);
  void leaveInstanceLoc();
  llvm::SMLoc getInstanceLoc() const;
  void rememberExactParams(const FuncSymbol *generic, std::vector<bool> exact);
  std::optional<std::vector<bool>> findExactParams(const FuncSymbol *generic) const;
  void rememberNumberParam(const Type *param, const Type *only, const Type *number);
  const Type *findNumberParam(const Type *param, const Type *only) const;
  void enterSubstitution(const Type *type);
  void leaveSubstitution(const Type *type);
  bool isSubstituting(const Type *type) const;

private:
  InstanceRecord *checkingInstance = nullptr;
  std::deque<InstanceRecord> instanceRecords;
  std::map<std::pair<const FuncSymbol *, std::vector<const Type *>>, InstanceRecord *>
      functionInstances;
  std::vector<InstanceRecord *> pendingInstances;
  llvm::SmallPtrSet<const FuncSymbol *, 4> brokenGenerics;
  llvm::SmallPtrSet<const FuncSymbol *, 2> ownClassReturnsReported;
  llvm::DenseMap<const char *, const InstanceRecord *> failedCalls;
  llvm::DenseMap<std::pair<const char *, unsigned>, const Type *> forcedTypes;
  std::map<std::pair<const ClassDecl *, std::vector<const Type *>>, ClassDecl *> classInstances;
  llvm::DenseMap<const ClassDecl *, std::vector<const Type *>> classDefaults;
  llvm::DenseMap<const ClassDecl *, InstanceRecord *> classChecks;
  std::vector<ClassDecl *> unfilledClasses;
  std::vector<const ClassDecl *> filling;
  llvm::SmallPtrSet<const ClassDecl *, 2> endlessReported;
  std::map<std::pair<const ClassDecl *, const FunctionDecl *>, FuncSymbol *> constructorFunctions;
  std::vector<llvm::SMLoc> instanceLocs;
  llvm::DenseMap<const FuncSymbol *, std::vector<bool>> exactParams;
  llvm::DenseMap<std::pair<const Type *, const Type *>, const Type *> numberParams;
  llvm::SmallPtrSet<const Type *, 4> substituting;
};

}
