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
  Expr *checkRadixString(CallExpr *call, MemberExpr *member);
  ResultMethodCallMade checkPlainString(CallExpr *call, MemberExpr *member);
  ResultMethodCallMade checkToFixed(CallExpr *call, MemberExpr *member);
  void walkDigitsArg(CallExpr *call, int64_t lo, int64_t hi, const std::string &what);
  ResultStaticCallMade checkStringStatic(CallExpr *call, MemberExpr *member);
  ResultStaticCallMade checkNumberStatic(CallExpr *call, MemberExpr *member);
  ResultStaticCallMade noSuchProperty(CallExpr *call, llvm::StringRef name, llvm::StringRef owner);
  ResultStaticCallMade checkStringRaw(CallExpr *call);
  ResultStaticCallMade checkFromCodes(CallExpr *call, Builtin builtin);
  void walkCodePoint(const Expr *arg);
  ResultStaticCallMade checkNumberParse(CallExpr *call, MemberExpr *member);
  ResultStaticCallMade checkNumberPredicate(CallExpr *call, Builtin builtin);

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
