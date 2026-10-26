#include "compiler/check/facts/facts.h"
#include <algorithm>
#include <limits>

namespace tz {

namespace {

constexpr int64_t kMaxOffset = int64_t(1) << 40;
constexpr size_t kMaxFacts = 64;

constexpr int64_t kIntMin = std::numeric_limits<int32_t>::min();
constexpr int64_t kIntMax = std::numeric_limits<int32_t>::max();

}

void Facts::add(Term lhs, Term rhs, int64_t offset) {
  if (lhs == rhs || offset > kMaxOffset || offset < -kMaxOffset) {
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
  add(a, b, offset + 1);
  add(b, a, 1 - offset);
}

bool Facts::proves(Term lhs, Term rhs, int64_t offset) const {
  if (lhs == rhs) {
    return offset >= 1;
  }
  std::optional<int64_t> least = bound(lhs, rhs);

  return least && *least <= offset;
}

std::optional<int64_t> Facts::bound(Term lhs, Term rhs) const {
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
    if (nodes[i].length) {
      edges.push_back({i, 0, kMaxLength});
      edges.push_back({0, i, 0});
    } else {
      edges.push_back({i, 0, kIntMax});
      edges.push_back({0, i, -kIntMin});
    }
  }

  constexpr int64_t kInf = std::numeric_limits<int64_t>::max();
  std::vector<int64_t> dist(nodes.size(), kInf);
  dist[source] = 0;
  for (size_t round = 0; round + 1 < nodes.size(); ++round) {
    bool changed = false;
    for (const Edge &e : edges) {
      if (dist[e.from] == kInf) {
        continue;
      }
      int64_t d = dist[e.from] + e.weight;
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
  forgetIf([&](const Fact &f) { return f.lhs.var == var || f.rhs.var == var; });
}

void Facts::shift(Term term, int64_t delta) {
  for (Fact &fact : facts_) {
    if (fact.lhs == term && fact.rhs != term) {
      fact.offset += delta;
    } else if (fact.rhs == term && fact.lhs != term) {
      fact.offset -= delta;
    }
  }
  forgetIf([](const Fact &f) { return f.offset > kMaxOffset || f.offset < -kMaxOffset; });
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
