#pragma once

#include "compiler/program/library/bytes/bytes_method_table/bytes_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/bytes/typed_array_checker/typed_array_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <string>

namespace tz {

class SemanticParts;
class Prelude;
class ByteProofs;
class ExprChecker;
class IndexProofs;
class MathChecker;
class MemberChecker;
class TypeRelations;
class ArgumentChecker;
class BytesNewChecker;
class CallbackChecker;
class IteratorMethods;

class TypedArrayChecker {
public:
  TypedArrayChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkTypedArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkDataViewMethod(CallExpr *call, MemberExpr *member);

private:
  struct TypedArrayCall {
    CallExpr *call = nullptr;
    MemberExpr *member = nullptr;
    const Type *arrayType = nullptr;
    const Type *element = nullptr;
    const Type *copyType = nullptr;
    std::string what;
  };

  using ResultSetSourceOk = Result<const Expr *, TypedArrayCheckerSetSourceError>;
  using ResultIndexProven = Result<const Expr *, TypedArrayCheckerWithIndexError>;
  using ResultMapFitsType = Result<const Expr *, TypedArrayCheckerMapResultFitsError>;

  static ResultSetSourceOk setSource(const Expr *source);
  static TypedArrayCall describeCall(CallExpr *call, MemberExpr *member);
  static Expr *toBuiltinCall(CallExpr *call, Builtin builtin, const Type *result);

  Expr *checkAt(const TypedArrayCall &typed);
  Expr *checkSet(const TypedArrayCall &typed);
  Expr *checkFill(const TypedArrayCall &typed);
  Expr *checkWith(const TypedArrayCall &typed);
  Expr *checkIterator(const TypedArrayCall &typed);
  CallExpr *makeArrayFrom(const TypedArrayCall &typed);
  void walkPositionArgs(CallExpr *call, size_t from);
  IntLitExpr *makeIntLiteral(llvm::SMLoc loc, int value);
  ResultMapFitsType mapResultFits(const TypedArrayCall &typed) const;
  ResultIndexProven withIndex(const Expr *array, const Expr *index) const;
  Expr *checkJoin(const TypedArrayCall &typed, const TypedArrayJoinMethod &join);
  Expr *checkSearch(const TypedArrayCall &typed, const TypedArraySearchMethod &search);
  Expr *checkReverse(const TypedArrayCall &typed, const TypedArrayReverseMethod &reverse);
  Expr *checkPositions(const TypedArrayCall &typed, const TypedArrayPositionsMethod &positions);
  Expr *reportMissingMethod(CallExpr *call, MemberExpr *member, const BytesMethodInfo *found);
  Expr *checkCallbackCall(const TypedArrayCall &typed, BytesMethod method, const Type *expected);
  Expr *checkSort(const TypedArrayCall &typed, const TypedArraySortMethod &sort,
                  const Type *expected);
  void checkSetRange(const Expr *target, const Expr *source, const Expr *offset);

  SemanticState &state;
  Prelude *prelude = nullptr;
  ByteProofs *byteProofs = nullptr;
  ExprChecker *exprChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  MathChecker *mathChecker = nullptr;
  MemberChecker *memberChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  BytesNewChecker *bytesNewChecker = nullptr;
  CallbackChecker *callbackChecker = nullptr;
  IteratorMethods *iteratorMethods = nullptr;
};

}
