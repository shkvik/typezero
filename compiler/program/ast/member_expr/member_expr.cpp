#include "compiler/program/ast/member_expr/member_expr.h"

namespace tz {

FuncSymbol *MemberExpr::method() const {
  const MethodMember *methodValue = this->form.as<MethodMember>();

  return methodValue ? methodValue->method : nullptr;
}

bool MemberExpr::virtualCall() const {
  const MethodMember *methodValue = this->form.as<MethodMember>();

  return methodValue && methodValue->virtualCall;
}

int MemberExpr::signature() const {
  const OverloadMember *overload = this->form.as<OverloadMember>();

  return overload ? overload->signature : -1;
}

}
