#pragma once

#include "compiler/program/library/async/promise_static_table/promise_static_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/async/promise_static_checker/promise_static_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <vector>

namespace tz {

class Destination;
class SemanticParts;
class CallChecker;
class ExprChecker;
class TypeResolver;
class ResultChecker;
class TypeRelations;
class PromiseChecker;
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

  const Type *awaited(const Type *type) const;
  ResultValueTypeKnown resolverValue(const Type *value) const;
  ResultTypeArgsAbsent staticTypeArgs(const MemberExpr *member) const;
  ResultOneTypeArgOnly resolverTypeArgs(const MemberExpr *member) const;
  ResultValueIsNotVoid notVoid(const Type *type, llvm::StringRef functionName) const;
  ResultTupleHasValues allValues(llvm::ArrayRef<const Type *> tuple) const;
  ResultPlainValueType plainValue(const Type *type, llvm::StringRef functionName) const;
  ResultListOfPromises promiseArray(const Expr *arg, llvm::StringRef functionName) const;
  ResultEmptyListTyped emptyList(const Type *wantedElement, llvm::StringRef functionName) const;
  ResultRaceTypeJoined raceJoin(const Type *element, const Type *type,
                                llvm::StringRef functionName) const;
  bool checkPromiseList(Expr *&arg, const Type *wantedElement, const Type *&element,
                        std::vector<const Type *> &tuple, llvm::StringRef functionName);

  const Type *wantedValue(const Type *expected) const;
  static bool isSpreadOrHole(const ArrayLitExpr::Element &element);
  const Type *unifiedType(const std::vector<const Type *> &types) const;
  Expr *typeAllCall(CallExpr *call, const Type *element, const std::vector<const Type *> &tuple);
  Expr *reportNoReject(CallExpr *call, MemberExpr *member, const std::string &message);
  const Type *resolverValueType(const MemberExpr *member, const Type *expected);
  Expr *checkWithResolvers(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkResolve(CallExpr *call, MemberExpr *member, const Type *wanted, const std::string &fn);
  Expr *checkCombinator(CallExpr *call, MemberExpr *member, const Type *wanted,
                        const std::string &fn, Builtin builtin);
  bool literalPromiseTypes(ArrayLitExpr *literal, const Type *wantedElement,
                           llvm::StringRef functionName, std::vector<const Type *> &types);
  bool checkValuePromiseList(Expr *&arg, const Type *&element, std::vector<const Type *> &tuple,
                             llvm::StringRef functionName);
  bool checkLiteralPromiseList(ArrayLitExpr *literal, const Type *wantedElement,
                               const Type *&element, std::vector<const Type *> &tuple,
                               llvm::StringRef functionName);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  Destination *destination = nullptr;
  TypeResolver *typeResolver = nullptr;
  ResultChecker *resultChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  PromiseChecker *promiseChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
