#pragma once

#include "compiler/program/ast/call_expr/call_expr.h"
#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/type_ref/type_ref.h"
#include "compiler/union/union/union.h"

namespace tz {

struct ClassDecl;

enum class NewKind {
  BUILTIN_NEW,
  CLASS_NEW,
  REFLECT_NEW,
};

struct BuiltinNew {
  static constexpr NewKind kind = NewKind::BUILTIN_NEW;
};

struct ClassNew {
  static constexpr NewKind kind = NewKind::CLASS_NEW;
  ClassDecl *cls = nullptr;
  CallExpr *ctor = nullptr;
};

struct ReflectNew {
  static constexpr NewKind kind = NewKind::REFLECT_NEW;
};

using NewForm = Union<BuiltinNew, ClassNew, ReflectNew>;

class NewExpr : public Expr {
public:
  NewExpr(llvm::SMLoc loc, llvm::StringRef name, std::vector<TypeRef> typeArgs,
          std::vector<Expr *> args)
      : Expr(Kind::New, loc), name(name), typeArgs(std::move(typeArgs)), args(std::move(args)) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::New; }

  llvm::StringRef name;
  llvm::StringRef member;
  std::vector<TypeRef> typeArgs;
  std::vector<Expr *> args;
  Expr *callee = nullptr;
  NewForm form = BuiltinNew{};

  ClassDecl *cls() const;
  CallExpr *ctor() const;
  bool reflect() const;
};

}
