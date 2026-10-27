#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class IndexProofs {
public:
  explicit IndexProofs(Checker &checker);

  std::optional<Linear> linear(const Expr *expr) const;
  std::optional<Linear> lengthOf(const Expr *object) const;
  std::optional<Linear> knownLength(const Expr *value) const;
  bool provesInBounds(const Expr *object, const Expr *index, bool allowEnd) const;
  bool provesAtInBounds(const Expr *object, const Expr *index) const;
  bool provesNonNegative(const Expr *expr) const;
  bool provesNotEmpty(const Expr *object) const;
  bool provesBetween(const Expr *expr, int64_t lo, std::optional<int64_t> hi) const;
  bool exempt(llvm::SMLoc loc, const Expr *object = nullptr) const;
  bool requireBetween(const Expr *expr, int64_t lo, std::optional<int64_t> hi,
                      llvm::StringRef what);
  bool provesLess(const Expr *a, const Expr *b) const;
  bool requireLess(const Expr *a, const Expr *b, llvm::StringRef what);
  bool provesLinearUpTo(Linear value, const Expr *object, int64_t less) const;
  bool provesUpTo(const Expr *value, const Expr *object, int64_t less) const;
  bool requireUpTo(const Expr *value, const Expr *object, int64_t less, llvm::StringRef what);
  std::optional<int64_t> exactLength(const Expr *object) const;
  std::optional<Linear> viewLength(const CallExpr *call) const;
  std::optional<Linear> utf8Length(const CallExpr *call) const;
  void addIndexCallbackFacts(const FunctionDecl *fn);
  std::optional<Linear> freshBufferLength(const CallExpr *call) const;
  ValueRange lengthRange(const Expr *value) const;
  std::optional<Linear> lengthAtMost(const Expr *expr) const;
  Linear settled(Linear value) const;
  bool provesSumUpTo(std::optional<Linear> a, std::optional<Linear> b, const Expr *object) const;

private:
  Checker &checker_;
};

}
