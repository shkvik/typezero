#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/stmt/return_checker/return_checker_errors.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class FlowChecker;
class Destination;
class SemanticParts;
class ByteProofs;
class ClassLinks;
class ExprChecker;
class ClassChecker;
class TruthChecker;
class ResultChecker;
class TypeRelations;
class ConditionFacts;
class EmitterChecker;
class PredicateChecker;

struct ReturnPart {
  const Stmt *stmt;
  std::vector<llvm::StringRef> labels;
};

class ReturnChecker {
public:
  ReturnChecker(SemanticState &owner, SemanticParts &all);

  void walkReturn(ReturnStmt *returnStmt);
  bool isExit(const Stmt *stmt) const;
  bool isAlwaysReturning(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels = {}) const;

private:
  using ResultVoidReturnsEmpty = Result<const Type *, ReturnCheckerVoidReturnError>;
  using ResultValueNotRequired = Result<const Type *, ReturnCheckerValueRequiredError>;
  using ResultPromiseTypeMatch = Result<const Type *, ReturnCheckerPromiseReturnError>;
  using ResultGeneratorReturns = Result<const Type *, ReturnCheckerGeneratorReturnError>;
  using ResultAwaitNotReturned = Result<const ReturnStmt *, ReturnCheckerAwaitReturnedError>;
  using ResultInsideOfFunction = Result<const FuncSymbol *, ReturnCheckerEnclosingFunctionError>;
  using ResultConstructorExits = Result<const FuncSymbol *, ReturnCheckerConstructorReturnError>;

  bool isTrueLiteral(const Expr *expr) const;
  bool isBreakingTo(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels) const;
  bool isLabeledBreak(const PlacedStmt &placed, llvm::ArrayRef<llvm::StringRef> labels) const;
  Expr *checkReturnValue(ReturnStmt *returnStmt, const Type *hint, bool isTested);
  Expr *checkPredicateReturn(ReturnStmt *returnStmt, const Type *hint);
  Expr *checkTestedReturn(ReturnStmt *returnStmt);
  void notePredicateExit(const ReturnStmt *returnStmt, const Flow &whenTrue, const Flow &whenFalse);
  ResultInsideOfFunction checkInsideFunction() const;
  ResultConstructorExits checkConstructorReturn(const ReturnStmt *returnStmt,
                                                const FuncSymbol *function) const;
  ResultAwaitNotReturned checkAwaitNotReturned(const ReturnStmt *returnStmt,
                                               const FuncSymbol *function) const;
  ResultValueNotRequired checkValueNotRequired(const Type *returned,
                                               const FuncSymbol *function) const;
  ResultGeneratorReturns checkGeneratorReturn(const Type *given, const Type *returned,
                                              const FuncSymbol *function) const;
  ResultPromiseTypeMatch checkPromiseReturn(const Type *given, const Type *returned,
                                            const FuncSymbol *function) const;
  ResultVoidReturnsEmpty checkVoidReturn(const Type *type, const FuncSymbol *function) const;
  void walkStrayValue(ReturnStmt *returnStmt);
  void walkBareReturn(const ReturnStmt *returnStmt, const FuncSymbol *function,
                      const Type *returned);
  const Type *declaredReturn(const FuncSymbol *function) const;
  void walkConstructorReturn(ReturnStmt *returnStmt, const FuncSymbol *function);
  void walkInferredReturn(ReturnStmt *returnStmt);
  const Type *earlierReturnShape() const;
  bool isShapedReturn(const ReturnStmt *earlier) const;
  void walkAsyncGeneratorReturn(ReturnStmt *returnStmt, const FuncSymbol *function,
                                const Type *returned);
  void walkPromiseReturn(ReturnStmt *returnStmt, const FuncSymbol *function, const Type *returned);
  void walkVoidReturn(ReturnStmt *returnStmt, const FuncSymbol *function);
  void walkValueReturn(ReturnStmt *returnStmt, const FuncSymbol *function, const Type *returned);
  NodeChildren<ReturnPart> returnParts(ReturnPart part) const;
  NodeChildren<const Stmt *> innerStatements(const ReturnPart &part) const;
  NodeChildren<const Stmt *> switchTailStatements(const SwitchStmt *switchStmt,
                                                  llvm::ArrayRef<llvm::StringRef> labels) const;
  ReturnPart unlabeled(const ReturnPart &part) const;
  NodeChildren<const Stmt *> ifBranches(const IfStmt *ifStmt) const;
  NodeChildren<const Stmt *> elseBranch(const IfStmt *ifStmt) const;
  const std::vector<Stmt *> *switchTail(const SwitchStmt *switchStmt,
                                        llvm::ArrayRef<llvm::StringRef> labels) const;
  bool hasDefaultCase(const SwitchStmt *switchStmt) const;
  bool hasLeavingCase(const SwitchStmt *switchStmt, llvm::ArrayRef<llvm::StringRef> labels) const;
  bool hasLoopExit(const Stmt *body, llvm::ArrayRef<llvm::StringRef> labels) const;
  bool isNodeReturning(const ReturnPart &part, std::vector<bool> &answers) const;
  bool isBlockReturning(const BlockStmt *block, llvm::ArrayRef<llvm::StringRef> labels,
                        std::vector<bool> &answers) const;
  bool isIfReturning(const IfStmt *ifStmt, llvm::ArrayRef<llvm::StringRef> labels,
                     std::vector<bool> &answers) const;
  bool isWhileReturning(const WhileStmt *loop, llvm::ArrayRef<llvm::StringRef> labels) const;
  bool isDoWhileReturning(const DoWhileStmt *loop, llvm::ArrayRef<llvm::StringRef> labels) const;
  bool isForReturning(const ForStmt *loop, llvm::ArrayRef<llvm::StringRef> labels) const;
  bool isSwitchReturning(const SwitchStmt *switchStmt, llvm::ArrayRef<llvm::StringRef> labels,
                         std::vector<bool> &answers) const;

  SemanticState &state;
  ByteProofs *byteProofs = nullptr;
  ClassLinks *classLinks = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  Destination *destination = nullptr;
  ClassChecker *classChecker = nullptr;
  TruthChecker *truthChecker = nullptr;
  ResultChecker *resultChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConditionFacts *conditionFacts = nullptr;
  EmitterChecker *emitterChecker = nullptr;
  PredicateChecker *predicateChecker = nullptr;
};

}
