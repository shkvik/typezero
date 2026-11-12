#include "compiler/semantic/flow/facts/facts.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/STLExtras.h"
#include <algorithm>
#include <functional>
#include <limits>

namespace tz {

namespace {

constexpr size_t kMaxFacts = 64;
constexpr int64_t kInf = std::numeric_limits<int64_t>::max();
constexpr int64_t kLowest = std::numeric_limits<int64_t>::min();
constexpr size_t kZeroNode = 0;

}

std::pair<uintptr_t, bool> Term::sortKey() const {
  return std::pair(reinterpret_cast<uintptr_t>(this->var), this->length);
}

std::optional<Term> Term::sum(Term left, Term right) {
  if (left.isZero() || right.isZero() || left.isSum() || right.isSum()) {
    return std::nullopt;
  }
  bool isSwapped = right.sortKey() < left.sortKey();
  const Term &low = isSwapped ? right : left;
  const Term &high = isSwapped ? left : right;

  return Term{.var = low.var, .length = low.length, .var2 = high.var, .length2 = high.length};
}

int64_t Facts::saturatingAdd(int64_t left, int64_t right) {
  if (right > 0 && left > INT64_MAX - right) {
    return INT64_MAX;
  }
  if (right < 0 && left < INT64_MIN - right) {
    return INT64_MIN;
  }

  return left + right;
}

int64_t Facts::saturatingSub(int64_t left, int64_t right) {
  if (right < 0 && left > INT64_MAX + right) {
    return INT64_MAX;
  }
  if (right > 0 && left < INT64_MIN + right) {
    return INT64_MIN;
  }

  return left - right;
}

const Type *Facts::integerType(Term term) const {
  if (term.var == nullptr || term.var->type == nullptr) {
    return nullptr;
  }
  const Type *type = term.var->type->nonOptional();

  return type->isInteger() ? type : nullptr;
}

std::optional<int64_t> Facts::lower(Term term) const {
  if (term.length) {
    return 0;
  }
  const auto *type = this->integerType(term);
  if (type == nullptr || type->intMin() <= kLowest) {
    return std::nullopt;
  }

  return type->intMin();
}

std::optional<int64_t> Facts::upper(Term term) const {
  if (term.length) {
    return Type::kMaxLength;
  }
  const auto *type = this->integerType(term);
  if (type == nullptr || type->intMax() > uint64_t(kInf)) {
    return std::nullopt;
  }

  return int64_t(type->intMax());
}

std::optional<int64_t> Facts::plus(std::optional<int64_t> left,
                                   std::optional<int64_t> right) const {
  if (!left.has_value() || !right.has_value()) {
    return std::nullopt;
  }
  bool isTooHigh = *right > 0 && *left > kInf - 1 - *right;
  bool isTooLow = *right < 0 && *left < -kInf + 1 - *right;
  if (isTooHigh || isTooLow) {
    return std::nullopt;
  }

  return *left + *right;
}

void Facts::add(Term lhs, Term rhs, int64_t offset) {
  if (lhs == rhs || offset <= kLowest + 1) {
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
  this->facts.push_back(Fact{.lhs = lhs, .rhs = rhs, .offset = offset});
}

void Facts::addEqual(Term left, Term right, int64_t offset) {
  int64_t above = Facts::saturatingAdd(offset, 1);
  int64_t below = Facts::saturatingSub(1, offset);
  this->add(left, right, above);
  this->add(right, left, below);
}

bool Facts::canProve(Term lhs, Term rhs, int64_t offset) const {
  if (lhs == rhs) {
    return offset >= 1;
  }
  auto least = this->bound(lhs, rhs);

  return least.has_value() && *least <= offset;
}

std::optional<int64_t> Facts::bound(Term lhs, Term rhs) const {
  if (lhs == rhs) {
    return 1;
  }
  auto nodes = this->boundNodes(lhs, rhs);
  auto limits = this->partLimits(nodes);

  return this->shortestBound(nodes, lhs, rhs, &limits);
}

std::optional<int64_t> Facts::plainBound(Term lhs, Term rhs) const {
  if (lhs == rhs) {
    return 1;
  }
  auto nodes = this->boundNodes(lhs, rhs);

  return this->shortestBound(nodes, lhs, rhs, nullptr);
}

std::vector<Term> Facts::boundNodes(Term lhs, Term rhs) const {
  auto nodes = std::vector<Term>{Term::zero()};
  for (const Fact &fact : this->facts) {
    this->addNode(nodes, fact.lhs);
    this->addNode(nodes, fact.rhs);
  }
  this->addNode(nodes, lhs);
  this->addNode(nodes, rhs);
  for (size_t index = 1; index < nodes.size(); ++index) {
    if (nodes[index].isSum()) {
      Term first = nodes[index].first();
      Term second = nodes[index].second();
      this->addNode(nodes, first);
      this->addNode(nodes, second);
    }
  }

  return nodes;
}

void Facts::addNode(std::vector<Term> &nodes, Term term) const {
  if (!llvm::is_contained(nodes, term)) {
    nodes.push_back(term);
  }
}

size_t Facts::nodeIndex(const std::vector<Term> &nodes, Term term) const {
  return size_t(llvm::find(nodes, term) - nodes.begin());
}

std::vector<Facts::PartLimit> Facts::partLimits(const std::vector<Term> &nodes) const {
  auto limits = std::vector<PartLimit>();
  for (size_t index = 1; index < nodes.size(); ++index) {
    if (nodes[index].isSum()) {
      this->addPartLimit(limits, nodes[index].second());
      this->addPartLimit(limits, nodes[index].first());
    }
  }

  return limits;
}

void Facts::addPartLimit(std::vector<PartLimit> &limits, Term part) const {
  auto above = this->plainBound(part, Term::zero());
  auto below = this->plainBound(Term::zero(), part);
  limits.push_back(PartLimit{.term = part, .above = above, .below = below});
}

std::vector<Facts::Edge> Facts::factEdges(const std::vector<Term> &nodes) const {
  auto edges = std::vector<Edge>();
  for (const Fact &fact : this->facts) {
    size_t from = this->nodeIndex(nodes, fact.lhs);
    size_t to = this->nodeIndex(nodes, fact.rhs);
    edges.push_back(Edge{.from = from, .to = to, .weight = fact.offset - 1});
  }

  return edges;
}

std::optional<int64_t> Facts::shortestBound(const std::vector<Term> &nodes, Term lhs, Term rhs,
                                            const std::vector<PartLimit> *limits) const {
  auto edges = this->factEdges(nodes);
  size_t source = this->nodeIndex(nodes, lhs);
  size_t target = this->nodeIndex(nodes, rhs);
  for (size_t index = 1; index < nodes.size(); ++index) {
    if (nodes[index].isSum()) {
      this->addSumEdges(nodes, index, limits, edges);
    }
  }
  this->addZeroEdges(nodes, edges);

  return this->shortestDistance(nodes.size(), edges, source, target);
}

void Facts::addSumEdges(const std::vector<Term> &nodes, size_t sumIndex,
                        const std::vector<PartLimit> *limits, std::vector<Edge> &edges) const {
  Term first = nodes[sumIndex].first();
  Term second = nodes[sumIndex].second();
  this->addPartEdges(nodes, sumIndex, SumParts{.part = first, .other = second}, limits, edges);
  this->addPartEdges(nodes, sumIndex, SumParts{.part = second, .other = first}, limits, edges);
}

void Facts::addPartEdges(const std::vector<Term> &nodes, size_t sumIndex, const SumParts &parts,
                         const std::vector<PartLimit> *limits, std::vector<Edge> &edges) const {
  size_t self = this->nodeIndex(nodes, parts.part);
  auto high = this->limitedUpper(parts.other, limits);
  auto low = this->limitedLower(parts.other, limits);
  if (high.has_value()) {
    edges.push_back(Edge{.from = sumIndex, .to = self, .weight = *high});
  }
  if (low.has_value() && *low > -kInf) {
    edges.push_back(Edge{.from = self, .to = sumIndex, .weight = -*low});
  }
}

std::optional<int64_t> Facts::limitedUpper(Term other, const std::vector<PartLimit> *limits) const {
  auto high = this->upper(other);
  if (limits == nullptr) {
    return high;
  }
  const PartLimit &limit = this->limitOf(*limits, other);
  if (!limit.above.has_value()) {
    return high;
  }
  int64_t known = *limit.above - 1;

  return high.has_value() ? std::min(*high, known) : known;
}

std::optional<int64_t> Facts::limitedLower(Term other, const std::vector<PartLimit> *limits) const {
  auto low = this->lower(other);
  if (limits == nullptr) {
    return low;
  }
  const PartLimit &limit = this->limitOf(*limits, other);
  if (!limit.below.has_value()) {
    return low;
  }
  int64_t known = 1 - *limit.below;

  return low.has_value() ? std::max(*low, known) : known;
}

bool Facts::isLimitOf(Term term, const PartLimit &limit) const {
  return limit.term == term;
}

const Facts::PartLimit &Facts::limitOf(const std::vector<PartLimit> &limits, Term term) const {
  return *llvm::find_if(limits, std::bind_front(&Facts::isLimitOf, this, term));
}

std::optional<int64_t> Facts::nodeUpper(Term node) const {
  if (!node.isSum()) {
    return this->upper(node);
  }
  auto first = this->upper(node.first());
  auto second = this->upper(node.second());

  return this->plus(first, second);
}

std::optional<int64_t> Facts::nodeLower(Term node) const {
  if (!node.isSum()) {
    return this->lower(node);
  }
  auto first = this->lower(node.first());
  auto second = this->lower(node.second());

  return this->plus(first, second);
}

void Facts::addZeroEdges(const std::vector<Term> &nodes, std::vector<Edge> &edges) const {
  for (size_t index = 1; index < nodes.size(); ++index) {
    auto high = this->nodeUpper(nodes[index]);
    auto low = this->nodeLower(nodes[index]);
    if (high.has_value()) {
      edges.push_back(Edge{.from = index, .to = kZeroNode, .weight = *high});
    }
    if (low.has_value() && *low > -kInf) {
      edges.push_back(Edge{.from = kZeroNode, .to = index, .weight = -*low});
    }
  }
}

std::optional<int64_t> Facts::shortestDistance(size_t count, const std::vector<Edge> &edges,
                                               size_t source, size_t target) const {
  auto distances = std::vector<int64_t>(count, kInf);
  distances[source] = 0;
  bool hasChanged = true;
  for (size_t round = 0; round + 1 < count && hasChanged; ++round) {
    hasChanged = this->relaxEdges(edges, distances);
  }
  if (distances[target] == kInf) {
    return std::nullopt;
  }

  return distances[target] + 1;
}

bool Facts::relaxEdges(const std::vector<Edge> &edges, std::vector<int64_t> &distances) const {
  bool hasChanged = false;
  for (const Edge &edge : edges) {
    bool isRelaxed = this->relaxEdge(edge, distances);
    hasChanged = hasChanged || isRelaxed;
  }

  return hasChanged;
}

bool Facts::relaxEdge(const Edge &edge, std::vector<int64_t> &distances) const {
  if (distances[edge.from] == kInf) {
    return false;
  }
  auto sum = this->plus(distances[edge.from], edge.weight);
  if (!sum.has_value() || *sum >= distances[edge.to]) {
    return false;
  }
  distances[edge.to] = *sum;

  return true;
}

void Facts::forgetIf(llvm::function_ref<bool(const Fact &)> isForgotten) {
  llvm::erase_if(this->facts, isForgotten);
}

bool Facts::isMentioning(const VarSymbol *variable, const Fact &fact) const {
  return fact.lhs.mentions(variable) || fact.rhs.mentions(variable);
}

void Facts::forget(const VarSymbol *variable) {
  auto isMentioning = std::bind_front(&Facts::isMentioning, this, variable);
  this->forgetIf(isMentioning);
}

bool Facts::isVanishing(const Fact &fact) const {
  return fact.offset <= kLowest + 1;
}

void Facts::shift(Term term, int64_t delta) {
  for (Fact &fact : this->facts) {
    bool isLeft = fact.lhs.contains(term);
    bool isRight = fact.rhs.contains(term);
    if (isLeft && !isRight) {
      fact.offset = Facts::saturatingAdd(fact.offset, delta);
    } else if (isRight && !isLeft) {
      fact.offset = Facts::saturatingSub(fact.offset, delta);
    }
  }
  auto isVanishing = std::bind_front(&Facts::isVanishing, this);
  this->forgetIf(isVanishing);
}

Facts Facts::meet(const Facts &other) const {
  auto result = Facts();
  for (const Fact &fact : this->facts) {
    if (other.canProve(fact.lhs, fact.rhs, fact.offset)) {
      result.add(fact.lhs, fact.rhs, fact.offset);
    }
  }
  for (const Fact &fact : other.facts) {
    if (this->canProve(fact.lhs, fact.rhs, fact.offset)) {
      result.add(fact.lhs, fact.rhs, fact.offset);
    }
  }

  return result;
}

}
