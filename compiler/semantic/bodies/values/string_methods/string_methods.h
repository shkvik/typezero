#pragma once

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
  struct Method {
    llvm::StringRef name;
    Builtin builtin;
    unsigned minArgs;
    unsigned maxArgs;
    const char *args;
    const Type *ret;
  };

  using ResultStringMethod = Result<const Method *, StringMethodsStringMethodError>;

  static ResultStringMethod stringMethod(llvm::StringRef name);

  SemanticState &state;
  ExprChecker *exprChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  ConcatChecker *concatChecker = nullptr;
  MemberChecker *memberChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  StringRegexChecker *stringRegexChecker = nullptr;
};

}
