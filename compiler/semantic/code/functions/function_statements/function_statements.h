#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/StringSet.h"
#include <optional>
#include <vector>

namespace tz {

class FunctionStatements {
public:
  explicit FunctionStatements(SemanticState &owner);

  void leaveGroup();
  void noteUse(const NoteUse &task);
  void groupMember(const GroupMember &task);
  void nextStatement(const NextStatement &task);
  void groupUseOrder(const GroupUseOrder &task);
  void walkStatements(const WalkStatements &task);
  void afterStatement(const AfterStatement &task);
  void afterBlockTypes(const AfterBlockTypes &task);
  void enterStatements(const EnterStatements &task);
  void afterGroupMemberReturn(const AfterGroupMemberReturn &task);

private:
  using ResultUseInOrder = Result<const NestedGroup::Use *, CodeFunctionsRegistryUseOrderError>;

  void markGroupCreated(NestedGroup *group);
  void finishStatements(const StatementsDraft &draft);
  void rememberGroupUse(VarSymbol *variable, llvm::SMLoc loc);
  void prepareMember(NestedGroup *group, FunctionDecl *functionDecl);
  void walkUseOrder(const NestedGroup *group, const NestedGroup::Use &use);
  void linkSibling(FunctionDecl *functionDecl, FunctionDecl *sibling) const;
  void collectDeclaredNames(const BlockStmt *block, NestedGroup *group) const;
  void addDeclaredNames(const VarDeclStmt *varDecl, NestedGroup *group) const;
  void captureVariable(FunctionDecl *functionDecl, VarSymbol *variable) const;
  void addDeclaratorNames(const VarDeclStmt *varDecl, BlockNames &names) const;
  void collectAssigned(const Stmt *stmt, std::vector<llvm::StringRef> &names) const;


  bool isDeclarationStmt(const Stmt *stmt) const;
  bool isUsingAny(const Stmt *stmt, const llvm::StringSet<> &names) const;
  bool isGroupMate(const FunctionDecl *functionDecl, const VarSymbol *variable) const;
  bool isDecidedEnd(const BlockStmt *block, size_t index, const NestedGroup *group) const;


  VarSymbol *capturedLater(const NestedGroup *group, const NestedGroup::Use &use) const;
  VarSymbol *laterCapture(const FunctionDecl *functionDecl, const NestedGroup *group,
                          const NestedGroup::Use &use) const;


  std::vector<FunctionDecl *> reachedFrom(FunctionDecl *start) const;
  std::vector<FunctionDecl *> nestedMembers(const BlockStmt *block) const;
  std::vector<FunctionDecl *> functionsInside(const VarSymbol *variable) const;


  BlockNames blockNames(const BlockStmt *block) const;
  const NameExpr *assignedName(const Expr *expr) const;
  ResultUseInOrder checkUseOrder(const NestedGroup *group, const NestedGroup::Use &use) const;
  std::optional<size_t> firstUseOf(const BlockStmt *block, const llvm::StringSet<> &names) const;

  SemanticState &state;
};

}
