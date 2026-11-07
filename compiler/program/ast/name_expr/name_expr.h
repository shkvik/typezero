#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/type_ref/type_ref.h"
#include "compiler/program/symbols/func_symbol/func_symbol.h"
#include "compiler/program/symbols/var_symbol/var_symbol.h"
#include "compiler/union/union/union.h"

namespace tz {

struct ClassDecl;

enum class NameBindingKind {
  UNBOUND_NAME,
  NAMED_VAR,
  NAMED_FUNCTION,
  NAMED_CLASS,
};

struct UnboundName {
  static constexpr NameBindingKind kind = NameBindingKind::UNBOUND_NAME;
};

struct NamedVar {
  static constexpr NameBindingKind kind = NameBindingKind::NAMED_VAR;
  VarSymbol *var = nullptr;
};

struct NamedFunction {
  static constexpr NameBindingKind kind = NameBindingKind::NAMED_FUNCTION;
  FuncSymbol *func = nullptr;
};

struct NamedClass {
  static constexpr NameBindingKind kind = NameBindingKind::NAMED_CLASS;
  ClassDecl *cls = nullptr;
};

using NameBinding = Union<UnboundName, NamedVar, NamedFunction, NamedClass>;

class NameExpr : public Expr {
public:
  NameExpr(llvm::SMLoc loc, llvm::StringRef name) : Expr(Kind::Name, loc), name(name) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Name; }

  llvm::StringRef name;
  std::vector<TypeRef> typeArgs;
  NameBinding binding = UnboundName{};

  VarSymbol *var() const;
  FuncSymbol *func() const;
  ClassDecl *cls() const;
};

}
