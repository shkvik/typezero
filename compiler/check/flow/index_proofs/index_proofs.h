#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The proofs that an index is in bounds (docs/arrays.md).
class IndexProofs {
public:
  explicit IndexProofs(Sema &sema);

  // Flow: facts, narrowing and proofs (sema_flow.cpp)
  // `expr` (checked, of an integer type) as `term + offset`, if it has that
  // form and its computation cannot overflow.
  std::optional<Linear> linear(const Expr *expr) const;
  // The length of a string or array expression as a linear term.
  std::optional<Linear> lengthOf(const Expr *object) const;
  // The length a string or array value is known to have when created.
  std::optional<Linear> knownLength(const Expr *value) const;
  // Whether 0 <= index < length(object), or <= length with `allowEnd`.
  bool provesInBounds(const Expr *object, const Expr *index, bool allowEnd) const;
  // The same for `at(index)`, where a negative index counts from the end.
  bool provesAtInBounds(const Expr *object, const Expr *index) const;
  bool provesNonNegative(const Expr *expr) const;
  bool provesNotEmpty(const Expr *object) const;

private:
  Sema &sema_;
};

} // namespace tz
