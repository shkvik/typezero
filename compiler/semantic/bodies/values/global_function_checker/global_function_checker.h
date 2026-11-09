#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/values/global_function_checker/global_function_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Prelude;
class CallChecker;
class DateChecker;
class ExprChecker;
class MathChecker;
class RegexChecker;
class TruthChecker;
class BinaryChecker;
class ConcatChecker;
class TypeRelations;
class DecimalChecker;
class ArgumentChecker;

class GlobalFunctionChecker {
public:
  using ResultGlobalCallMade = Result<Expr *, GlobalFunctionCheckerCheckGlobalCallError>;
  using ResultFunctionCalled = Result<Expr *, GlobalFunctionCheckerCheckGlobalFunctionError>;

  GlobalFunctionChecker(SemanticState &owner, SemanticParts &all);

  ResultGlobalCallMade checkGlobalCall(CallExpr *call, NameExpr *name);
  ResultFunctionCalled checkGlobalFunction(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);

private:
  SemanticState &state;
  Prelude *prelude = nullptr;
  CallChecker *callChecker = nullptr;
  DateChecker *dateChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  MathChecker *mathChecker = nullptr;
  RegexChecker *regexChecker = nullptr;
  TruthChecker *truthChecker = nullptr;
  BinaryChecker *binaryChecker = nullptr;
  ConcatChecker *concatChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  DecimalChecker *decimalChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
