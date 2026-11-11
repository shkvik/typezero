#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/stmt/loop_checker/loop_checker_errors.h"
#include "compiler/semantic/flow/tree_place/tree_place.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

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

  void walkWhile(WhileStmt *stmt);
  void walkDoWhile(DoWhileStmt *stmt);
  void walkFor(ForStmt *stmt);
  void walkForOf(ForOfStmt *stmt);
  void walkLoopBody(Stmt *body, bool awaits);

private:
  static bool continuesTo(const Stmt *body, llvm::ArrayRef<llvm::StringRef> labels);
  static bool continuesLoop(const ContinueStmt *next, const TreePlace &place,
                            llvm::ArrayRef<llvm::StringRef> labels);
  using ResultIterableType = Result<const Type *, LoopCheckerIterableTypeError>;
  using ResultBindingTyped = Result<const VarDeclarator *, LoopCheckerBindingTypeError>;
  using ResultBindingShape = Result<const Type *, LoopCheckerBindingShapeError>;

  ResultIterableType iterableType(const Type *type) const;
  static ResultBindingTyped bindingType(const ForOfStmt *stmt);
  static ResultBindingShape bindingShape(const ForOfStmt *stmt, const Type *element);
  void walkIterable(ForOfStmt *stmt);
  bool isListParam(const NameExpr *name);
  Expr *listElements(const NameExpr *name);
  const Type *iteratedElement(ForOfStmt *stmt);
  void addLiteralBounds(const ForOfStmt *stmt, const Type *element);
  IntegerRange literalRange(const ArrayLitExpr *lit) const;

  SemanticState &state;
  Scopes *scopes = nullptr;
  LoopBounds *loopBounds = nullptr;
  DictChecker *dictChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  JumpChecker *jumpChecker = nullptr;
  LoopScanner *loopScanner = nullptr;
  StmtChecker *stmtChecker = nullptr;
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
