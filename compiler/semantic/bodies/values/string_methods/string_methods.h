#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/values/string_methods/string_methods_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class StringMethods {
public:
  explicit StringMethods(SemanticState &owner);

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
};

}
