#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/values/string_methods/string_methods_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class StringMethods {
public:
  explicit StringMethods(Checker &owner);

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

  Checker &checker;
};

}
