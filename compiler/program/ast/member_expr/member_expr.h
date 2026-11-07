#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/type_ref/type_ref.h"
#include "compiler/program/symbols/func_symbol/func_symbol.h"
#include "compiler/union/union/union.h"

namespace tz {

enum class MemberKind {
  PROPERTY_MEMBER,
  METHOD_MEMBER,
  OVERLOAD_MEMBER,
};

struct PropertyMember {
  static constexpr MemberKind kind = MemberKind::PROPERTY_MEMBER;
};

struct MethodMember {
  static constexpr MemberKind kind = MemberKind::METHOD_MEMBER;
  FuncSymbol *method = nullptr;
  bool virtualCall = false;
};

struct OverloadMember {
  static constexpr MemberKind kind = MemberKind::OVERLOAD_MEMBER;
  int signature = 0;
};

using MemberForm = Union<PropertyMember, MethodMember, OverloadMember>;

class MemberExpr : public Expr {
public:
  MemberExpr(llvm::SMLoc loc, Expr *object, llvm::StringRef name)
      : Expr(Kind::Member, loc), object(object), name(name) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Member; }

  Expr *object;
  llvm::StringRef name;
  bool optional = false;
  std::vector<TypeRef> typeArgs;
  MemberForm form = PropertyMember{};

  FuncSymbol *method() const;
  bool virtualCall() const;
  int signature() const;
};

}
