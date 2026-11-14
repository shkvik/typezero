#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"

namespace tz {

class SemanticState;

struct ReturnPart {
  const Stmt *stmt;
  std::vector<llvm::StringRef> labels;
};

class FlowReachability {
public:
  explicit FlowReachability(SemanticState &owner);

  bool isExit(const Stmt *stmt) const;
  bool isAlwaysReturning(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels = {}) const;

private:
  bool isTrueLiteral(const Expr *expr) const;
  bool hasDefaultCase(const SwitchStmt *switchStmt) const;
  bool isNodeReturning(const ReturnPart &part, std::vector<bool> &answers) const;
  bool hasLoopExit(const Stmt *body, llvm::ArrayRef<llvm::StringRef> labels) const;
  bool isBreakingTo(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels) const;
  bool isForReturning(const ForStmt *loop, llvm::ArrayRef<llvm::StringRef> labels) const;
  bool isWhileReturning(const WhileStmt *loop, llvm::ArrayRef<llvm::StringRef> labels) const;
  bool isLabeledBreak(const PlacedStmt &placed, llvm::ArrayRef<llvm::StringRef> labels) const;
  bool isDoWhileReturning(const DoWhileStmt *loop, llvm::ArrayRef<llvm::StringRef> labels) const;
  bool hasLeavingCase(const SwitchStmt *switchStmt, llvm::ArrayRef<llvm::StringRef> labels) const;
  bool isIfReturning(const IfStmt *ifStmt, llvm::ArrayRef<llvm::StringRef> labels,
                     std::vector<bool> &answers) const;
  bool isBlockReturning(const BlockStmt *block, llvm::ArrayRef<llvm::StringRef> labels,
                        std::vector<bool> &answers) const;
  bool isSwitchReturning(const SwitchStmt *switchStmt, llvm::ArrayRef<llvm::StringRef> labels,
                         std::vector<bool> &answers) const;


  NodeChildren<const Stmt *> elseBranch(const IfStmt *ifStmt) const;
  NodeChildren<const Stmt *> ifBranches(const IfStmt *ifStmt) const;
  NodeChildren<const Stmt *> innerStatements(const ReturnPart &part) const;
  NodeChildren<const Stmt *> switchTailStatements(const SwitchStmt *switchStmt,
                                                  llvm::ArrayRef<llvm::StringRef> labels) const;


  ReturnPart unlabeled(const ReturnPart &part) const;
  NodeChildren<ReturnPart> returnParts(ReturnPart part) const;
  const std::vector<Stmt *> *switchTail(const SwitchStmt *switchStmt,
                                        llvm::ArrayRef<llvm::StringRef> labels) const;

  SemanticState &state;
};

}
