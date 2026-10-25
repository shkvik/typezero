#pragma once

#include "compiler/program/ast/ast.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace tz {

// The longest string or array, in elements. Keeping it well below the int
// range lets `length + 1` and similar bounds never overflow.
constexpr int64_t kMaxLength = (int64_t(1) << 30) - 1;

// One side of a fact: an int variable, the length of a string or array
// variable, or the constant 0.
struct Term {
  const VarSymbol *var = nullptr; // null: the constant 0
  bool length = false;

  static Term zero() { return {}; }
  static Term of(const VarSymbol *var) { return {var, false}; }
  static Term lengthOf(const VarSymbol *var) { return {var, true}; }
  bool isZero() const { return var == nullptr; }
  bool operator==(const Term &other) const { return var == other.var && length == other.length; }
  bool operator!=(const Term &other) const { return !(*this == other); }
};

// `term + offset`: an index, a bound or a constant (when term is zero).
struct Linear {
  Term term;
  int64_t offset = 0;
};

// Facts `lhs < rhs + offset` known to hold at some point of the program, such
// as `i < s.length` or `0 < i + 1` (that is, i >= 0). Sema collects them from
// conditions, declarations and assignments, and uses them to prove that an
// index is in range.
class Facts {
public:
  struct Fact {
    Term lhs, rhs;
    int64_t offset;
  };

  // Adds `lhs < rhs + offset`.
  void add(Term lhs, Term rhs, int64_t offset);
  // Adds `a == b + offset`.
  void addEqual(Term a, Term b, int64_t offset);
  // Whether `lhs < rhs + offset` follows from the facts and from the ranges of
  // the terms: ints lie in the int range, lengths in [0, kMaxLength].
  bool proves(Term lhs, Term rhs, int64_t offset) const;
  // The least offset for which they prove `lhs < rhs + offset`, if any.
  std::optional<int64_t> bound(Term lhs, Term rhs) const;

  // Forgets the facts for which `pred` returns true.
  void forgetIf(llvm::function_ref<bool(const Fact &)> pred);
  // Forgets everything about `var` and its length.
  void forget(const VarSymbol *var);
  // Updates the facts after `term` grew by `delta` without overflowing.
  void shift(Term term, int64_t delta);

  // The facts that hold both here and in `other`.
  static Facts meet(const Facts &a, const Facts &b);

  const std::vector<Fact> &all() const { return facts_; }

private:
  std::vector<Fact> facts_;
};

} // namespace tz
