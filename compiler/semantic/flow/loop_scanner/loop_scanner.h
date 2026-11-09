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
    bool grows = false;
    bool shrinks = false;
    bool releases = false;
  };

  static bool breaksOut(const PlacedNodes &nodes);
  static bool awaitsInside(const PlacedNodes &nodes);
  static int64_t literalStep(const Expr *expr);
  static int64_t changeDelta(const Expr *expr);
  static const Expr *changedTarget(const Expr *expr);
  static int64_t assignDelta(const AssignExpr *assign);
  static bool sameName(const Expr *expr, const Expr *target);
  static llvm::StringSet<> declaredNames(const PlacedNodes &nodes);
  static bool leavesLoop(const BreakStmt *exit, const TreePlace &place);
  static void addTargetEffects(const Expr *target, ArrayEffects &effects);
  static bool passesNamedFunction(const CallExpr *call, int callbackArgs);
  static void addArrayMethodEffects(llvm::StringRef name, ArrayEffects &effects);
  static bool arrayMethodRunsCode(const CallExpr *call, llvm::StringRef name);
  static bool resultMethodRunsCode(const CallExpr *call, llvm::StringRef name);
  static llvm::StringMap<LoopScan::Change> changedVariables(const PlacedNodes &nodes);
  bool namesIterator(const Expr *expr) const;
  bool iteratesInStmt(const Stmt *stmt) const;
  bool isConsoleOrMath(const MemberExpr *member) const;
  bool isPlainGlobalFunction(const Expr *callee) const;
  llvm::StringSet<> writtenFields(const PlacedNodes &nodes) const;
  bool runsOtherCode(const Expr *expr, const llvm::StringSet<> &declared) const;
  bool methodByNameRunsCode(const CallExpr *call, const MemberExpr *member) const;
  bool callRunsOtherCode(const CallExpr *call, const llvm::StringSet<> &declared) const;
  bool callsFunctions(const PlacedNodes &nodes, const llvm::StringSet<> &declared) const;
  LoopScan scanNodes(const PlacedNodes &nodes, const llvm::StringSet<> &declared) const;
  const Type *getTypeBeforeLoop(const Expr *expr, const llvm::StringSet<> &declared) const;
  ArrayEffects arrayEffects(const PlacedNodes &nodes, const llvm::StringSet<> &declared) const;
  void addCallEffects(const CallExpr *call, const llvm::StringSet<> &declared,
                      ArrayEffects &effects) const;

  SemanticState &state;
  Scopes *scopes = nullptr;
  CallChecker *callChecker = nullptr;
  KeyedFieldChecker *keyedFieldChecker = nullptr;
};

}
