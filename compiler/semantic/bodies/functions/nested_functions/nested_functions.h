#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/functions/nested_functions/nested_functions_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/StringSet.h"

namespace tz {

class SemanticParts;
class CallCycles;
class ReturnInferrer;
class FunctionChecker;

class NestedFunctions {
public:
  NestedFunctions(SemanticState &owner, SemanticParts &all);

  void finishGroup(NestedGroup *group);
  NestedGroup *declareNested(BlockStmt *block);
  void noteUse(VarSymbol *variable, llvm::SMLoc loc);
  FunctionDecl *namedFunction(const Expr *expr) const;
  FunctionDecl *variableFunction(const VarSymbol *variable) const;

private:
  using ResultUseInOrder = Result<const NestedGroup::Use *, NestedFunctionsUseOrderError>;

  ResultUseInOrder checkUseOrder(const NestedGroup *group, const NestedGroup::Use &use) const;

  void walkMemberBody(FunctionDecl *functionDecl);
  const NameExpr *assignedName(const Expr *expr) const;
  void rememberGroupUse(VarSymbol *variable, llvm::SMLoc loc);
  void declareMember(NestedGroup *group, FunctionDecl *functionDecl);
  std::vector<FunctionDecl *> reachedFrom(FunctionDecl *start) const;
  void walkUseOrder(const NestedGroup *group, const NestedGroup::Use &use);
  std::vector<FunctionDecl *> nestedMembers(const BlockStmt *block) const;
  bool isUsingAny(const Stmt *stmt, const llvm::StringSet<> &names) const;
  void linkSibling(FunctionDecl *functionDecl, FunctionDecl *sibling) const;
  void addDeclaredNames(const VarDeclStmt *varDecl, NestedGroup *group) const;
  void captureVariable(FunctionDecl *functionDecl, VarSymbol *variable) const;
  void collectDeclaredNames(const BlockStmt *block, NestedGroup *group) const;
  std::vector<FunctionDecl *> functionsInside(const VarSymbol *variable) const;
  void collectAssigned(const Stmt *stmt, std::vector<llvm::StringRef> &names) const;
  bool isGroupMate(const FunctionDecl *functionDecl, const VarSymbol *variable) const;
  VarSymbol *capturedLater(const NestedGroup *group, const NestedGroup::Use &use) const;
  std::optional<size_t> firstUseOf(const BlockStmt *block, const llvm::StringSet<> &names) const;
  VarSymbol *laterCapture(const FunctionDecl *functionDecl, const NestedGroup *group,
                          const NestedGroup::Use &use) const;

  SemanticState &state;
  CallCycles *callCycles = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  FunctionChecker *functionChecker = nullptr;
};

}
