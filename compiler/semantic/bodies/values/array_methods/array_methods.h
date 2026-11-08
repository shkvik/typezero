#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/values/array_methods/array_methods_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ArrayMethods {
public:
  using ResultArrayMethodCall = Result<Expr *, ArrayMethodsCheckArrayMethodError>;

  explicit ArrayMethods(SemanticState &owner);

  ResultArrayMethodCall checkArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  using ResultSortableElement = Result<const Type *, ArrayMethodsSortableError>;
  using ResultElementsAreText = Result<const Type *, ArrayMethodsTextElementsError>;

  static const VarSymbol *arrayVariable(const Expr *object);
  static ResultSortableElement sortable(const Type *element);
  static ResultElementsAreText textElements(const Type *element, llvm::StringRef name);

  SemanticState &state;
};

}
