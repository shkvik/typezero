#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/values/number_methods/number_methods_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallChecker;
class IndexProofs;
class MathChecker;
class BinaryChecker;
class ConcatChecker;
class MemberChecker;
class TypeRelations;
class LiteralChecker;
class ArgumentChecker;
class GlobalFunctionChecker;

class NumberMethods {
public:
  using ResultStaticCallMade = Result<Expr *, NumberMethodsCheckStaticCallError>;
  using ResultMethodCallMade = Result<Expr *, NumberMethodsCheckNumberMethodError>;

  NumberMethods(SemanticState &owner, SemanticParts &all);

  ResultStaticCallMade checkStaticCall(CallExpr *call, MemberExpr *member);
  ResultMethodCallMade checkNumberMethod(CallExpr *call, MemberExpr *member);

private:
  using ResultConversionMade = Result<Expr *, NumberMethodsCheckConversionError>;

  ResultConversionMade checkConversion(CallExpr *call, MemberExpr *member);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  MathChecker *mathChecker = nullptr;
  BinaryChecker *binaryChecker = nullptr;
  ConcatChecker *concatChecker = nullptr;
  MemberChecker *memberChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  LiteralChecker *literalChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  GlobalFunctionChecker *globalFunctionChecker = nullptr;
};

}
