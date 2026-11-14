#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"
#include <string>
#include <vector>

namespace tz {

class SemanticState;

class LoopScans {
public:
  explicit LoopScans(SemanticState &owner);

  LoopScan scanForOfLoop(const ForOfStmt *loop) const;
  LoopScan scanLoop(llvm::ArrayRef<const Stmt *> stmts, llvm::ArrayRef<const Expr *> exprs) const;

private:
  struct ArrayEffects {
    bool isGrowing = false;
    bool isShrinking = false;
    bool isReleasing = false;
  };

  void addTargetEffects(const Expr *target, ArrayEffects &effects) const;
  void addDeclaredNames(const Stmt *stmt, llvm::StringSet<> &names) const;
  void addChange(LoopScan::Change &change, const PlacedExpr &placed) const;
  void addKeyNames(const IndexExpr *index, llvm::StringSet<> &fields) const;
  void addWrittenFields(const Expr *target, llvm::StringSet<> &fields) const;
  void addArrayMethodEffects(llvm::StringRef name, ArrayEffects &effects) const;
  void addDeclaratorNames(const VarDeclStmt *declaration, llvm::StringSet<> &names) const;
  void addFunctionNames(const FunctionDecl *functionDecl, llvm::StringSet<> &names) const;
  void addCallEffects(const CallExpr *call, const llvm::StringSet<> &declared,
                      ArrayEffects &effects) const;


  bool isNamingIterator(const Expr *expr) const;
  bool isIteratingInStmt(const Stmt *stmt) const;
  bool isMakingNonArray(const NewExpr *made) const;
  bool isBreakingOut(const PlacedNodes &nodes) const;
  bool isConsoleOrMath(const MemberExpr *member) const;
  bool isPlainGlobalFunction(const Expr *callee) const;
  bool isAwaitingInside(const PlacedNodes &nodes) const;
  bool isSpreadingIterator(const ArrayLitExpr *list) const;
  bool isSameName(const Expr *expr, const Expr *target) const;
  bool isUnpackingIterator(const VarDeclStmt *declaration) const;
  bool isLeavingLoop(const BreakStmt *exit, const TreePlace &place) const;
  bool isPassingNamedFunction(const CallExpr *call, int callbackArgs) const;
  bool isArrayMethodRunningCode(const CallExpr *call, llvm::StringRef name) const;
  bool isResultMethodRunningCode(const CallExpr *call, llvm::StringRef name) const;
  bool isRunningOtherCode(const Expr *expr, const llvm::StringSet<> &declared) const;
  bool isMethodByNameRunningCode(const CallExpr *call, const MemberExpr *member) const;
  bool isCallRunningOtherCode(const CallExpr *call, const llvm::StringSet<> &declared) const;
  bool isCallingFunctions(const PlacedNodes &nodes, const llvm::StringSet<> &declared) const;


  int64_t changeDelta(const Expr *expr) const;
  int64_t literalStep(const Expr *expr) const;
  int64_t assignDelta(const AssignExpr *assign) const;


  llvm::StringSet<> declaredNames(const PlacedNodes &nodes) const;
  llvm::StringSet<> writtenFields(const PlacedNodes &nodes) const;


  std::vector<std::string> keyNames(const Expr *key) const;
  std::vector<std::string> memberConstants(const MemberExpr *member) const;


  const Expr *changedTarget(const Expr *expr) const;
  llvm::StringMap<LoopScan::Change> changedVariables(const PlacedNodes &nodes) const;
  LoopScan scanNodes(const PlacedNodes &nodes, const llvm::StringSet<> &declared) const;
  const Type *getTypeBeforeLoop(const Expr *expr, const llvm::StringSet<> &declared) const;
  ArrayEffects arrayEffects(const PlacedNodes &nodes, const llvm::StringSet<> &declared) const;

  SemanticState &state;
};

}
