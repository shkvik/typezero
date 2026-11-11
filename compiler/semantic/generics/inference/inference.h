#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <set>

namespace tz {

enum class InferStepKind { PAIR, HELD, LIST, CANDIDATE };

struct InferPair {
  static constexpr InferStepKind kind = InferStepKind::PAIR;
  const Type *pattern;
  const Type *actual;
};

struct InferHeld {
  static constexpr InferStepKind kind = InferStepKind::HELD;
  const Type *pattern;
  const Type *actual;
};

struct InferList {
  static constexpr InferStepKind kind = InferStepKind::LIST;
  const Type *pattern;
  const Type *actual;
  const std::vector<const Type *> *bound;
};

struct AddCandidate {
  static constexpr InferStepKind kind = InferStepKind::CANDIDATE;
  size_t index;
  const Type *type;
};

using InferStep = Union<InferPair, InferHeld, InferList, AddCandidate>;

class Inference {
public:
  using TypeOf = std::function<const Type *(const Type *)>;

  Inference(llvm::ArrayRef<const Type *> typeParams, const AliasStore &aliases,
            TypeOf elementOfType);

  bool hasDeferred() const;
  void setAsyncOf(TypeOf asyncOfType);
  bool hasSeenUndefined(size_t k) const;
  void from(const Type *pattern, const Type *actual);
  void retry(const std::vector<const Type *> &bound);
  void fromHeld(const Type *pattern, const Type *actual);
  void adoptCandidates(size_t k, const Inference &other);
  void markArgument(bool weakArgument, bool intLiteralArgument);
  const std::vector<const Type *> &getCandidates(size_t k) const;
  const std::vector<const Type *> &getWeakCandidates(size_t k) const;
  const std::vector<const Type *> &getLiteralCandidates(size_t k) const;
  void matchList(const Type *pattern, const Type *actual, const std::vector<const Type *> *bound);

private:
  static const Type *paramsAsList(const Type *fn, size_t from);
  static const Type *listSlice(const Type *list, size_t begin, size_t end, bool last);

  void runInference(InferStep first);
  std::vector<InferStep> applyInferStep(const InferStep &step);
  std::vector<InferStep> inferPair(const Type *pattern, const Type *actual);
  std::optional<size_t> paramIndex(const Type *type) const;
  std::vector<InferStep> inferParam(size_t k, const Type *pattern, const Type *actual);
  std::vector<const Type *> &candidatesFor(size_t k);
  std::vector<InferStep> inferInside(const Type *pattern, const Type *actual);
  std::vector<InferStep> inferFunction(const Type *pattern, const Type *actual);
  static std::vector<InferStep> iteratorParts(const Type *pattern, const Type *actual);
  static std::vector<InferStep> inferDict(const Type *pattern, const Type *actual);
  static std::vector<InferStep> inferConstructor(const Type *pattern, const Type *actual);
  std::vector<InferStep> inferObject(const Type *pattern, const Type *actual);
  std::vector<InferStep> inferAsyncIterator(const Type *pattern, const Type *actual);
  static std::vector<InferStep> inferClassArgs(const Type *pattern, const ClassDecl *cls,
                                               const Type *actual);
  std::optional<std::vector<InferStep>> inferAliasArgs(const AliasOrigin *origin,
                                                       const Type *actual);
  static std::vector<InferStep> pairArgs(llvm::ArrayRef<const Type *> want,
                                         llvm::ArrayRef<const Type *> have);
  static std::vector<InferStep> inferFields(const Type *pattern, const Type *actual);
  std::vector<InferStep> inferUnion(const Type *pattern, const Type *actual);
  static std::vector<InferStep> inferGenericMember(llvm::ArrayRef<const Type *> open,
                                                   const Type *actual);
  static std::vector<const Type *> membersOutside(const Type *value,
                                                  llvm::ArrayRef<const Type *> fixed);
  std::vector<InferStep> inferHeld(const Type *pattern, const Type *actual);
  std::vector<InferStep> inferList(const Type *pattern, const Type *actual,
                                   const std::vector<const Type *> *bound);

  llvm::ArrayRef<const Type *> params;
  std::vector<std::vector<const Type *>> candidates, weakCandidates, literalCandidates;
  std::vector<bool> undefinedSeen;
  std::set<std::pair<const Type *, const Type *>> visited;
  bool weak = false;
  bool intLiteral = false;
  const AliasStore &origins;
  TypeOf elementOf;
  TypeOf asyncOf;
  std::vector<std::pair<const Type *, const Type *>> deferred;
};

}
