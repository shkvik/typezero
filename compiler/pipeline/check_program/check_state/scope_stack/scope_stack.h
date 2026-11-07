#pragma once

#include "compiler/pipeline/check_program/check_state/flow_state/flow_state_types.h"
#include "compiler/pipeline/check_program/scopes/scope_entry.h"
#include "compiler/program/ast/ast.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
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

class ScopeStack {
public:
  void openScope();
  void closeScope();
  bool isTopLevel() const;
  bool isTopLevelTypes() const;
  size_t getScopeDepth() const;
  ScopeEntry findName(llvm::StringRef name) const;
  std::optional<ScopeEntry> findInCurrentScope(llvm::StringRef name) const;
  void addToCurrentScope(llvm::StringRef name, ScopeEntry entry);
  std::optional<size_t> findDeclaringScope(const VarSymbol *var) const;
  std::vector<llvm::StringRef> getLocalVarNames() const;
  llvm::StringMap<ScopeEntry> getTopValues() const;
  void replaceTopValues(llvm::StringMap<ScopeEntry> values);
  bool hasTopValue(llvm::StringRef name) const;
  std::optional<ScopeEntry> findTopValue(llvm::StringRef name) const;
  void addTopValue(llvm::StringRef name, ScopeEntry entry);
  void openTypeScope();
  void closeTypeScope();
  Stmt *findType(llvm::StringRef name) const;
  bool hasTypeInCurrentScope(llvm::StringRef name) const;
  Stmt *findTypeInCurrentScope(llvm::StringRef name) const;
  void addType(llvm::StringRef name, Stmt *decl);
  void setType(llvm::StringRef name, Stmt *decl);
  llvm::StringMap<Stmt *> getTopTypes() const;
  void replaceTopTypes(llvm::StringMap<Stmt *> types);
  bool hasTopType(llvm::StringRef name) const;
  Stmt *findTopType(llvm::StringRef name) const;
  void addTopType(llvm::StringRef name, Stmt *decl);
  void enterOnlyTypes(llvm::StringMap<Stmt *> types);
  void leaveOnlyTypes();
  void enterBlockNames(BlockNames names);
  void leaveBlockNames();
  const BlockNames *findDeclaredLater(llvm::StringRef name) const;
  void markUsedInFunction(llvm::StringRef name);
  bool isUsedInFunction(llvm::StringRef name) const;
  void enterBranch();
  void leaveBranch();
  bool isInsideBranch() const;
  bool isBranchedSince(size_t scope) const;
  ScopeSnapshot saveBody() const;
  void keepGroupScopes(const NestedGroup *group);
  void keepTopScopes();
  void alignBranchesWithScopes();
  void restoreBody(ScopeSnapshot saved);

private:
  std::vector<llvm::StringMap<ScopeEntry>> scopes;
  std::vector<llvm::StringMap<Stmt *>> typeScopes;
  std::vector<std::vector<llvm::StringMap<Stmt *>>> outerTypeScopes;
  std::vector<BlockNames> blockNames;
  llvm::StringSet<> namesInFunctions;
  std::vector<int> scopeBranches;
  int branchDepth = 0;
};

}
