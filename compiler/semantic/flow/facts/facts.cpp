#include "compiler/semantic/flow/facts/facts.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/STLExtras.h"
#include <algorithm>
#include <limits>

namespace tz {

namespace {

constexpr size_t kMaxFacts = 64;

constexpr int64_t kInf = std::numeric_limits<int64_t>::max();

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

int64_t Facts::saturatingAdd(int64_t a, int64_t b) {
  if (b > 0 && a > INT64_MAX - b) {
    return INT64_MAX;
  }
  if (b < 0 && a < INT64_MIN - b) {
    return INT64_MIN;
  }
  return a + b;
}

int64_t Facts::saturatingSub(int64_t a, int64_t b) {
  if (b < 0 && a > INT64_MAX + b) {
    return INT64_MAX;
  }
  if (b > 0 && a < INT64_MIN + b) {
    return INT64_MIN;
  }
  return a - b;
}

std::optional<int64_t> Facts::lower(Term t) {
  if (t.length) {
    return 0;
  }
  const Type *type = t.var && t.var->type ? t.var->type->nonOptional() : nullptr;
  if (type && type->isInteger() && type->intMin() > std::numeric_limits<int64_t>::min()) {
    return type->intMin();
  }

  return std::nullopt;
}

std::optional<int64_t> Facts::plus(std::optional<int64_t> a, std::optional<int64_t> b) {
  if (!a || !b || (*b > 0 && *a > kInf - 1 - *b) || (*b < 0 && *a < -kInf + 1 - *b)) {
    return std::nullopt;
  }

  return *a + *b;
}

std::optional<int64_t> Facts::upper(Term t) {
  if (t.length) {
    return Type::kMaxLength;
  }
  const Type *type = t.var && t.var->type ? t.var->type->nonOptional() : nullptr;
  if (type && type->isInteger() && type->intMax() <= uint64_t(kInf)) {
    return int64_t(type->intMax());
  }

  return std::nullopt;
}

void Facts::add(Term lhs, Term rhs, int64_t offset) {
  if (lhs == rhs || offset <= std::numeric_limits<int64_t>::min() + 1) {
    return;
  }
  for (Fact &fact : this->facts) {
    if (fact.lhs == lhs && fact.rhs == rhs) {
      fact.offset = std::min(fact.offset, offset);
      return;
    }
  }
  if (this->facts.size() == kMaxFacts) {
    this->facts.erase(this->facts.begin());
  }
  this->facts.push_back({lhs, rhs, offset});
}

void Facts::addEqual(Term a, Term b, int64_t offset) {
  add(a, b, Facts::saturatingAdd(offset, 1));
  add(b, a, Facts::saturatingSub(1, offset));
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
  std::vector<Term> nodes = this->boundNodes(lhs, rhs);
  std::vector<PartLimit> limits = this->partLimits(nodes);

  return this->shortestBound(nodes, lhs, rhs, &limits);
}

std::optional<int64_t> Facts::plainBound(Term lhs, Term rhs) const {
  if (lhs == rhs) {
    return 1;
  }
  std::vector<Term> nodes = this->boundNodes(lhs, rhs);

  return this->shortestBound(nodes, lhs, rhs, nullptr);
}

std::vector<Term> Facts::boundNodes(Term lhs, Term rhs) const {
  std::vector<Term> nodes{Term::zero()};
  for (const Fact &fact : this->facts) {
    Facts::addNode(nodes, fact.lhs);
    Facts::addNode(nodes, fact.rhs);
  }
  Facts::addNode(nodes, lhs);
  Facts::addNode(nodes, rhs);
  for (size_t i = 1; i < nodes.size(); ++i) {
    if (nodes[i].isSum()) {
      Term first = nodes[i].first();
      Term second = nodes[i].second();
      Facts::addNode(nodes, first);
      Facts::addNode(nodes, second);
    }
  }

  return nodes;
}

void Facts::addNode(std::vector<Term> &nodes, Term term) {
  if (!llvm::is_contained(nodes, term)) {
    nodes.push_back(term);
  }
}

size_t Facts::nodeIndex(const std::vector<Term> &nodes, Term term) {
  return size_t(llvm::find(nodes, term) - nodes.begin());
}

std::vector<Facts::PartLimit> Facts::partLimits(const std::vector<Term> &nodes) const {
  std::vector<PartLimit> limits;
  for (size_t i = 1; i < nodes.size(); ++i) {
    if (nodes[i].isSum()) {
      for (Term part : {nodes[i].second(), nodes[i].first()}) {
        std::optional<int64_t> above = this->plainBound(part, Term::zero());
        std::optional<int64_t> below = this->plainBound(Term::zero(), part);
        limits.push_back(PartLimit{part, above, below});
      }
    }
  }

  return limits;
}

std::optional<int64_t> Facts::shortestBound(const std::vector<Term> &nodes, Term lhs, Term rhs,
                                            const std::vector<PartLimit> *limits) const {
  std::vector<Edge> edges;
  for (const Fact &fact : this->facts) {
    edges.push_back(
        {Facts::nodeIndex(nodes, fact.lhs), Facts::nodeIndex(nodes, fact.rhs), fact.offset - 1});
  }
  size_t source = Facts::nodeIndex(nodes, lhs);
  size_t target = Facts::nodeIndex(nodes, rhs);
  for (size_t i = 1; i < nodes.size(); ++i) {
    if (!nodes[i].isSum()) {
      continue;
    }
    Term parts[2] = {nodes[i].first(), nodes[i].second()};
    for (int k = 0; k < 2; ++k) {
      Term other = parts[1 - k];
      size_t self = Facts::nodeIndex(nodes, parts[k]);
      std::optional<int64_t> hi = Facts::upper(other), lo = Facts::lower(other);
      if (limits != nullptr) {
        const PartLimit &limit = Facts::limitOf(*limits, other);
        if (limit.above) {
          hi = hi ? std::min(*hi, *limit.above - 1) : *limit.above - 1;
        }
        if (limit.below) {
          lo = lo ? std::max(*lo, 1 - *limit.below) : 1 - *limit.below;
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
  Facts::addZeroEdges(nodes, edges);

  return Facts::shortestDistance(nodes.size(), edges, source, target);
}

const Facts::PartLimit &Facts::limitOf(const std::vector<PartLimit> &limits, Term term) {
  return *llvm::find_if(limits, [&](const PartLimit &limit) { return limit.term == term; });
}

void Facts::addZeroEdges(const std::vector<Term> &nodes, std::vector<Edge> &edges) {
  for (size_t i = 1; i < nodes.size(); ++i) {
    std::optional<int64_t> hi, lo;
    if (nodes[i].isSum()) {
      hi = Facts::plus(Facts::upper(nodes[i].first()), Facts::upper(nodes[i].second()));
      lo = Facts::plus(Facts::lower(nodes[i].first()), Facts::lower(nodes[i].second()));
    } else {
      hi = Facts::upper(nodes[i]);
      lo = Facts::lower(nodes[i]);
    }
    if (hi) {
      edges.push_back({i, 0, *hi});
    }
    if (lo && *lo > -kInf) {
      edges.push_back({0, i, -*lo});
    }
  }
}

std::optional<int64_t> Facts::shortestDistance(size_t count, const std::vector<Edge> &edges,
                                               size_t source, size_t target) {
  std::vector<int64_t> dist(count, kInf);
  dist[source] = 0;
  for (size_t round = 0; round + 1 < count; ++round) {
    bool changed = false;
    for (const Edge &e : edges) {
      if (dist[e.from] == kInf) {
        continue;
      }
      std::optional<int64_t> sum = Facts::plus(dist[e.from], e.weight);
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
  this->facts.erase(std::remove_if(this->facts.begin(), this->facts.end(), pred),
                    this->facts.end());
}

void Facts::forget(const VarSymbol *var) {
  forgetIf([&](const Fact &f) { return f.lhs.mentions(var) || f.rhs.mentions(var); });
}

void Facts::shift(Term term, int64_t delta) {
  for (Fact &fact : this->facts) {
    bool lhs = fact.lhs.contains(term), rhs = fact.rhs.contains(term);
    if (lhs && !rhs) {
      fact.offset = Facts::saturatingAdd(fact.offset, delta);
    } else if (rhs && !lhs) {
      fact.offset = Facts::saturatingSub(fact.offset, delta);
    }
  }
  forgetIf([](const Fact &f) { return f.offset <= std::numeric_limits<int64_t>::min() + 1; });
}

Facts Facts::meet(const Facts &a, const Facts &b) {
  Facts result;
  for (const Fact &fact : a.facts) {
    if (b.proves(fact.lhs, fact.rhs, fact.offset)) {
      result.add(fact.lhs, fact.rhs, fact.offset);
    }
  }
  for (const Fact &fact : b.facts) {
    if (a.proves(fact.lhs, fact.rhs, fact.offset)) {
      result.add(fact.lhs, fact.rhs, fact.offset);
    }
  }

  return result;
}

}
