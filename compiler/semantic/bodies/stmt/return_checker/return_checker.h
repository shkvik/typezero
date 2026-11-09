#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/stmt/return_checker/return_checker_errors.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

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

  void checkReturn(ReturnStmt *ret);
  static bool isExit(const Stmt *stmt);
  Expr *checkReturnValue(ReturnStmt *ret, const Type *hint, bool tested);
  static bool alwaysReturns(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels = {});

private:
  using ResultVoidReturnsEmpty = Result<const Type *, ReturnCheckerVoidReturnError>;
  using ResultValueNotRequired = Result<const Type *, ReturnCheckerValueRequiredError>;
  using ResultPromiseTypeMatch = Result<const Type *, ReturnCheckerPromiseReturnError>;
  using ResultGeneratorReturns = Result<const Type *, ReturnCheckerGeneratorReturnError>;
  using ResultAwaitNotReturned = Result<const ReturnStmt *, ReturnCheckerAwaitReturnedError>;
  using ResultInsideOfFunction = Result<const FuncSymbol *, ReturnCheckerEnclosingFunctionError>;
  using ResultConstructorExits = Result<const FuncSymbol *, ReturnCheckerConstructorReturnError>;

  static bool isTrueLiteral(const Expr *expr);
  ResultInsideOfFunction enclosingFunction() const;
  static bool breaksTo(const Stmt *stmt, llvm::ArrayRef<llvm::StringRef> labels);
  static NodeChildren<ReturnPart> returnParts(ReturnPart part);
  static ReturnPart unlabeled(const ReturnPart &part);
  static NodeChildren<const Stmt *> ifBranches(const IfStmt *s);
  static const std::vector<Stmt *> *switchTail(const SwitchStmt *s,
                                               llvm::ArrayRef<llvm::StringRef> labels);
  static bool nodeReturns(const ReturnPart &part, std::vector<bool> &answers);
  static ResultVoidReturnsEmpty voidReturn(const Type *type, const FuncSymbol *fn);
  static ResultValueNotRequired valueRequired(const Type *returned, const FuncSymbol *fn);
  static ResultAwaitNotReturned awaitReturned(const ReturnStmt *ret, const FuncSymbol *fn);
  ResultConstructorExits constructorReturn(const ReturnStmt *ret, const FuncSymbol *fn) const;
  static ResultPromiseTypeMatch promiseReturn(const Type *given, const Type *returned,
                                              const FuncSymbol *fn);
  static ResultGeneratorReturns generatorReturn(const Type *given, const Type *returned,
                                                const FuncSymbol *fn);

  SemanticState &state;
  ByteProofs *byteProofs = nullptr;
  ClassLinks *classLinks = nullptr;
  ExprChecker *exprChecker = nullptr;
  ClassChecker *classChecker = nullptr;
  TruthChecker *truthChecker = nullptr;
  ResultChecker *resultChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConditionFacts *conditionFacts = nullptr;
  EmitterChecker *emitterChecker = nullptr;
  PredicateChecker *predicateChecker = nullptr;
};

}
