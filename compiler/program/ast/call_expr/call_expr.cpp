#include "compiler/program/ast/call_expr/call_expr.h"

namespace tz {

FuncSymbol *CallExpr::func() const {
  const FunctionCall *direct = this->target.as<FunctionCall>();

  return direct ? direct->func : nullptr;
}

Builtin CallExpr::builtin() const {
  const BuiltinCall *native = this->target.as<BuiltinCall>();

  return native ? native->builtin : Builtin::None;
}

FuncSymbol *CallExpr::method() const {
  FuncSymbol *called = nullptr;
  switch (this->target.kind()) {
    case CallTargetKind::UNRESOLVED_CALL:
    case CallTargetKind::FUNCTION_CALL:
    case CallTargetKind::OVERLOAD_CALL:
    case CallTargetKind::UNION_METHOD_CALL:
      break;
    case CallTargetKind::METHOD_CALL:
      called = this->target.as<MethodCall>()->method;
      break;
    case CallTargetKind::SUPER_CALL:
      called = this->target.as<SuperCall>()->method;
      break;
    case CallTargetKind::BUILTIN_CALL:
      called = this->target.as<BuiltinCall>()->method;
      break;
  }

  return called;
}

bool CallExpr::virtualCall() const {
  const MethodCall *dispatched = this->target.as<MethodCall>();

  return dispatched && dispatched->virtualCall;
}

int CallExpr::signature() const {
  const OverloadCall *overload = this->target.as<OverloadCall>();

  return overload ? overload->signature : -1;
}

bool CallExpr::superCall() const {
  return this->target.is<SuperCall>();
}

llvm::ArrayRef<UnionMethod> CallExpr::unionMethods() const {
  const UnionMethodCall *joined = this->target.as<UnionMethodCall>();

  return joined ? llvm::ArrayRef<UnionMethod>(joined->unionMethods) : llvm::ArrayRef<UnionMethod>();
}

}
