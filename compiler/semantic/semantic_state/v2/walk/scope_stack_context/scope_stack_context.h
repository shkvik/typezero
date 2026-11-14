#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/names/scopes/scope_entry.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"
#include "compiler/semantic/semantic_state/v2/walk/scope_stack_context/scope_stack_context_errors.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include <optional>
#include <vector>

namespace tz {

struct ScopeSnapshot {
  std::vector<llvm::StringMap<ScopeEntry>> scopes;
  std::vector<llvm::StringMap<Stmt *>> typeScopes;
  std::vector<BlockNames> blockNames;
  std::vector<int> scopeBranches;
  int branchDepth = 0;
};

class ScopeStackContext {
public:
  using ResultEntryDeclared = Result<ScopeEntry, ScopeStackContextDeclareError>;

  void openScope();
  void closeScope();
  void enterBranch();
  void leaveBranch();
  void keepTopScopes();
  void openTypeScope();
  void closeTypeScope();
  void leaveOnlyTypes();
  void leaveBlockNames();
  void alignBranchesWithScopes();
  void restoreBody(ScopeSnapshot saved);
  void enterBlockNames(BlockNames names);
  void addType(llvm::StringRef name, Stmt *decl);
  void keepGroupScopes(const NestedGroup *group);
  void setType(llvm::StringRef name, Stmt *decl);
  void addTopType(llvm::StringRef name, Stmt *decl);
  void enterOnlyTypes(llvm::StringMap<Stmt *> types);
  void replaceTopTypes(llvm::StringMap<Stmt *> types);
  void addTopValue(llvm::StringRef name, ScopeEntry entry);
  void replaceTopValues(llvm::StringMap<ScopeEntry> values);
  void addToCurrentScope(llvm::StringRef name, ScopeEntry entry);


  bool isTopLevel() const;
  bool isInsideBranch() const;
  bool isTopLevelTypes() const;
  bool isBranchedSince(size_t scope) const;
  bool hasTopType(llvm::StringRef name) const;
  bool hasTopValue(llvm::StringRef name) const;
  bool hasTypeInCurrentScope(llvm::StringRef name) const;


  Stmt *findType(llvm::StringRef name) const;
  Stmt *findTopType(llvm::StringRef name) const;
  Stmt *findTypeInCurrentScope(llvm::StringRef name) const;


  std::optional<ScopeEntry> findTopValue(llvm::StringRef name) const;
  std::optional<ScopeEntry> findInCurrentScope(llvm::StringRef name) const;


  size_t getScopeDepth() const;
  ScopeSnapshot saveBody() const;
  llvm::StringMap<Stmt *> getTopTypes() const;
  ScopeEntry findName(llvm::StringRef name) const;
  llvm::StringMap<ScopeEntry> getTopValues() const;
  std::vector<llvm::StringRef> getLocalVarNames() const;
  FuncSymbol *findGenericFunction(const NameExpr *name) const;
  const BlockNames *findDeclaredLater(llvm::StringRef name) const;
  ResultEntryDeclared declare(llvm::StringRef name, ScopeEntry entry);
  std::optional<size_t> findDeclaringScope(const VarSymbol *variable) const;

private:
  using ResultNameIsAllowed = Result<void, ScopeStackContextDeclareError>;

  ResultNameIsAllowed checkNotDeclared(llvm::StringRef name) const;
  ResultNameIsAllowed checkNameAllowed(llvm::StringRef name) const;
  ResultNameIsAllowed checkFunctionOnce(llvm::StringRef name, const ScopeEntry &entry) const;

  int branchDepth = 0;
  std::vector<int> scopeBranches;
  std::vector<BlockNames> blockNames;
  std::vector<llvm::StringMap<ScopeEntry>> scopes;
  std::vector<llvm::StringMap<Stmt *>> typeScopes;
  std::vector<std::vector<llvm::StringMap<Stmt *>>> outerTypeScopes;
};

}
