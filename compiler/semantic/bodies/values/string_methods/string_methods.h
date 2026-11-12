#pragma once

#include "compiler/program/library/string/string_method_table/string_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/values/string_methods/string_methods_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ExprChecker;
class IndexProofs;
class ConcatChecker;
class MemberChecker;
class ArgumentChecker;
class StringRegexChecker;

class StringMethods {
public:
  StringMethods(SemanticState &owner, SemanticParts &all);

  Expr *checkStringMethod(CallExpr *call, MemberExpr *member);

private:
  using ResultStringMethod = Result<const StringMethodInfo *, StringMethodsStringMethodError>;

  const Type *resultType(StringResult result) const;
  ResultStringMethod checkMethodName(llvm::StringRef name) const;
  void walkStringArgs(CallExpr *call, llvm::StringRef argKinds);
  Expr *checkStringArg(Expr *arg, char kind);
  void walkMethodRange(CallExpr *call, const MemberExpr *member, StringMethod method);
  void walkRepeatCount(const CallExpr *call);

  SemanticState &state;
  ExprChecker *exprChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  ConcatChecker *concatChecker = nullptr;
  MemberChecker *memberChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  StringRegexChecker *stringRegexChecker = nullptr;
};

}
