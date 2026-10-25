#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

// `/pattern/flags`. Checker compiles it (docs/regex.md): the program, a
// constant in the executable, says what its groups are.
class RegexLitExpr : public Expr {
public:
  RegexLitExpr(llvm::SMLoc loc, std::u16string pattern, std::u16string flags)
      : Expr(Kind::RegexLit, loc), pattern(std::move(pattern)), flags(std::move(flags)) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::RegexLit; }

  // As written: the escapes are the regex's own.
  std::u16string pattern;
  std::u16string flags;
  // The length of the literal in the source, from loc(); 0 for one that
  // `new RegExp("...")` stands for.
  size_t written = 0;
  // Set by Checker (see runtime/library/regex/regex/regex.h); empty after an
  // error.
  std::vector<uint32_t> program;
};

} // namespace tz
