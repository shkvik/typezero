#include "compiler/check/facts/facts.h"
#include "compiler/program/types/types.h"
#include <algorithm>
#include <limits>

namespace tz {

namespace {

constexpr size_t kMaxFacts = 64;

constexpr int64_t kInf = std::numeric_limits<int64_t>::max();

std::optional<int64_t> upper(Term t) {
  if (t.length) {
    return kMaxLength;
  }
  const Type *type = t.var && t.var->type ? t.var->type->nonOptional() : nullptr;
  if (type && type->isInteger() && type->intMax() <= uint64_t(kInf)) {
    return int64_t(type->intMax());
  }

  return std::nullopt;
}

std::optional<int64_t> lower(Term t) {
  if (t.length) {
    return 0;
  }
  const Type *type = t.var && t.var->type ? t.var->type->nonOptional() : nullptr;
  if (type && type->isInteger() && type->intMin() > std::numeric_limits<int64_t>::min()) {
    return type->intMin();
  }

  return std::nullopt;
}

std::optional<int64_t> plus(std::optional<int64_t> a, std::optional<int64_t> b) {
  if (!a || !b || (*b > 0 && *a > kInf - 1 - *b) || (*b < 0 && *a < -kInf + 1 - *b)) {
    return std::nullopt;
  }

  return *a + *b;
}

}

std::optional<Term> Term::sum(Term a, Term b) {
  if (a.isZero() || b.isZero() || a.isSum() || b.isSum()) {
    return std::nullopt;
  }
  auto key = [](Term t) { return std::pair(reinterpret_cast<uintptr_t>(t.var), t.length); };
  if (key(b) < key(a)) {
    std::swap(a, b);
  }

  return Term{a.var, a.length, b.var, b.length};
}

void Facts::add(Term lhs, Term rhs, int64_t offset) {
  if (lhs == rhs || offset <= std::numeric_limits<int64_t>::min() + 1) {
    return;
  }
  for (Fact &fact : facts_) {
    if (fact.lhs == lhs && fact.rhs == rhs) {
      fact.offset = std::min(fact.offset, offset);
      return;
    }
  }
  if (facts_.size() == kMaxFacts) {
    facts_.erase(facts_.begin());
  }
  facts_.push_back({lhs, rhs, offset});
}

void Facts::addEqual(Term a, Term b, int64_t offset) {
  add(a, b, saturatingAdd(offset, 1));
  add(b, a, saturatingSub(1, offset));
}

bool Facts::proves(Term lhs, Term rhs, int64_t offset) const {
  if (lhs == rhs) {
    return offset >= 1;
  }
  std::optional<int64_t> least = bound(lhs, rhs);

  return least && *least <= offset;
}

std::optional<int64_t> Facts::bound(Term lhs, Term rhs) const { return bound(lhs, rhs, true); }

std::optional<int64_t> Facts::bound(Term lhs, Term rhs, bool sums) const {
  if (lhs == rhs) {
    return 1;
  }

  std::vector<Term> nodes{Term::zero()};
  auto node = [&](Term t) {
    for (size_t i = 0; i < nodes.size(); ++i) {
      if (nodes[i] == t) {
        return i;
      }
    }
    nodes.push_back(t);

    return nodes.size() - 1;
  };
  struct Edge {
    size_t from, to;
    int64_t weight;
  };
  std::vector<Edge> edges;
  for (const Fact &fact : facts_) {
    edges.push_back({node(fact.lhs), node(fact.rhs), fact.offset - 1});
  }
  size_t source = node(lhs), target = node(rhs);
  for (size_t i = 1; i < nodes.size(); ++i) {
    if (!nodes[i].isSum()) {
      continue;
    }
    Term parts[2] = {nodes[i].first(), nodes[i].second()};
    for (int k = 0; k < 2; ++k) {
      Term other = parts[1 - k];
      size_t self = node(parts[k]);
      std::optional<int64_t> hi = upper(other), lo = lower(other);
      if (sums) {
        if (std::optional<int64_t> above = bound(other, Term::zero(), false)) {
          hi = hi ? std::min(*hi, *above - 1) : *above - 1;
        }
        if (std::optional<int64_t> below = bound(Term::zero(), other, false)) {
          lo = lo ? std::max(*lo, 1 - *below) : 1 - *below;
        }
      }
      if (hi) {
        edges.push_back({i, self, *hi});
      }
      if (lo && *lo > -kInf) {
        edges.push_back({self, i, -*lo});
      }
    }
  }
  for (size_t i = 1; i < nodes.size(); ++i) {
    std::optional<int64_t> hi, lo;
    if (nodes[i].isSum()) {
      hi = plus(upper(nodes[i].first()), upper(nodes[i].second()));
      lo = plus(lower(nodes[i].first()), lower(nodes[i].second()));
    } else {
      hi = upper(nodes[i]);
      lo = lower(nodes[i]);
    }
    if (hi) {
      edges.push_back({i, 0, *hi});
    }
    if (lo && *lo > -kInf) {
      edges.push_back({0, i, -*lo});
    }
  }

  std::vector<int64_t> dist(nodes.size(), kInf);
  dist[source] = 0;
  for (size_t round = 0; round + 1 < nodes.size(); ++round) {
    bool changed = false;
    for (const Edge &e : edges) {
      if (dist[e.from] == kInf) {
        continue;
      }
      std::optional<int64_t> sum = plus(dist[e.from], e.weight);
      if (!sum) {
        continue;
      }
      int64_t d = *sum;
      if (d < dist[e.to]) {
        dist[e.to] = d;
        changed = true;
      }
    }
    if (!changed) {
      break;
    }
  }
  if (dist[target] == kInf) {
    return std::nullopt;
  }

  return dist[target] + 1;
}

void Facts::forgetIf(llvm::function_ref<bool(const Fact &)> pred) {
  facts_.erase(std::remove_if(facts_.begin(), facts_.end(), pred), facts_.end());
}

void Facts::forget(const VarSymbol *var) {
  forgetIf([&](const Fact &f) { return f.lhs.mentions(var) || f.rhs.mentions(var); });
}

void Facts::shift(Term term, int64_t delta) {
  for (Fact &fact : facts_) {
    bool lhs = fact.lhs.contains(term), rhs = fact.rhs.contains(term);
    if (lhs && !rhs) {
      fact.offset = saturatingAdd(fact.offset, delta);
    } else if (rhs && !lhs) {
      fact.offset = saturatingSub(fact.offset, delta);
    }
  }
  forgetIf([](const Fact &f) { return f.offset <= std::numeric_limits<int64_t>::min() + 1; });
}

Facts Facts::meet(const Facts &a, const Facts &b) {
  Facts result;
  for (const Fact &fact : a.facts_) {
    if (b.proves(fact.lhs, fact.rhs, fact.offset)) {
      result.add(fact.lhs, fact.rhs, fact.offset);
    }
  }
  for (const Fact &fact : b.facts_) {
    if (a.proves(fact.lhs, fact.rhs, fact.offset)) {
      result.add(fact.lhs, fact.rhs, fact.offset);
    }
  }

  return result;
}

}
