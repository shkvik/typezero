#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/bytes/typed_array_checker/typed_array_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

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
  using ResultSetSourceOk = Result<const Expr *, TypedArrayCheckerSetSourceError>;

  static ResultSetSourceOk setSource(const Expr *source);

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

  void checkSetRange(const Expr *target, const Expr *source, const Expr *offset);
};

}
