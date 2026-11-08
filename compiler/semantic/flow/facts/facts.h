#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include <cstdint>
#include <optional>
#include <vector>

namespace tz {

struct Term {
  const VarSymbol *var = nullptr;
  bool length = false;
  const VarSymbol *var2 = nullptr;
  bool length2 = false;

  static Term zero() { return {}; }
  static Term of(const VarSymbol *var) { return {var, false}; }
  static Term lengthOf(const VarSymbol *var) { return {var, true}; }
  static std::optional<Term> sum(Term a, Term b);
  bool isZero() const { return var == nullptr; }
  bool isSum() const { return var2 != nullptr; }
  Term first() const { return {var, length}; }
  Term second() const { return {var2, length2}; }
  bool mentions(const VarSymbol *v) const { return var == v || var2 == v; }
  bool contains(Term part) const {
    return *this == part || (isSum() && (first() == part || second() == part));
  }
  bool operator==(const Term &other) const {
    return var == other.var && length == other.length && var2 == other.var2 &&
           length2 == other.length2;
  }
  bool operator!=(const Term &other) const { return !(*this == other); }
};

struct IntegerRange {
  std::optional<int64_t> lo, hi;
};

struct Linear {
  Term term;
  int64_t offset = 0;
};

class Facts {
public:
  struct Fact {
    Term lhs, rhs;
    int64_t offset;
  };

  void add(Term lhs, Term rhs, int64_t offset);
  void addEqual(Term a, Term b, int64_t offset);
  bool proves(Term lhs, Term rhs, int64_t offset) const;
  std::optional<int64_t> bound(Term lhs, Term rhs) const;

  void forgetIf(llvm::function_ref<bool(const Fact &)> pred);
  void forget(const VarSymbol *var);
  void shift(Term term, int64_t delta);

  static Facts meet(const Facts &a, const Facts &b);
  static int64_t saturatingAdd(int64_t a, int64_t b);
  static int64_t saturatingSub(int64_t a, int64_t b);

  const std::vector<Fact> &all() const {
    return this->facts;
  }

private:
  static std::optional<int64_t> lower(Term t);
  static std::optional<int64_t> plus(std::optional<int64_t> a, std::optional<int64_t> b);
  static std::optional<int64_t> upper(Term t);
  std::vector<Fact> facts;

  std::optional<int64_t> bound(Term lhs, Term rhs, bool sums) const;
};

}
