#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/async/promise_static_checker/promise_static_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <vector>

namespace tz {

class SemanticParts;
class CallChecker;
class ExprChecker;
class TypeResolver;
class ResultChecker;
class TypeRelations;
class ArgumentChecker;

class PromiseStaticChecker {
public:
  PromiseStaticChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkPromiseStatic(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  using ResultValueIsNotVoid = Result<const Type *, PromiseStaticCheckerNotVoidError>;
  using ResultRaceTypeJoined = Result<const Type *, PromiseStaticCheckerRaceJoinError>;
  using ResultTupleHasValues = Result<const Type *, PromiseStaticCheckerAllValuesError>;
  using ResultEmptyListTyped = Result<const Type *, PromiseStaticCheckerEmptyListError>;
  using ResultPlainValueType = Result<const Type *, PromiseStaticCheckerPlainValueError>;
  using ResultListOfPromises = Result<const Type *, PromiseStaticCheckerPromiseArrayError>;
  using ResultValueTypeKnown = Result<const Type *, PromiseStaticCheckerResolverValueError>;
  using ResultTypeArgsAbsent = Result<const MemberExpr *, PromiseStaticCheckerStaticTypeArgsError>;
  using ResultOneTypeArgOnly =
      Result<const MemberExpr *, PromiseStaticCheckerResolverTypeArgsError>;

  static const Type *awaited(const Type *type);
  static ResultValueTypeKnown resolverValue(const Type *value);
  static ResultTypeArgsAbsent staticTypeArgs(const MemberExpr *member);
  static ResultOneTypeArgOnly resolverTypeArgs(const MemberExpr *member);
  static ResultValueIsNotVoid notVoid(const Type *type, llvm::StringRef fn);
  static ResultTupleHasValues allValues(llvm::ArrayRef<const Type *> tuple);
  static ResultPlainValueType plainValue(const Type *type, llvm::StringRef fn);
  static ResultListOfPromises promiseArray(const Expr *arg, llvm::StringRef fn);
  static ResultEmptyListTyped emptyList(const Type *wantedElement, llvm::StringRef fn);
  static ResultRaceTypeJoined raceJoin(const Type *element, const Type *type, llvm::StringRef fn);
  bool checkPromiseList(Expr *&arg, const Type *wantedElement, const Type *&element,
                        std::vector<const Type *> &tuple, llvm::StringRef fn);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  TypeResolver *typeResolver = nullptr;
  ResultChecker *resultChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
