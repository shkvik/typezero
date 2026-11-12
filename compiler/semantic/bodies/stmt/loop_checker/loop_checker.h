#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/stmt/loop_checker/loop_checker_errors.h"
#include "compiler/semantic/flow/tree_place/tree_place.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class FlowChecker;
class SemanticParts;
class Scopes;
class LoopBounds;
class DictChecker;
class ExprChecker;
class JumpChecker;
class LoopScanner;
class StmtChecker;
class AsyncChecker;
class MatchChecker;
class TruthChecker;
class TupleChecker;
class ObjectChecker;
class ResultChecker;
class ConstantFolder;
class EmitterChecker;
class GenericChecker;
class IteratorMethods;
class GeneratorChecker;

class LoopChecker {
public:
  LoopChecker(SemanticState &owner, SemanticParts &all);

  void walkFor(ForStmt *stmt);
  void walkWhile(WhileStmt *stmt);
  void walkForOf(ForOfStmt *stmt);
  void walkDoWhile(DoWhileStmt *stmt);
  void walkAwaitLoopBody(Stmt *body);

private:
  using ResultIterableType = Result<const Type *, LoopCheckerIterableTypeError>;
  using ResultBindingShape = Result<const Type *, LoopCheckerBindingShapeError>;
  using ResultBindingTyped = Result<const VarDeclarator *, LoopCheckerBindingTypeError>;

  ResultIterableType iterableType(const Type *type) const;
  ResultBindingTyped bindingType(const ForOfStmt *stmt) const;
  ResultBindingShape bindingShape(const ForOfStmt *stmt, const Type *element) const;

  void walkBody(Stmt *body);
  void walkLoopBody(Stmt *body);
  void walkForStep(ForStmt *stmt);
  void walkSource(ForOfStmt *stmt);
  void walkIterable(ForOfStmt *stmt);
  bool isListParam(const NameExpr *name);
  bool isAsyncOnly(const Type *type) const;
  Expr *listElements(const NameExpr *name);
  bool isConcurrentMap(const Type *type) const;
  const Type *iteratedElement(ForOfStmt *stmt);
  bool isOptionalIterable(const Type *type) const;
  void walkScannedBody(Stmt *body, const LoopScan &scan);
  IntegerRange literalRange(const ArrayLitExpr *literal) const;
  void addLiteralBounds(const ForOfStmt *stmt, const Type *element);
  const Type *boundElement(const ForOfStmt *stmt, const Type *element);
  bool isContinuingTo(const Stmt *body, llvm::ArrayRef<llvm::StringRef> labels) const;
  Flow flowAfter(const LoopScan &scan, const Flow &whenFalse, const Flow &head) const;
  std::optional<int64_t> elementConstant(const ArrayLitExpr::Element &element) const;
  bool isContinuingLoop(const ContinueStmt *next, const TreePlace &place,
                        llvm::ArrayRef<llvm::StringRef> labels) const;
  void enterStepFlow(const Stmt *body, llvm::ArrayRef<llvm::StringRef> labels, const Flow &head,
                     const Flow &whenTrue);

  SemanticState &state;
  Scopes *scopes = nullptr;
  LoopBounds *loopBounds = nullptr;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  JumpChecker *jumpChecker = nullptr;
  LoopScanner *loopScanner = nullptr;
  StmtChecker *stmtChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  AsyncChecker *asyncChecker = nullptr;
  MatchChecker *matchChecker = nullptr;
  TruthChecker *truthChecker = nullptr;
  TupleChecker *tupleChecker = nullptr;
  ObjectChecker *objectChecker = nullptr;
  ResultChecker *resultChecker = nullptr;
  ConstantFolder *constantFolder = nullptr;
  EmitterChecker *emitterChecker = nullptr;
  GenericChecker *genericChecker = nullptr;
  IteratorMethods *iteratorMethods = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
};

}
