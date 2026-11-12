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
  static std::optional<Term> sum(Term left, Term right);
  bool isZero() const { return var == nullptr; }
  bool isSum() const { return var2 != nullptr; }
  Term first() const { return {var, length}; }
  Term second() const { return {var2, length2}; }
  bool mentions(const VarSymbol *variable) const {
    return var == variable || var2 == variable;
  }
  bool contains(Term part) const {
    return *this == part || (isSum() && (first() == part || second() == part));
  }
  bool operator==(const Term &other) const {
    return var == other.var && length == other.length && var2 == other.var2 &&
           length2 == other.length2;
  }
  bool operator!=(const Term &other) const { return !(*this == other); }
  std::pair<uintptr_t, bool> sortKey() const;
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
  void addEqual(Term left, Term right, int64_t offset);
  bool canProve(Term lhs, Term rhs, int64_t offset) const;
  std::optional<int64_t> bound(Term lhs, Term rhs) const;

  void forgetIf(llvm::function_ref<bool(const Fact &)> isForgotten);
  void forget(const VarSymbol *variable);
  void shift(Term term, int64_t delta);

  Facts meet(const Facts &other) const;
  static int64_t saturatingAdd(int64_t left, int64_t right);
  static int64_t saturatingSub(int64_t left, int64_t right);

  const std::vector<Fact> &all() const {
    return this->facts;
  }

private:
  struct Edge {
    size_t from, to;
    int64_t weight;
  };

  struct PartLimit {
    Term term;
    std::optional<int64_t> above;
    std::optional<int64_t> below;
  };

  struct SumParts {
    Term part;
    Term other;
  };

  const Type *integerType(Term term) const;
  std::optional<int64_t> lower(Term term) const;
  std::optional<int64_t> upper(Term term) const;
  std::optional<int64_t> plus(std::optional<int64_t> left, std::optional<int64_t> right) const;
  std::optional<int64_t> plainBound(Term lhs, Term rhs) const;
  std::vector<Term> boundNodes(Term lhs, Term rhs) const;
  void addNode(std::vector<Term> &nodes, Term term) const;
  size_t nodeIndex(const std::vector<Term> &nodes, Term term) const;
  std::vector<PartLimit> partLimits(const std::vector<Term> &nodes) const;
  void addPartLimit(std::vector<PartLimit> &limits, Term part) const;
  std::vector<Edge> factEdges(const std::vector<Term> &nodes) const;
  std::optional<int64_t> shortestBound(const std::vector<Term> &nodes, Term lhs, Term rhs,
                                       const std::vector<PartLimit> *limits) const;
  void addSumEdges(const std::vector<Term> &nodes, size_t sumIndex,
                   const std::vector<PartLimit> *limits, std::vector<Edge> &edges) const;
  void addPartEdges(const std::vector<Term> &nodes, size_t sumIndex, const SumParts &parts,
                    const std::vector<PartLimit> *limits, std::vector<Edge> &edges) const;
  std::optional<int64_t> limitedUpper(Term other, const std::vector<PartLimit> *limits) const;
  std::optional<int64_t> limitedLower(Term other, const std::vector<PartLimit> *limits) const;
  bool isLimitOf(Term term, const PartLimit &limit) const;
  const PartLimit &limitOf(const std::vector<PartLimit> &limits, Term term) const;
  std::optional<int64_t> nodeUpper(Term node) const;
  std::optional<int64_t> nodeLower(Term node) const;
  void addZeroEdges(const std::vector<Term> &nodes, std::vector<Edge> &edges) const;
  std::optional<int64_t> shortestDistance(size_t count, const std::vector<Edge> &edges,
                                          size_t source, size_t target) const;
  bool relaxEdges(const std::vector<Edge> &edges, std::vector<int64_t> &distances) const;
  bool relaxEdge(const Edge &edge, std::vector<int64_t> &distances) const;
  bool isMentioning(const VarSymbol *variable, const Fact &fact) const;
  bool isVanishing(const Fact &fact) const;

  std::vector<Fact> facts;
};

}
