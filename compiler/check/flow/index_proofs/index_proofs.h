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

private:
  Checker &checker_;
};

}
