#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

class RegexLitExpr : public Expr {
public:
  RegexLitExpr(llvm::SMLoc loc, std::u16string pattern, std::u16string flags)
      : Expr(Kind::RegexLit, loc), pattern(std::move(pattern)), flags(std::move(flags)) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::RegexLit; }

  std::u16string pattern;
  std::u16string flags;
  size_t written = 0;
  std::vector<uint32_t> program;
};

}
