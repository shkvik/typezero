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

  struct PromiseList {
    const Type *element = nullptr;
    std::vector<const Type *> tuple;
  };

  const Type *awaited(const Type *type) const;
  ResultValueTypeKnown checkResolverValue(const Type *value) const;
  ResultTypeArgsAbsent checkStaticTypeArgs(const MemberExpr *member) const;
  ResultOneTypeArgOnly checkResolverTypeArgs(const MemberExpr *member) const;
  ResultValueIsNotVoid checkNotVoid(const Type *type, llvm::StringRef functionName) const;
  ResultTupleHasValues checkAllValues(llvm::ArrayRef<const Type *> tuple) const;
  ResultPlainValueType checkPlainValue(const Type *type, llvm::StringRef functionName) const;
  ResultListOfPromises checkPromiseArray(const Expr *arg, llvm::StringRef functionName) const;
  ResultEmptyListTyped checkEmptyList(const Type *wantedElement,
                                      llvm::StringRef functionName) const;
  ResultRaceTypeJoined checkRaceJoin(const Type *element, const Type *type,
                                     llvm::StringRef functionName) const;
  ResultRaceTypeJoined checkRaceElement(const PromiseList &list,
                                        llvm::StringRef functionName) const;
  bool isVoidValue(const Type *type) const;
  std::optional<PromiseList> promiseList(Expr *&arg, const Type *wantedElement,
                                         llvm::StringRef functionName);
  bool isSpreadOrHole(const ArrayLitExpr::Element &element) const;
  std::optional<PromiseList> literalPromiseList(ArrayLitExpr *literal, const Type *wantedElement,
                                                llvm::StringRef functionName);
  std::optional<PromiseList> emptyPromiseList(ArrayLitExpr *literal, const Type *wantedElement,
                                              llvm::StringRef functionName);
  const Type *sameLiteralType(ArrayLitExpr *literal, llvm::ArrayRef<const Type *> types);
  std::optional<std::vector<const Type *>> literalPromiseTypes(ArrayLitExpr *literal,
                                                               const Type *wantedElement,
                                                               llvm::StringRef functionName);
  const Type *unifiedType(llvm::ArrayRef<const Type *> types) const;
  std::optional<PromiseList> valuePromiseList(Expr *&arg, llvm::StringRef functionName);
  std::optional<PromiseList> tuplePromiseList(const Expr *arg, llvm::StringRef functionName);
  const Type *wantedValue(const Type *expected) const;
  Expr *typeAllCall(CallExpr *call, const Type *element, const std::vector<const Type *> &tuple);
  Expr *reportNoReject(CallExpr *call, MemberExpr *member, const std::string &message);
  const Type *resolverValueType(const MemberExpr *member, const Type *expected);
  Expr *checkWithResolvers(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkResolve(CallExpr *call, MemberExpr *member, const Type *wanted,
                     const std::string &functionName);
  Expr *checkCombinator(CallExpr *call, MemberExpr *member, const Type *wanted,
                        const std::string &functionName, Builtin builtin);
  const Type *combinatorElement(const Type *wanted, bool isRace) const;
  const Type *settledElement(const Type *element, const Type *wantedElement) const;
  Expr *typeRaceCall(CallExpr *call, const PromiseList &list, const Type *wantedElement,
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
