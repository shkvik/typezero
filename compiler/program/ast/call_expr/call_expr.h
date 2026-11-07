#pragma once

#include "compiler/program/ast/builtin/builtin.h"
#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/regex_lit_expr/regex_lit_expr.h"
#include "compiler/program/symbols/func_symbol/func_symbol.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/ArrayRef.h"

namespace tz {

class Type;

struct UnionMethod {
  const Type *member;
  FuncSymbol *method;
  bool virtualCall;
};

enum class CallTargetKind {
  UNRESOLVED_CALL,
  FUNCTION_CALL,
  METHOD_CALL,
  SUPER_CALL,
  OVERLOAD_CALL,
  BUILTIN_CALL,
  UNION_METHOD_CALL,
};

struct UnresolvedCall {
  static constexpr CallTargetKind kind = CallTargetKind::UNRESOLVED_CALL;
};

struct FunctionCall {
  static constexpr CallTargetKind kind = CallTargetKind::FUNCTION_CALL;
  FuncSymbol *func = nullptr;
};

struct MethodCall {
  static constexpr CallTargetKind kind = CallTargetKind::METHOD_CALL;
  FuncSymbol *method = nullptr;
  bool virtualCall = false;
};

struct SuperCall {
  static constexpr CallTargetKind kind = CallTargetKind::SUPER_CALL;
  FuncSymbol *method = nullptr;
};

struct OverloadCall {
  static constexpr CallTargetKind kind = CallTargetKind::OVERLOAD_CALL;
  int signature = 0;
};

struct BuiltinCall {
  static constexpr CallTargetKind kind = CallTargetKind::BUILTIN_CALL;
  Builtin builtin = Builtin::None;
  FuncSymbol *method = nullptr;
};

struct UnionMethodCall {
  static constexpr CallTargetKind kind = CallTargetKind::UNION_METHOD_CALL;
  std::vector<UnionMethod> unionMethods;
};

using CallTarget = Union<UnresolvedCall, FunctionCall, MethodCall, SuperCall, OverloadCall,
                         BuiltinCall, UnionMethodCall>;

class CallExpr : public Expr {
public:
  CallExpr(llvm::SMLoc loc, Expr *callee, std::vector<Expr *> args)
      : Expr(Kind::Call, loc), callee(callee), args(std::move(args)) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Call; }

  Expr *callee;
  std::vector<Expr *> args;
  std::vector<bool> spread;
  bool spreads(size_t i) const { return i < spread.size() && spread[i]; }
  bool hasSpread() const { return llvm::is_contained(spread, true); }
  bool optional = false;
  bool tagged = false;
  CallTarget target = UnresolvedCall{};
  bool mayBeOutOfRange = false;
  bool fused = false;
  const RegexLitExpr *regex = nullptr;
  bool forIn = false;
  const void *metaKey = nullptr;
  bool designRecord = false;

  FuncSymbol *func() const;
  Builtin builtin() const;
  FuncSymbol *method() const;
  bool virtualCall() const;
  int signature() const;
  bool superCall() const;
  llvm::ArrayRef<UnionMethod> unionMethods() const;
};

}
