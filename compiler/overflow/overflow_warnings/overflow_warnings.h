#pragma once

#include "compiler/overflow/overflow_check/overflow_check.h"

namespace tz {

struct DatePart {
  const Expr *expr = nullptr;
  Num num;
  double lo = 0, hi = 0;
};

class OverflowWarnings {
public:
  explicit OverflowWarnings(OverflowCheck &check);

  Val trusted(const Type *type) const;
  Val fitVal(Val v, const Type *type) const;
  Num fit(Num n, const Type *type) const;
  const Reason *reason(Origin origin, llvm::SMLoc loc, std::string what, bool unbounded = false);
  Num input(const Type *type, llvm::SMLoc loc, const char *what);
  void warn(const Expr *site, const Num &l, const Num &r, const Expr *lhs, const Expr *rhs,
            const Num &exceeding, const VarSymbol *accum, const Type *type);
  std::string line(llvm::SMLoc loc, llvm::SMLoc from) const;
  std::string inputText(const Reason *why, llvm::SMLoc from) const;
  std::string fixText(const Expr *site, const VarSymbol *accum) const;
  bool checkConversion(const CallExpr *call, const Expr *value, const Num &x, const Type *to);
  bool checkDivisor(const Expr *site, BinaryOp op, const Expr *divisor, const Num &l, const Num &r);
  void noteProof(const Expr *site, bool proven);
  void checkShiftCount(const Expr *count, const Num &n, unsigned width);
  bool checkPowerBase(const Expr *site, const Expr *base, const Num &a);
  bool checkDomain(const CallExpr *call, const Expr *value, const Num &x, double lo, double hi,
                   bool loOpen, bool hiOpen, llvm::StringRef what, llvm::StringRef condition,
                   llvm::StringRef extra = "");
  bool checkClamp(const CallExpr *call, const Num &lo, const Num &hi);
  void overflowCall(const CallExpr *call, const Num &x, const Num &exceeding);
  void checkWithin(const Expr *value, const Num &x, double lo, double hi, llvm::StringRef what,
                   llvm::StringRef limits);
  void checkDate(const Num &time, double limit, llvm::ArrayRef<DatePart> parts);

private:
  OverflowCheck &check_;
};

}
