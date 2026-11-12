#pragma once

#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class CallChecker;
class KeyedFieldChecker;

class LoopScanner {
public:
  LoopScanner(SemanticState &owner, SemanticParts &all);

  LoopScan scanForOfLoop(const ForOfStmt *loop) const;
  LoopScan scanLoop(llvm::ArrayRef<const Stmt *> stmts, llvm::ArrayRef<const Expr *> exprs) const;

private:
  struct ArrayEffects {
    bool isGrowing = false;
    bool isShrinking = false;
    bool isReleasing = false;
  };

  LoopScan scanNodes(const PlacedNodes &nodes, const llvm::StringSet<> &declared) const;
  bool isBreakingOut(const PlacedNodes &nodes) const;
  bool isLeavingLoop(const BreakStmt *exit, const TreePlace &place) const;
  bool isAwaitingInside(const PlacedNodes &nodes) const;
  llvm::StringSet<> declaredNames(const PlacedNodes &nodes) const;
  void addDeclaredNames(const Stmt *stmt, llvm::StringSet<> &names) const;
  void addDeclaratorNames(const VarDeclStmt *declaration, llvm::StringSet<> &names) const;
  void addFunctionNames(const FunctionDecl *functionDecl, llvm::StringSet<> &names) const;
  llvm::StringMap<LoopScan::Change> changedVariables(const PlacedNodes &nodes) const;
  void addChange(LoopScan::Change &change, const PlacedExpr &placed) const;
  const Expr *changedTarget(const Expr *expr) const;
  int64_t changeDelta(const Expr *expr) const;
  int64_t assignDelta(const AssignExpr *assign) const;
  int64_t literalStep(const Expr *expr) const;
  bool isSameName(const Expr *expr, const Expr *target) const;
  llvm::StringSet<> writtenFields(const PlacedNodes &nodes) const;
  void addWrittenFields(const Expr *target, llvm::StringSet<> &fields) const;
  void addKeyNames(const IndexExpr *index, llvm::StringSet<> &fields) const;
  ArrayEffects arrayEffects(const PlacedNodes &nodes, const llvm::StringSet<> &declared) const;
  void addTargetEffects(const Expr *target, ArrayEffects &effects) const;
  void addCallEffects(const CallExpr *call, const llvm::StringSet<> &declared,
                      ArrayEffects &effects) const;
  void addArrayMethodEffects(llvm::StringRef name, ArrayEffects &effects) const;
  bool isCallingFunctions(const PlacedNodes &nodes, const llvm::StringSet<> &declared) const;
  bool isIteratingInStmt(const Stmt *stmt) const;
  bool isUnpackingIterator(const VarDeclStmt *declaration) const;
  bool isRunningOtherCode(const Expr *expr, const llvm::StringSet<> &declared) const;
  bool isMakingNonArray(const NewExpr *made) const;
  bool isSpreadingIterator(const ArrayLitExpr *list) const;
  bool isCallRunningOtherCode(const CallExpr *call, const llvm::StringSet<> &declared) const;
  bool isMethodByNameRunningCode(const CallExpr *call, const MemberExpr *member) const;
  bool isArrayMethodRunningCode(const CallExpr *call, llvm::StringRef name) const;
  bool isResultMethodRunningCode(const CallExpr *call, llvm::StringRef name) const;
  bool isPassingNamedFunction(const CallExpr *call, int callbackArgs) const;
  bool isConsoleOrMath(const MemberExpr *member) const;
  bool isPlainGlobalFunction(const Expr *callee) const;
  bool isNamingIterator(const Expr *expr) const;
  const Type *getTypeBeforeLoop(const Expr *expr, const llvm::StringSet<> &declared) const;

  SemanticState &state;
  Scopes *scopes = nullptr;
  CallChecker *callChecker = nullptr;
  KeyedFieldChecker *keyedFieldChecker = nullptr;
};

}
