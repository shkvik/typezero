#pragma once

#include "compiler/overflow/overflow_check/overflow_check.h"

namespace tz {

// Where to warn and what to suggest.
class OverflowWarnings {
public:
  explicit OverflowWarnings(OverflowCheck &check);

  Val trusted(const Type *type) const;
  Val fitVal(Val v, const Type *type) const;
  Num fit(Num n, const Type *type) const;
  const Reason *reason(Origin origin, llvm::SMLoc loc, std::string what, bool unbounded = false);
  Num input(const Type *type, llvm::SMLoc loc, const char *what);
  void warn(const Expr *site, const Num &l, const Num &r, const Expr *lhs, const Expr *rhs,
            const Num &exceeding, const VarSymbol *accum);
  std::string line(llvm::SMLoc loc, llvm::SMLoc from) const;
  std::string inputText(const Reason *why, llvm::SMLoc from) const;
  std::string fixText(const Expr *site, const VarSymbol *accum) const;

private:
  OverflowCheck &check_;
};

} // namespace tz
