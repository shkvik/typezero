#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include <cstdint>
#include <optional>
#include <vector>

namespace tz {

constexpr int64_t kMaxLength = (int64_t(1) << 30) - 1;

struct Term {
  const VarSymbol *var = nullptr;
  bool length = false;

  static Term zero() { return {}; }
  static Term of(const VarSymbol *var) { return {var, false}; }
  static Term lengthOf(const VarSymbol *var) { return {var, true}; }
  bool isZero() const { return var == nullptr; }
  bool operator==(const Term &other) const { return var == other.var && length == other.length; }
  bool operator!=(const Term &other) const { return !(*this == other); }
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

  const std::vector<Fact> &all() const { return facts_; }

private:
  std::vector<Fact> facts_;
};

}
